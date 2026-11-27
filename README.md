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
   - Incorpora un **subsistema de registro** (`amiga/log.c`) que vuelca toda la ejecución en la consola con marca de tiempo, nivel y componente de origen. Véase la [sección 8](#8-registro-logging-del-agente).

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
HELLO id=amiga_v4_01 os=3.2 cpu=68080 fpu=1 ram_mb=256 engines=lwsn,povray role=both listen_port=7800
```

- `role`: `worker` (solo ejecuta renders), `director` (solo envía trabajos) o `both` (ambos).
- `listen_port`: puerto TCP local donde el agente acepta conexiones directas de otros pares.

> El agente anuncia hoy valores fijos (`os=3.2 cpu=68080 fpu=1 ram_mb=256 engines=lwsn,povray role=both`); sólo `id`, `listen_port` y las opciones de la línea de órdenes son configurables.

El servidor responde confirmando la sesión y devolviendo la IP visible detectada:

```text
WELCOME peer_ip=192.168.1.45 session=s_98a12
```

### B. Latido de vida (*Heartbeat*) y cambio de estado

Periódicamente el agente reporta que sigue activo para evitar ser expulsado por *timeout*:

```text
PING state=IDLE load=0.05
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

> **Estado de implementación.** El flujo actualmente operativo es el siguiente:
> el hub reparte invitaciones, el worker acepta, el director envía **una**
> escena de ejemplo para el frame `1`, el worker la renderiza y devuelve el
> frame. Los puntos 1, 3 y 4 de esta sección, así como el ajuste de prioridad
> del punto 2, están **diseñados pero todavía no implementados**. El código
> manda sobre este documento.

1. **Cola dinámica (*Pull* por velocidad real):** *(previsto)*
   - Dado que las capacidades entre distintos modelos Amiga varían enormemente (por ejemplo, un 68030 frente a un 68080), los fotogramas no se reparten de forma estática al inicio.
   - El Director mantiene la cola de fotogramas pendientes (`1..N`) y entrega el siguiente fotograma libre a cada Worker conforme este devuelve el anterior.
2. **Aislamiento y ejecución en AmigaOS:** *(implementado)*
   - Los archivos temporales de escena y salida se escriben en `T:` (`T:amirs_scene_<frame>.dat` y `T:amirs_out_<frame>.dat`) y se eliminan con `DeleteFile()` tras procesarlos.
   - El comando de render se invoca mediante `SystemTagList()`. El ajuste de prioridad con `SetTaskPri` para mantener fluida la interfaz de Workbench está **previsto, no implementado**.
3. **Recuperación ante desconexiones:** *(previsto)*
   - Si un Worker pierde conexión o supera el tiempo máximo sin entregar su fotograma asignado, el Director devuelve ese `frame` a la cola pendiente para que otro nodo lo procese.
4. **Conectividad tras NAT:** *(previsto)*
   - En LAN o redes *overlay* (WireGuard / Tailscale), los nodos conectan directamente a `ip:port`.
   - Si ambos nodos están tras routers domésticos sin apertura de puertos, el servidor en Go puede ofrecer opcionalmente un *relay* TCP por *token* de sesión manteniendo intacto el protocolo binario de 16 bytes en el cliente Amiga.

---

## 6. Estructura del Repositorio

```text
AmiRenderSwarm/
├── README.md
├── AGENTS.md               # Guía de trabajo del repositorio (build, gotchas y protocolo)
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
    ├── log.h / log.c         # Subsistema de log con timestamp, nivel y etiqueta de componente
    └── worker.h / worker.c   # Listener directo, ejecución de render y devolución de frames
```

---

## 7. Puesta en Marcha y Pruebas

### Requisitos del agente en el Amiga

- **AmigaOS 3.2 o superior** con `68020` como mínimo (el binario se compila con
  `-m68020`; un 68060 o un Vampire 68080 funcionan perfectamente).
- Las bibliotecas **`bsdsocket.library`** (Roadshow / AmiTCP) y **`dos.library`**
  deben estar disponibles. **No** se necesita ninguna otra biblioteca en tiempo
  de ejecución.
- Un volumen **`T:`** escribible, donde el worker crea los ficheros temporales
  `amirs_scene_<frame>.dat` y `amirs_out_<frame>.dat` y donde el director
  guarda los frames recibidos como `amirs_recv_<frame>.dat`.

### Arrancar el Hub en Go 1.26

El módulo Go vive en `hub/`; los comandos `go` **no** deben ejecutarse desde la
raíz del repositorio.

```bash
cd hub
go build ./...        # genera hub.exe
go run . -addr :7900 -timeout 60s
```

*(Para pruebas manuales prolongadas sin desconexiones por inactividad, puede pasarse por ejemplo `-timeout 10m`).*

### Compilar el agente para AmigaOS

En una máquina de desarrollo sin toolchain AmigaOS nativo, la vía recomendada es
el contenedor oficial de cross-compilación, ejecutado **desde la raíz del
repositorio**:

```bash
docker run --rm -v "${PWD}:/work" -w /work/amiga amigadev/crosstools:m68k-amigaos make clean all
```

El `Makefile` usa `-noixemul` de forma obligatoria (requisito de este
*toolchain*) y produce el ejecutable `amiga/AmiRenderSwarm`.

Si ya se dispone de un `m68k-amigaos-gcc` instalado de forma nativa, basta con:

```bash
cd amiga
make
```

> `make clean` borra los ficheros `.o`, que están versionados en Git; un build
> limpio los regenera de forma idéntica.

### Lanzar el agente en el Amiga

Copia `AmiRenderSwarm` al Amiga y ejecútalo desde una ventana de shell o de
Consola (CLI) con los argumentos posicionales y, opcionalmente, las opciones de
registro:

```text
AmiRenderSwarm <hub_ip> <hub_port> <node_id> <listen_port> [render_cmd_printf]
                [--log-level <debug|info|warn|error>] [--log-file <ruta>]
```

Ejemplos:

```bash
# Nodo worker/director con valores por defecto
AmiRenderSwarm 192.168.18.26 7900 amiga_vampire 7800

# Con log más silencioso y copia a fichero
AmiRenderSwarm 192.168.18.26 7900 amiga_vampire 7800 --log-level info --log-file T:amirs_log.txt

# Con plantilla de render propia (%lu = frame, %s = escena, %s = salida)
AmiRenderSwarm 192.168.18.26 7900 amiga_vampire 7800 'lwsn -i %lu -o %s %s'
```

Si no se indica `render_cmd_printf`, el worker ejecuta `Copy "<escena>" "<salida>"`
para validar el transporte binario antes de conectar `lwsn` o `povray`.

### Probar el hub sin Amiga

El hub habla ASCII línea a línea, así que puede ejercitarse con cualquier
cliente TCP. Con el hub escuchando en `127.0.0.1:7900`:

```text
HELLO id=smoke os=3.2 cpu=68080 fpu=1 ram_mb=512 engines=lwsn role=both listen_port=7800
PING state=IDLE load=0.1
LIST_PEERS engine=lwsn min_ram=64
```

Respuestas esperadas: `WELCOME peer_ip=... session=...`, `PONG ts=...` y una
línea `PEER ...` por cada par disponible, cerrada con `PEERS_END count=N`.

---

## 8. Registro (Logging) del Agente

El agente registra **toda** su ejecución en la consola. Cada línea lleva marca de
tiempo absoluta, segundos transcurridos desde el arranque, nivel y una etiqueta
del componente que la emite:

```text
2026-11-27 10:35:12.340 [T+000003s] [INFO ] [NET] Conectado al Hub en 192.168.18.26:7900 (fd=3)
2026-11-27 10:35:12.345 [T+000003s] [INFO ] [NET] TX -> HELLO id=amiga_vampire os=3.2 cpu=68080 ...
2026-11-27 10:35:12.512 [T+000003s] [INFO ] [HUB] WELCOME peer_ip=192.168.18.26 session=s_1a2b3c4d
2026-11-27 10:35:27.348 [T+000018s] [DEBUG] [PROTO] RX paquete RENDER_DONE frame=1 flags=0 payload=0 bytes
```

### Niveles

| Nivel    | Contenido |
| -------- | --------- |
| `DEBUG`  | Traza fina: bytes leídos/escritos, cada paquete binario, cada línea de señalización, ficheros temporales. **Es el nivel por defecto.** |
| `INFO`   | Hitos: conexión al hub, `HELLO`/`PING`, invitaciones, inicio y fin de render, recepción de frames. |
| `WARN`   | Situaciones anómalas que no abortan la ejecución. |
| `ERROR`  | Fallos que sí abortan la operación o la sesión. |

### Etiquetas de componente

| Etiqueta | Módulo |
| -------- | ------ |
| `MAIN`   | Arranque, configuración, bucle principal y apagado. |
| `NET`    | `bsdsocket.library`, conexión al hub, envío/recepción de líneas. |
| `PROTO`  | Trama binaria `AMIR` de 16 bytes. |
| `WORKER` | Listener directo, escritura de ficheros, render y envío del frame. |
| `DIR`    | Flujo del director: conexión directa al worker y guardado del frame. |
| `HUB`    | Líneas de señalización recibidas del hub. |
| `LOG`    | Mensajes del propio subsistema de log. |

### Línea de órdenes

```text
AmiRenderSwarm <hub_ip> <hub_port> <node_id> <listen_port> [render_cmd_printf]
                [--log-level <debug|info|warn|error>] [--log-file <ruta>]
```

- `--log-level <nivel>`: nivel mínimo que se escribe. Por defecto `debug` (todo).
- `--log-file <ruta>`: copia del log a un fichero, **añadiendo al final** si ya existe.
  Ejemplo: `--log-file T:amirs_log.txt`. Sólo requiere `dos.library`; no hace falta
  `Nudos.library` ni `Nuutility.library`.
- `--help`: muestra el modo de uso y termina.

Las opciones se pueden escribir como `--clave valor` o `--clave=valor`, y pueden
aparecer en cualquier posición. Los argumentos posicionales mantienen el orden
original, de modo que las órdenes existentes siguen funcionando sin cambios:

```bash
AmiRenderSwarm 192.168.18.26 7900 amiga_vampire 7800
AmiRenderSwarm 192.168.18.26 7900 amiga_vampire 7800 --log-level info
AmiRenderSwarm 192.168.18.26 7900 amiga_vampire 7800 --log-file=T:amirs_log.txt
```

### Notas de implementación

- La marca de tiempo se obtiene del `DateStamp()` del **`dos.library` real** del
  sistema (invocado por LVO, `-192`), no del `DOSBase` del entorno de compilación.
  El epoch de DOS (1978-01-01) se convierte al calendario gregoriano.
- `stdout` se configura con salida por líneas y se vacía tras cada mensaje, de
  modo que los mensajes aparecen en la consola al instante, incluso durante un
  render largo.
- El fichero de log se abre con la API de DOS (`Open`/`Seek`/`Write`/`Close`) en
  lugar de `fopen()`, para no arrastrar dependencias de biblioteca adicionales
  en el Amiga.
- El agente **no** envía `PING`s mientras atiende a un director: durante un render
  largo el hub puede darlo por caducado (`-timeout`, 60 s por defecto). El log
  deja ver exactamente cuándo empieza y termina cada sesión de render.