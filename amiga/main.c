#include "net.h"
#include "proto.h"
#include "worker.h"
#include "log.h"

#include <exec/exec.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PING_INTERVAL_SEC 15L
#define MAX_POS_ARGS      8

/* Opciones admitidas en la línea de órdenes:
 *
 *   --log-level <debug|info|warn|error>   nivel mínimo de log (por defecto: debug)
 *   --log-file <ruta>                     copia del log a un fichero (append)
 *
 * Se aceptan tanto "--clave valor" como "--clave=valor". Los argumentos
 * posicionales siguen siendo:
 *
 *   AmiRenderSwarm <hub_ip> <hub_port> <node_id> <listen_port> [render_cmd_printf]
 */
static void print_usage(void)
{
    printf("Uso: AmiRenderSwarm <hub_ip> <hub_port> <node_id> <listen_port> [render_cmd_printf]\n");
    printf("            [--log-level <debug|info|warn|error>] [--log-file <ruta>]\n");
    printf("\n");
    printf("  hub_ip          dirección del hub de rendezvous (por defecto 127.0.0.1)\n");
    printf("  hub_port        puerto del hub (por defecto 7900)\n");
    printf("  node_id         identificador de este nodo (por defecto amiga_node_01)\n");
    printf("  listen_port     puerto de escucha para conexiones directas (por defecto 7800)\n");
    printf("  render_cmd_printf  plantilla de render: %%lu frame, %%s escena, %%s salida\n");
    printf("  --log-level     nivel mínimo de log: debug, info, warn o error\n");
    printf("  --log-file      ruta del fichero de log (se añade al final si existe)\n");
}

/* TRUE si el argumento es la opción "--name" o "--name=valor". */
static BOOL arg_is_option(const char *arg, const char *name)
{
    size_t n;

    if (arg == NULL || name == NULL) {
        return FALSE;
    }

    n = strlen(name);
    if (strncmp(arg, name, n) != 0) {
        return FALSE;
    }

    return (arg[n] == '\0' || arg[n] == '=') ? TRUE : FALSE;
}

/* Separa las opciones de los argumentos posicionales.
 * Las advertencias se guardan en warn_buf para poder registrarlas en el log.
 */
static void parse_command_line(int argc, char **argv,
                               const char **pos, int max_pos, int *out_n_pos,
                               LogLevel *out_level, const char **out_log_file,
                               BOOL *out_help, char *warn_buf, int warn_len)
{
    int i;
    int p = 0;

    *out_n_pos   = 0;
    *out_level   = LOG_LEVEL_DEBUG;
    *out_log_file = NULL;
    *out_help    = FALSE;

    if (warn_buf != NULL && warn_len > 0) {
        warn_buf[0] = '\0';
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = NULL;

        if (a == NULL) {
            continue;
        }

        if (a[0] != '-' || a[1] != '-') {
            if (p < max_pos) {
                pos[p++] = a;
            }
            continue;
        }

        if (arg_is_option(a, "--help") || arg_is_option(a, "--usage")) {
            *out_help = TRUE;
            continue;
        }

        if (arg_is_option(a, "--log-level") || arg_is_option(a, "--log-file")) {
            const char *eq = strchr(a, '=');
            if (eq != NULL) {
                val = eq + 1;
            } else if (i + 1 < argc && argv[i + 1] != NULL &&
                       !(argv[i + 1][0] == '-' && argv[i + 1][1] == '-')) {
                /* Sólo consumimos el siguiente argumento si no es otra opción:
                 * así "--log-level --log-file x" no se come el --log-file.
                 */
                val = argv[++i];
            }
        }

        if (arg_is_option(a, "--log-level")) {
            LogLevel lvl = Log_LevelFromName(val);
            if (lvl == LOG_LEVEL_INVALID) {
                if (warn_buf != NULL && warn_len > 0) {
                    snprintf(warn_buf, (size_t)warn_len,
                             "Nivel de log desconocido: %s (use debug, info, warn o error)",
                             val ? val : "(vacío)");
                }
            } else {
                *out_level = lvl;
            }
        } else if (arg_is_option(a, "--log-file")) {
            if (val != NULL && val[0] != '\0') {
                *out_log_file = val;
            } else if (warn_buf != NULL && warn_len > 0) {
                snprintf(warn_buf, (size_t)warn_len, "Falta la ruta del fichero de log");
            }
        } else if (warn_buf != NULL && warn_len > 0) {
            snprintf(warn_buf, (size_t)warn_len, "Opción desconocida: %s", a);
        }
    }

    *out_n_pos = p;
}

