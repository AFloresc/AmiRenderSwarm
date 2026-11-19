# AmigaRender — Granja de Renderizado Cooperativo Distribuido para AmigaOS

**AmigaRender** es una arquitectura de renderizado distribuido orientada a equipos **Amiga** (68k clásicos, acelerados como 68060/Vampire 68080 o PPC con AmigaOS 3.x/4.x) coordinados mediante un servidor moderno ligero en **Go** que actúa exclusivamente como **punto de encuentro (*rendezvous / signaling*)**.

El objetivo es permitir que múltiples ordenadores Amiga compartan potencia de cálculo de forma cooperativa sin saturar el servidor central con archivos pesados ni exigir configuraciones de red complejas en las máquinas retro.

---

## 1. Arquitectura General

El sistema separa completamente el **plano de control (señalización)** del **plano de datos (transferencia de escenas y fotogramas)**:

```text
                  +-----------------------------------+
                  |   Servidor Rendezvous (Go)        |
                  |   - Presencia y Heartbeats        |
                  |   - Inventario (CPU/RAM/Motores)  |
                  |   - Arbitraje y Matchmaking       |
                  +-----------------+-----------------+
                         ^                     ^
        TCP ASCII (\n)   |                     |   TCP ASCII (\n)
       (Señalización)    |                     |  (Señalización)
                         v                     v
              +--------------------+  +--------------------+
              |   Agente Director  |  |   Agente Worker    |
              |   (AmigaOS / C)    |  |   (AmigaOS / C)    |
              +---------+----------+  +---------+----------+
                        |                       ^
                        |   TCP Binario Directo |
                        |  (Escena LHA / Frames)|
                        +-----------------------+
```

### Componentes principales

1. **Punto de Encuentro / Rendezvous Server (Go):**
   * Corre en una máquina moderna (VPS, contenedor o servidor doméstico).
   * Mantiene en memoria el registro de agentes conectados, sus capacidades hardware y su estado (`IDLE`, `BUSY`).
   * Media las invitaciones entre nodos para evitar colisiones.
   * **No almacena ni transfiere escenas ni fotogramas** en flujo normal.
2. **Agente AmigaOS (C + `bsdsocket.library`):**
   * Ejecutable único ligero capaz de operar en modo **Worker** (en segundo plano con baja prioridad mediante `SetTaskPri`) y en modo **Director** (cuando el usuario lanza un trabajo).
   * Empaqueta y desempaqueta escenas en `RAM:` o `T:` y ejecuta el motor CLI configurado (LightWave ScreamerNet `LWSN`, POV-Ray, Imagine, Real 3D, etc.).

---

## 2. Por qué Go + Protocolo TCP Plano para AmigaOS

* **En el servidor moderno (Go):** Concurrencia eficiente mediante *goroutines* por conexión, bajo consumo de memoria, gestión nativa de *timeouts* y despliegue como binario estático único.
* **En el cliente (AmigaOS):** Evita el coste de CPU y memoria de *handshakes* WebSocket, *parsers* JSON o librerías criptográficas pesadas directamente en el 68k. La señalización utiliza texto plano terminado en `\n`, fácil de parsear en C con buffers fijos y depurable con herramientas estándar.

---

## 3. Protocolo de Señalización (Servidor Go ↔ Agente Amiga)

Transporte: **TCP persistente** con mensajes de una línea terminados en `\n` (`\r\n` normalizado a `\n`). Formato clave-valor separado por espacios (`VERB key=val key=val`).

### A. Registro y bienvenida

Al conectar, el agente envía sus capacidades y puertos:

```text
HELLO id=amiga_v4_01 os=3.2 cpu=68080 fpu=1 ram_mb=512 engines=lwsn,povray role=both listen_port=7800
```

* `role`: `worker` (solo ejecuta renders), `director` (solo envía trabajos) o `both` (ambos).
* `listen_port`: puerto TCP local donde el agente acepta conexiones directas de otros pares.

El servidor responde confirmando la sesión y devolviendo la IP pública/visible detectada:

```text
WELCOME peer_ip=192.168.1.45 session=s_98a12
```

### B. Latido de vida (*Heartbeat*) y cambio de estado

Cada 15–20 segundos el agente reporta que sigue activo:

```text
PING state=IDLE load=0.12
```

Respuesta del servidor:

```text
PONG ts=1795116480
```

Si el usuario necesita pausar su disponibilidad inmediatamente (por ejemplo, al abrir una aplicación pesada):

```text
STATE state=BUSY
```

### C. Descubrimiento de nodos disponibles

El agente que va a lanzar un render solicita nodos libres filtrando por motor o memoria mínima:

```text
LIST_PEERS engine=lwsn min_ram=64
```

El servidor devuelve una línea por cada agente disponible y cierra el listado:

