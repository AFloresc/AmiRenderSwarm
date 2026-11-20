#include "net.h"
#include "proto.h"
#include "worker.h"

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
    const char *sample_scene = "AMIRS_SAMPLE_SCENE_DATA\nframe=1\nquality=draft\n";
    BOOL ok = FALSE;

    if (!peer_ip || peer_ip[0] == '\0' || peer_port == 0) {
        return FALSE;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("[DIR] Error creando socket hacia %s:%u\n", peer_ip, (unsigned int)peer_port);
        return FALSE;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(peer_port);

    if (inet_aton((STRPTR)peer_ip, &addr.sin_addr) == 0) {
        struct hostent *he = gethostbyname((STRPTR)peer_ip);
        if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
            printf("[DIR] No se pudo resolver IP %s\n", peer_ip);
            CloseSocket(sock);
            return FALSE;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    }

    printf("[DIR] Conectando directamente a %s:%u para job=%s frame=%lu...\n",
           peer_ip, (unsigned int)peer_port, job_id, (unsigned long)frame_num);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[DIR] Error conectando al worker %s:%u\n", peer_ip, (unsigned int)peer_port);
        CloseSocket(sock);
        return FALSE;
    }

    snprintf(job_meta, sizeof(job_meta), "job=%s;frame=%lu", job_id, (unsigned long)frame_num);
    if (!Amirs_SendPacket(sock, AMIRS_MSG_JOB_INIT, 0, frame_num,
                          job_meta, (ULONG)strlen(job_meta))) {
        printf("[DIR] Error enviando JOB_INIT\n");
        CloseSocket(sock);
        return FALSE;
    }

    if (!Amirs_SendPacket(sock, AMIRS_MSG_SCENE_CHUNK, 0, frame_num,
                          sample_scene, (ULONG)strlen(sample_scene))) {
        printf("[DIR] Error enviando SCENE_CHUNK\n");
        CloseSocket(sock);
        return FALSE;
    }

    /* Esperar respuestas binarias del worker (RENDER_DONE / FRAME_DATA / ERR) */
    for (;;) {
        AmirsPacketHeader hdr;
        int hres = Amirs_RecvHeader(sock, &hdr);

        if (hres == 0) {
            printf("[DIR] Worker cerró la conexión directa\n");
            break;
        }
        if (hres < 0) {
            printf("[DIR] Error recibiendo cabecera del worker\n");
            break;
        }

        if (hdr.payload_len > 0) {
            UBYTE *buf = (UBYTE *)AllocVec(hdr.payload_len, MEMF_ANY);
            if (!buf) {
                printf("[DIR] Sin memoria para payload de %lu bytes\n", (unsigned long)hdr.payload_len);
                break;
            }
            if (!Amirs_RecvExact(sock, buf, hdr.payload_len)) {
                FreeVec(buf);
                printf("[DIR] Error leyendo payload del worker\n");
                break;
            }

            if (hdr.type == AMIRS_MSG_RENDER_DONE) {
                printf("[DIR] RENDER_DONE frame=%lu\n", (unsigned long)hdr.frame);
            } else if (hdr.type == AMIRS_MSG_FRAME_DATA) {
                char out_file[128];
                BPTR fh;
                snprintf(out_file, sizeof(out_file), "T:amirs_recv_%lu.dat", (unsigned long)hdr.frame);
                fh = Open((STRPTR)out_file, MODE_NEWFILE);
                if (fh) {
                    Write(fh, buf, (LONG)hdr.payload_len);
                    Close(fh);
                    printf("[DIR] FRAME_DATA frame=%lu guardado en %s (%lu bytes)\n",
                           (unsigned long)hdr.frame, out_file, (unsigned long)hdr.payload_len);
                    ok = TRUE;
                }
                FreeVec(buf);
                break;
            } else if (hdr.type == AMIRS_MSG_ERR) {
                printf("[DIR] Error remoto en frame=%lu: %.*s\n",
                       (unsigned long)hdr.frame, (int)hdr.payload_len, (const char *)buf);
                FreeVec(buf);
                break;
            }

            FreeVec(buf);
        } else {
            if (hdr.type == AMIRS_MSG_RENDER_DONE) {
                printf("[DIR] RENDER_DONE frame=%lu\n", (unsigned long)hdr.frame);
            } else if (hdr.type == AMIRS_MSG_ERR) {
                printf("[DIR] Error remoto sin payload en frame=%lu\n", (unsigned long)hdr.frame);
                break;
            }
        }
    }

    CloseSocket(sock);
    return ok;
}