/* Busca un parámetro "clave=valor" dentro de una línea de señalización.
 * Devuelve TRUE si se encontró y copia el valor en out_val.
 */
static BOOL parse_kv_param(const char *line, const char *key, char *out_val, int max_len)
{
    char needle[64];
    const char *p;
    int len = 0;

    if (!line || !key || !out_val || max_len < 2) {
        return FALSE;
    }

    snprintf(needle, sizeof(needle), "%s=", key);
    p = line;

    while (*p != '\0') {
        if ((p == line || p[-1] == ' ') && strncmp(p, needle, strlen(needle)) == 0) {
            const char *val_start = p + strlen(needle);
            const char *val_end = strchr(val_start, ' ');
            len = val_end ? (int)(val_end - val_start) : (int)strlen(val_start);
            if (len >= max_len) {
                len = max_len - 1;
            }
            memcpy(out_val, val_start, (size_t)len);
            out_val[len] = '\0';
            return TRUE;
        }
        p++;
    }

    return FALSE;
}

/* Conecta directamente a un Worker aceptado (INVITED) y envía un trabajo de prueba
 * usando la cabecera binaria de 16 bytes ("AMIR").
 */
static BOOL run_director_transfer(const char *peer_ip, UWORD peer_port,
                                  const char *job_id, ULONG frame_num)
{
    struct sockaddr_in addr;
    LONG sock;
    char job_meta[160];
    char ipbuf[24];
    const char *sample_scene = "AMIRS_SAMPLE_SCENE_DATA\nframe=1\nquality=draft\n";
    BOOL ok = FALSE;

    if (!peer_ip || peer_ip[0] == '\0' || peer_port == 0) {
        LOG_ERROR("DIR", "Datos del worker incompletos (ip=%s port=%u)",
                  peer_ip ? peer_ip : "(null)", (unsigned int)peer_port);
        return FALSE;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LOG_ERROR("DIR", "Error creando socket hacia %s:%u (errno=%ld)",
                  peer_ip, (unsigned int)peer_port, (long)Errno());
        return FALSE;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(peer_port);

    if (inet_aton((STRPTR)peer_ip, &addr.sin_addr) == 0) {
        struct hostent *he = gethostbyname((STRPTR)peer_ip);
        if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
            LOG_ERROR("DIR", "No se pudo resolver la IP %s", peer_ip);
            CloseSocket(sock);
            return FALSE;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
        Net_FormatIPv4(addr.sin_addr.s_addr, ipbuf, sizeof(ipbuf));
        LOG_INFO("DIR", "Worker %s resuelto a %s", peer_ip, ipbuf);
    } else {
        Net_FormatIPv4(addr.sin_addr.s_addr, ipbuf, sizeof(ipbuf));
        LOG_DEBUG("DIR", "IP literal del worker: %s", ipbuf);
    }

    LOG_INFO("DIR", "Conectando directamente a %s:%u para job=%s frame=%lu...",
             peer_ip, (unsigned int)peer_port, job_id, (unsigned long)frame_num);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("DIR", "Error conectando al worker %s:%u (errno=%ld)",
                  peer_ip, (unsigned int)peer_port, (long)Errno());
        CloseSocket(sock);
        return FALSE;
    }
    LOG_INFO("DIR", "Conexión directa con el worker establecida (fd=%ld)", (long)sock);

    snprintf(job_meta, sizeof(job_meta), "job=%s;frame=%lu", job_id, (unsigned long)frame_num);
    if (!Amirs_SendPacket(sock, AMIRS_MSG_JOB_INIT, 0, frame_num,
                          job_meta, (ULONG)strlen(job_meta))) {
        LOG_ERROR("DIR", "Error enviando JOB_INIT");
        CloseSocket(sock);
        return FALSE;
    }

    if (!Amirs_SendPacket(sock, AMIRS_MSG_SCENE_CHUNK, 0, frame_num,
                          sample_scene, (ULONG)strlen(sample_scene))) {
        LOG_ERROR("DIR", "Error enviando SCENE_CHUNK");
        CloseSocket(sock);
        return FALSE;
    }

    /* Esperar respuestas binarias del worker (RENDER_DONE / FRAME_DATA / ERR) */
    for (;;) {
        AmirsPacketHeader hdr;
        int hres = Amirs_RecvHeader(sock, &hdr);

        if (hres == 0) {
            LOG_INFO("DIR", "El worker cerró la conexión directa");
            break;
        }
        if (hres < 0) {
            LOG_ERROR("DIR", "Error recibiendo la cabecera del worker");
            break;
        }

        if (hdr.payload_len > 0) {
            UBYTE *buf = (UBYTE *)AllocVec(hdr.payload_len, MEMF_ANY);
            if (!buf) {
                LOG_ERROR("DIR", "Sin memoria para un payload de %lu bytes",
                          (unsigned long)hdr.payload_len);
                break;
            }
            if (!Amirs_RecvExact(sock, buf, hdr.payload_len)) {
                FreeVec(buf);
                LOG_ERROR("DIR", "Error leyendo el payload del worker");
                break;
            }

            if (hdr.type == AMIRS_MSG_RENDER_DONE) {
                LOG_INFO("DIR", "RENDER_DONE frame=%lu", (unsigned long)hdr.frame);
            } else if (hdr.type == AMIRS_MSG_FRAME_DATA) {
                char out_file[128];
                BPTR fh;
                snprintf(out_file, sizeof(out_file), "T:amirs_recv_%lu.dat", (unsigned long)hdr.frame);
                fh = Open((STRPTR)out_file, MODE_NEWFILE);
                if (fh) {
                    Write(fh, buf, (LONG)hdr.payload_len);
                    Close(fh);
                    LOG_INFO("DIR", "FRAME_DATA frame=%lu guardado en %s (%lu bytes)",
                             (unsigned long)hdr.frame, out_file, (unsigned long)hdr.payload_len);
                    ok = TRUE;
                } else {
                    LOG_ERROR("DIR", "No se pudo crear el fichero %s (IoErr=%ld)",
                              out_file, (long)IoErr());
                }
                FreeVec(buf);
                break;
            } else if (hdr.type == AMIRS_MSG_ERR) {
                LOG_ERROR("DIR", "Error remoto en frame=%lu: %.*s",
                          (unsigned long)hdr.frame, (int)hdr.payload_len, (const char *)buf);
                FreeVec(buf);
                break;
            }

            FreeVec(buf);
        } else {
            if (hdr.type == AMIRS_MSG_RENDER_DONE) {
                LOG_INFO("DIR", "RENDER_DONE frame=%lu", (unsigned long)hdr.frame);
            } else if (hdr.type == AMIRS_MSG_ERR) {
                LOG_ERROR("DIR", "Error remoto sin payload en frame=%lu", (unsigned long)hdr.frame);
                break;
            }
        }
    }

    LOG_DEBUG("DIR", "Cerrando la conexión directa con el worker (fd=%ld)", (long)sock);
    CloseSocket(sock);
    if (!ok) {
        LOG_WARN("DIR", "La transferencia directa no produjo ningún frame");
    }
    return ok;
}