```text
PEER id=amiga_060 ip=10.0.0.12 port=7800 cpu=68060 ram_mb=128 engines=lwsn,povray
PEER id=vampire_bcn ip=10.0.0.25 port=7800 cpu=68080 ram_mb=512 engines=lwsn
PEERS_END count=2
```

### D. Reserva y emparejamiento (*Matchmaking*)

Para evitar que dos directores asignen trabajo simultáneamente al mismo worker, el servidor actúa como árbitro:

1. **Director solicita reserva:**
   ```text
   INVITE target=vampire_bcn job=anim_01 engine=lwsn frames=120
   ```
2. **Servidor reenvía petición al Worker:**
   ```text
   INVITE_REQ from=amiga_director from_ip=10.0.0.5 from_port=7801 job=anim_01 engine=lwsn frames=120
   ```
3. **Worker acepta (o rechaza con `INVITE_NO job=anim_01 reason=busy`):**
   ```text
   INVITE_OK job=anim_01
   ```
4. **Servidor marca al Worker como `BUSY` y confirma al Director:**
   ```text
   INVITED target=vampire_bcn ip=10.0.0.25 port=7800 job=anim_01
   ```

---

## 4. Protocolo de Datos entre Pares (Director ↔ Worker)

Una vez completado el emparejamiento, la transferencia de archivos ocurre directamente entre los ordenadores Amiga mediante un socket TCP binario con una **cabecera fija de 16 bytes** en *network byte order* (**Big-Endian**, nativo en arquitectura Motorola 68k):

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Magic ("AMIR")                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   msg_type    |     flags     |           reserved            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           frame_num                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          payload_len                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Payload (payload_len bytes)               |
```

### Campos de la cabecera

* **`magic` (`uint32`)**: `0x414D4952` (`"AMIR"`) para validación rápida de trama.
* **`msg_type` (`uint8`)**:
  * `1`: Envío de paquete de escena (ej. archivo `.lha` con escena, objetos y texturas).
  * `2`: Orden de renderizar un fotograma concreto (`frame_num`).
  * `3`: Entrega de fotograma renderizado (ej. archivo `.iff` / `.tga` en el *payload*).
  * `4`: Fin de trabajo o abortar sesión.
* **`flags` (`uint8`)**: Bitmask de opciones (ej. bit `0` = *payload* comprimido con LHA).
* **`reserved` (`uint16`)**: Reservado para alineación y futuras extensiones (`0x0000`).
* **`frame_num` (`uint32`)**: Número de fotograma asociado al mensaje (`0` si aplica a toda la escena).
* **`payload_len` (`uint32`)**: Tamaño exacto en bytes de los datos que siguen a la cabecera.

---

## 5. Estrategia de Reparto y Tolerancia a Fallos

1. **Cola dinámica (*Pull* por velocidad real):**
   * Dado que las capacidades entre distintos modelos Amiga varían enormemente (por ejemplo, un 68030 frente a un 68080), los fotogramas no se reparten de forma estática al inicio.
   * El Director mantiene la cola de fotogramas pendientes (`1..N`) y entrega el siguiente fotograma libre a cada Worker conforme este devuelve el anterior.
2. **Aislamiento y prioridad en AmigaOS:**
   * Los archivos temporales se escriben en un subdirectorio aislado dentro de `RAM:` o `T:` y se eliminan al finalizar el trabajo (`msg_type=4`).
   * El proceso de renderizado se lanza con prioridad negativa (`SetTaskPri` entre `-1` y `-5`) para mantener fluida la interfaz de Workbench del usuario anfitrión.
3. **Recuperación ante desconexiones:**
   * Si un Worker pierde conexión o supera el tiempo máximo sin entregar su fotograma asignado, el Director devuelve ese `frame_num` a la cola pendiente para que otro nodo lo procese.
4. **Conectividad tras NAT:**
   * En LAN o redes *overlay* (WireGuard / Tailscale), los nodos conectan directamente a `ip:port`.
   * Si ambos nodos están tras routers domésticos sin apertura de puertos, el servidor en Go puede ofrecer opcionalmente un *relay* TCP por *token* de sesión manteniendo intacto el protocolo binario de 16 bytes en el cliente Amiga.

---

## 6. Estructura Sugerida del Repositorio

```text
AmiRenderSwarm/
├── README.md
├── hub/                  # Servidor de señalización en Go
│   ├── go.mod
│   ├── main.go
│   ├── registry.go       # Gestión de nodos, estados (IDLE/BUSY) y heartbeats
│   └── protocol.go       # Parser y formateador de comandos de texto línea a línea
└── amiga/                # Agente en C para AmigaOS (bsdsocket.library)
    ├── Makefile
    ├── main.c            # Bucle principal, conexión al hub y comandos
    ├── net.c / net.h       # Socket TCP, envío/recepción por líneas y cabecera binaria
    └── worker.c / worker.h # Ejecución del motor de render y devolución de frames
```
