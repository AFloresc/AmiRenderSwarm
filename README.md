# AmiRenderSwarm — Granja de Renderizado Cooperativo Distribuido para AmigaOS

**AmiRenderSwarm** es una arquitectura de renderizado distribuido orientada a equipos **Amiga** (68k clásicos, acelerados como 68060/Vampire 68080 o PPC con AmigaOS 3.x/4.x) coordinados mediante un servidor moderno ligero en **Go 1.26** que actúa exclusivamente como **punto de encuentro (*rendezvous / signaling*)**.

El objetivo es permitir que múltiples ordenadores Amiga compartan potencia de cálculo de forma cooperativa sin saturar el servidor central con archivos pesados ni exigir configuraciones de red complejas en las máquinas retro.

---

## 1. Arquitectura General

El sistema separa completamente el **plano de control (señalización)** del **plano de datos (transferencia de escenas y fotogramas)**:

```text
                  +-----------------------------------+
                  |   Servidor Rendezvous (Go 1.26)   |
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
                        |  (Escena / Frames)    |
                        +-----------------------+
```

### Componentes principales

1. **Punto de Encuentro / Rendezvous Server (Go 1.26):**
   - Corre en una máquina moderna (VPS, contenedor o servidor doméstico) en el puerto TCP `:7900` por defecto.
   - Mantiene en memoria el registro de agentes conectados (`Registry`), sus capacidades hardware y su estado (`IDLE`, `BUSY`), además de limpiar nodos inactivos mediante barridos periódicos (`SweepStale`).
   - Media las invitaciones entre nodos (`INVITE` / `INVITE_REQ` / `INVITE_OK` / `INVITE_NO` / `INVITED`) para evitar colisiones.
   - **No almacena ni transfiere escenas ni fotogramas** en flujo normal.
2. **Agente AmigaOS (C + `bsdsocket.library`):**
   - Ejecutable único ligero (`AmiRenderSwarm`) capaz de operar en modo **Worker** (escuchando conexiones directas en `listen_port`, por defecto `7800`, y ejecutando el render en `T:`) y en modo **Director** (cuando el usuario lanza un trabajo).
   - Utiliza `WaitSelect()` para esperar simultáneamente actividad en el socket de señalización, latidos periódicos y señales de consola (`SIGBREAKF_CTRL_C`).

---

## 2. Por qué Go 1.26 + Protocolo TCP Plano para AmigaOS

- **En el servidor moderno (Go 1.26):** Concurrencia eficiente mediante *goroutines* por conexión (lectora y escritora desacopladas), cancelación limpia con `signal.NotifyContext`, *timeouts* configurables (`-timeout`) y despliegue como binario estático único.
- **En el cliente (AmigaOS):** Evita el coste de CPU y memoria de *handshakes* WebSocket, *parsers* JSON o librerías criptográficas pesadas directamente en el 68k. La señalización utiliza texto plano terminado en `\n`, fácil de parsear en C con *buffers* fijos y depurable manualmente con clientes TCP en consola o PowerShell.

---

## 3. Protocolo de Señalización (Servidor Go ↔ Agente Amiga)

Transporte: **TCP persistente** con mensajes de una línea terminados en `\n` (`\r\n` normalizado a `\n`). Formato clave-valor separado por espacios (`VERB key=val key=val`).

### A. Registro y bienvenida

Al conectar, el agente envía sus capacidades y puertos:

```text
HELLO id=amiga_v4_01 os=3.2 cpu=68080 fpu=1 ram_mb=512 engines=lwsn,povray role=both listen_port=7800
```

- `role`: `worker` (solo ejecuta renders), `director` (solo envía trabajos) o `both` (ambos).
- `listen_port`: puerto TCP local donde el agente acepta conexiones directas de otros pares.

El servidor responde confirmando la sesión y devolviendo la IP visible detectada:

```text
WELCOME peer_ip=192.168.1.45 session=s_98a12
```

### B. Latido de vida (*Heartbeat*) y cambio de estado

Periódicamente el agente reporta que sigue activo para evitar ser expulsado por *timeout*:

```text
PING state=IDLE load=0.12
```

Respuesta del servidor:

```text
PONG ts=1795116480
```

Para actualizar explícitamente el estado del nodo (`IDLE` / `BUSY`):

```text
STATE state=BUSY
```

Respuesta del servidor:

```text
OK state=BUSY
```

### C. Descubrimiento de nodos disponibles

El agente que va a lanzar un render solicita nodos libres (`IDLE`) filtrando por motor (`engine`) o memoria mínima (`min_ram`), excluyendo siempre al propio solicitante:

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
3. **Worker acepta (o rechaza con `INVITE_NO to=amiga_director job=anim_01 reason=busy`):**
   ```text
   INVITE_OK to=amiga_director job=anim_01
   ```