static void handle_hub_line(HubConn *hc, LONG listen_sock, const char *line,
                            const char *render_cmd_template)
{
    LOG_INFO("HUB", "%s", line);

    if (strncmp(line, "INVITE_REQ ", 11) == 0) {
        char from_id[64] = "";
        char job_id[64] = "job_01";
        char resp[192];

        parse_kv_param(line, "from", from_id, sizeof(from_id));
        parse_kv_param(line, "job", job_id, sizeof(job_id));

        LOG_INFO("DIR", "INVITE_REQ recibida de %s para job=%s", from_id, job_id);

        if (from_id[0] != '\0' && listen_sock >= 0) {
            snprintf(resp, sizeof(resp), "INVITE_OK to=%s job=%s\n", from_id, job_id);
            if (Hub_SendLine(hc, resp)) {
                Hub_SendLine(hc, "STATE state=BUSY\n");
                Worker_HandleDirectorSession(listen_sock, render_cmd_template);
                Hub_SendLine(hc, "STATE state=IDLE\n");
            }
        } else {
            LOG_WARN("DIR", "No se puede atender la invitación: falta 'from' o no hay listener");
        }
    } else if (strncmp(line, "INVITED ", 8) == 0) {
        char peer_ip[64] = "";
        char port_str[16] = "7800";
        char job_id[64] = "job_01";

        parse_kv_param(line, "ip", peer_ip, sizeof(peer_ip));
        parse_kv_param(line, "port", port_str, sizeof(port_str));
        parse_kv_param(line, "job", job_id, sizeof(job_id));

        LOG_INFO("DIR", "INVITED recibida: worker=%s:%s job=%s", peer_ip, port_str, job_id);

        if (peer_ip[0] != '\0') {
            Hub_SendLine(hc, "STATE state=BUSY\n");
            run_director_transfer(peer_ip, (UWORD)atoi(port_str), job_id, 1UL);
            Hub_SendLine(hc, "STATE state=IDLE\n");
        } else {
            LOG_WARN("DIR", "INVITED sin dirección IP de worker");
        }
    } else {
        LOG_DEBUG("HUB", "Línea sin acción associada");
    }
}