static void handle_hub_line(HubConn *hc, LONG listen_sock, const char *line,
                            const char *render_cmd_template)
{
    printf("[HUB] %s\n", line);

    if (strncmp(line, "INVITE_REQ ", 11) == 0) {
        char from_id[64] = "";
        char job_id[64] = "job_01";
        char resp[192];

        parse_kv_param(line, "from", from_id, sizeof(from_id));
        parse_kv_param(line, "job", job_id, sizeof(job_id));

        if (from_id[0] != '\0' && listen_sock >= 0) {
            snprintf(resp, sizeof(resp), "INVITE_OK to=%s job=%s\n", from_id, job_id);
            if (Hub_SendLine(hc, resp)) {
                Hub_SendLine(hc, "STATE state=BUSY\n");
                Worker_HandleDirectorSession(listen_sock, render_cmd_template);
                Hub_SendLine(hc, "STATE state=IDLE\n");
            }
        }
    } else if (strncmp(line, "INVITED ", 8) == 0) {
        char peer_ip[64] = "";
        char port_str[16] = "7800";
        char job_id[64] = "job_01";

        parse_kv_param(line, "ip", peer_ip, sizeof(peer_ip));
        parse_kv_param(line, "port", port_str, sizeof(port_str));
        parse_kv_param(line, "job", job_id, sizeof(job_id));

        if (peer_ip[0] != '\0') {
            Hub_SendLine(hc, "STATE state=BUSY\n");
            run_director_transfer(peer_ip, (UWORD)atoi(port_str), job_id, 1UL);
            Hub_SendLine(hc, "STATE state=IDLE\n");
        }
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

    HubConn hc;
    LONG listen_sock = -1;
    char hello_msg[512];
    char line_out[512];
    BOOL running = TRUE;

    /* Uso opcional:
     * AmiRenderSwarm <hub_ip> <hub_port> <node_id> <listen_port> [render_cmd_printf]
     */
    if (argc > 1) hub_host    = argv[1];
    if (argc > 2) hub_port    = (UWORD)atoi(argv[2]);
    if (argc > 3) node_id     = argv[3];
    if (argc > 4) listen_port = (UWORD)atoi(argv[4]);
    if (argc > 5) render_cmd  = argv[5];

    if (!Net_Init()) {
        return RETURN_FAIL;
    }

    if (!Worker_StartListener(listen_port, &listen_sock)) {
        Net_Cleanup();
        return RETURN_FAIL;
    }

    if (!Hub_Connect(&hc, hub_host, hub_port)) {
        Worker_StopListener(&listen_sock);
        Net_Cleanup();
        return RETURN_ERROR;
    }

    snprintf(hello_msg, sizeof(hello_msg),
             "HELLO id=%s os=3.2 cpu=%s fpu=1 ram_mb=%d engines=%s role=both listen_port=%u\n",
             node_id, cpu, ram_mb, engines, (unsigned int)listen_port);

    if (!Hub_SendLine(&hc, hello_msg)) {
        printf("Error enviando HELLO\n");
        Hub_Disconnect(&hc);
        Worker_StopListener(&listen_sock);
        Net_Cleanup();
        return RETURN_ERROR;
    }

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
            printf("\n[EXIT] Ctrl-C recibido, cerrando...\n");
            running = FALSE;
            break;
        }

        if (ready < 0) {
            printf("[ERR] Error en WaitSelect\n");
            running = FALSE;
            break;
        }

        if (ready == 0) {
            if (!Hub_SendLine(&hc, "PING state=IDLE load=0.05\n")) {
                printf("[ERR] Error enviando PING\n");
                running = FALSE;
            }
            continue;
        }

        if (FD_ISSET(hc.sock, &read_fds)) {
            while (running) {
                int r = Hub_ReadLine(&hc, line_out, sizeof(line_out));
                if (r > 0) {
                    handle_hub_line(&hc, listen_sock, line_out, render_cmd);
                } else if (r == 0) {
                    break;
                } else {
                    printf("[DISCONNECT] Conexión cerrada por el Hub\n");
                    running = FALSE;
                    break;
                }
            }
        }
    }

    Hub_Disconnect(&hc);
    Worker_StopListener(&listen_sock);
    Net_Cleanup();
    return RETURN_OK;
}