4. **Servidor marca al Worker como `BUSY` y confirma al Director:**
   ```text
   INVITED target=vampire_bcn ip=10.0.0.25 port=7800 job=anim_01
   ```

---

## 4. Protocolo de Datos entre Pares (Director ↔ Worker)

Una vez completado el emparejamiento, la transferencia de datos ocurre directamente entre los pares mediante un socket TCP binario con una **cabecera fija de 16 bytes** en *network byte order* (**Big-Endian**, nativo en arquitectura Motorola 68k):

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Magic ("AMIR")                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     type      |     flags     |           reserved            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                             frame                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          payload_len                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Payload (payload_len bytes)               |
```

### Campos de la cabecera (`AmirsPacketHeader`)

- **`magic` (`uint32`)**: `0x414D4952` (`"AMIR"`) para validación de trama.
- **`type` (`uint8`)**:
  - `1` (`AMIRS_MSG_JOB_INIT`): metadatos / parámetros del trabajo (`job=...;engine=...;frame=...`).
  - `2` (`AMIRS_MSG_SCENE_CHUNK`): paquete o fragmento de escena enviado al worker para renderizar el `frame`.
  - `3` (`AMIRS_MSG_RENDER_DONE`): notificación de renderizado completado para el `frame`.
  - `4` (`AMIRS_MSG_FRAME_DATA`): bytes del fotograma renderizado devuelto al director.
  - `5` (`AMIRS_MSG_ERR`): reporte de error en el nodo (`payload_too_large`, `write_scene_failed`, `render_failed`, etc.).
- **`flags` (`uint8`)**: *bitmask* de opciones (`0` por defecto).
- **`reserved` (`uint16`)**: reservado para alineación y futuras extensiones (`0x0000`).
- **`frame` (`uint32`)**: número de fotograma asociado al mensaje en orden de red (`htonl` / `ntohl`).
- **`payload_len` (`uint32`)**: tamaño exacto en bytes del *payload* que sigue a la cabecera en orden de red (`htonl` / `ntohl`).

---

## 5. Estrategia de Reparto y Tolerancia a Fallos

1. **Cola dinámica (*Pull* por velocidad real):**
   - Dado que las capacidades entre distintos modelos Amiga varían enormemente (por ejemplo, un 68030 frente a un 68080), los fotogramas no se reparten de forma estática al inicio.
   - El Director mantiene la cola de fotogramas pendientes (`1..N`) y entrega el siguiente fotograma libre a cada Worker conforme este devuelve el anterior.
2. **Aislamiento y ejecución en AmigaOS:**
   - Los archivos temporales de escena y salida se escriben en `T:` (`T:amirs_scene_<frame>.dat` y `T:amirs_out_<frame>.dat`) y se eliminan con `DeleteFile()` tras procesarlos.
   - El comando de render se invoca mediante `SystemTagList()`, permitiendo ajustar prioridades con `SetTaskPri` para mantener fluida la interfaz de Workbench.
3. **Recuperación ante desconexiones:**
   - Si un Worker pierde conexión o supera el tiempo máximo sin entregar su fotograma asignado, el Director devuelve ese `frame` a la cola pendiente para que otro nodo lo procese.
4. **Conectividad tras NAT:**
   - En LAN o redes *overlay* (WireGuard / Tailscale), los nodos conectan directamente a `ip:port`.
   - Si ambos nodos están tras routers domésticos sin apertura de puertos, el servidor en Go puede ofrecer opcionalmente un *relay* TCP por *token* de sesión manteniendo intacto el protocolo binario de 16 bytes en el cliente Amiga.

---

## 6. Estructura del Repositorio

```text
AmiRenderSwarm/
├── README.md
├── hub/                    # Servidor de señalización en Go 1.26
│   ├── go.mod
│   ├── main.go             # Listener TCP, goroutines por conexión y manejo de señales
│   ├── registry.go         # Registro concurrente de agentes, estados y limpieza por timeout
│   └── protocol.go         # Parser y formateador de comandos línea a línea
└── amiga/                  # Agente en C para AmigaOS (bsdsocket.library)
    ├── Makefile            # Cross-compilación con m68k-amigaos-gcc (-noixemul)
    ├── main.c              # Conexión al hub, WaitSelect(), PINGs y gestión de eventos
    ├── net.h / net.c         # Inicialización de bsdsocket.library y lectura/escritura de líneas
    ├── proto.h / proto.c     # Cabecera binaria de 16 bytes ("AMIR") y envío/recepción exacta
    └── worker.h / worker.c   # Listener directo, ejecución de render y devolución de frames
```

---

## 7. Puesta en Marcha y Pruebas

### Arrancar el Hub en Go 1.26

```bash
cd hub
go run . -addr :7900 -timeout 60s
```

*(Para pruebas manuales prolongadas sin desconexiones por inactividad, puede pasarse por ejemplo `-timeout 10m`).*

### Compilar el agente para AmigaOS

```bash
cd amiga
make
```