int main(int argc, char **argv)
{
    const char *hub_host = "127.0.0.1";
    UWORD hub_port = 7900;
    const char *node_id = "amiga_node_01";
    UWORD listen_port = 7800;
    const char *cpu = "68080";
    int ram_mb = 256;
    const char *engines = "lwsn,povray";
    const char *render_cmd = NULL;

    const char *pos[MAX_POS_ARGS];
    int   n_pos = 0;
    LogLevel log_level = LOG_LEVEL_DEBUG;
    const char *log_file = NULL;
    BOOL want_help = FALSE;
    char cli_warn[192] = "";

    HubConn hc;
    LONG listen_sock = -1;
    char hello_msg[512];
    char line_out[512];
    BOOL running = TRUE;

    /* Uso opcional:
     * AmiRenderSwarm <hub_ip> <hub_port> <node_id> <listen_port> [render_cmd_printf]
     *                [--log-level <nivel>] [--log-file <ruta>]
     */
    parse_command_line(argc, argv, pos, MAX_POS_ARGS, &n_pos,
                        &log_level, &log_file, &want_help,
                        cli_warn, sizeof(cli_warn));

    Log_Init(log_level, log_file);

    if (want_help) {
        print_usage();
        Log_Shutdown();
        return RETURN_OK;
    }

    LOG_INFO("MAIN", "AmiRenderSwarm iniciando (nivel de log: %s%s)",
             Log_LevelName(log_level),
             log_file ? ", con fichero de log" : "");
    if (cli_warn[0] != '\0') {
        LOG_WARN("MAIN", "%s", cli_warn);
    }

    if (n_pos > 0) hub_host    = pos[0];
    if (n_pos > 1) hub_port    = (UWORD)atoi(pos[1]);
    if (n_pos > 2) node_id     = pos[2];
    if (n_pos > 3) listen_port = (UWORD)atoi(pos[3]);
    if (n_pos > 4) render_cmd  = pos[4];

    LOG_INFO("MAIN", "Configuración: hub=%s:%u node_id=%s listen_port=%u",
             hub_host, (unsigned int)hub_port, node_id, (unsigned int)listen_port);
    LOG_INFO("MAIN", "Perfil anunciado: os=3.2 cpu=%s fpu=1 ram_mb=%d engines=%s role=both",
             cpu, ram_mb, engines);
    if (render_cmd != NULL) {
        LOG_INFO("MAIN", "Plantilla de render: %s", render_cmd);
    } else {
        LOG_INFO("MAIN", "Sin plantilla de render: se usará \"Copy <escena> <salida>\"");
    }

    if (!Net_Init()) {
        Log_Shutdown();
        return RETURN_FAIL;
    }

    if (!Worker_StartListener(listen_port, &listen_sock)) {
        Net_Cleanup();
        Log_Shutdown();
        return RETURN_FAIL;
    }

    if (!Hub_Connect(&hc, hub_host, hub_port)) {
        Worker_StopListener(&listen_sock);
        Net_Cleanup();
        Log_Shutdown();
        return RETURN_ERROR;
    }

    snprintf(hello_msg, sizeof(hello_msg),
             "HELLO id=%s os=3.2 cpu=%s fpu=1 ram_mb=%d engines=%s role=both listen_port=%u\n",
             node_id, cpu, ram_mb, engines, (unsigned int)listen_port);

    if (!Hub_SendLine(&hc, hello_msg)) {
        LOG_ERROR("MAIN", "Error enviando HELLO");
        Hub_Disconnect(&hc);
        Worker_StopListener(&listen_sock);
        Net_Cleanup();
        Log_Shutdown();
        return RETURN_ERROR;
    }

    LOG_INFO("MAIN", "Esperando mensajes del hub (latido cada %ld s)", PING_INTERVAL_SEC);

    while (running) {
        fd_set read_fds;
        struct timeval tv;
        ULONG sig_mask = SIGBREAKF_CTRL_C;
        LONG max_fd = hc.sock;
        LONG ready;

        FD_ZERO(&read_fds);
        FD_SET(hc.sock, &read_fds);
        if (listen_sock >= 0) {
            FD_SET(listen_sock, &read_fds);
            if (listen_sock > max_fd) {
                max_fd = listen_sock;
            }
        }

        tv.tv_secs  = PING_INTERVAL_SEC;
        tv.tv_micro = 0;

        ready = WaitSelect(max_fd + 1, &read_fds, NULL, NULL, &tv, &sig_mask);

        if (sig_mask & SIGBREAKF_CTRL_C) {
            LOG_INFO("MAIN", "Ctrl-C recibido, cerrando...");
            running = FALSE;
            break;
        }

        if (ready < 0) {
            LOG_ERROR("MAIN", "Error en WaitSelect");
            running = FALSE;
            break;
        }

        if (ready == 0) {
            LOG_DEBUG("MAIN", "Tiempo de espera agotado: toca enviar el latido");
            if (!Hub_SendLine(&hc, "PING state=IDLE load=0.05\n")) {
                LOG_ERROR("MAIN", "Error enviando PING");
                running = FALSE;
            }
            continue;
        }

        if (listen_sock >= 0 && FD_ISSET(listen_sock, &read_fds)) {
            LOG_DEBUG("MAIN", "Hay una conexión directa pendiente de atender");
        }

        if (FD_ISSET(hc.sock, &read_fds)) {
            while (running) {
                int r = Hub_ReadLine(&hc, line_out, sizeof(line_out));
                if (r > 0) {
                    handle_hub_line(&hc, listen_sock, line_out, render_cmd);
                } else if (r == 0) {
                    break;
                } else {
                    LOG_INFO("MAIN", "Conexión cerrada por el Hub");
                    running = FALSE;
                    break;
                }
            }
        }
    }

    LOG_INFO("MAIN", "Cerrando el agente");
    Hub_Disconnect(&hc);
    Worker_StopListener(&listen_sock);
    Net_Cleanup();
    Log_Shutdown();
    return RETURN_OK;
}