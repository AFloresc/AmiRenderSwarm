#include "net.h"

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

static void handle_hub_line(HubConn *hc, const char *line)
{
    printf("[HUB] %s\n", line);

    if (strncmp(line, "INVITE_REQ ", 11) == 0) {
        /* Ejemplo de respuesta automática de prueba:
         * INVITE_REQ from=amiga_dir from_ip=... from_port=... job=test engine=lwsn frames=10
         * En el siguiente paso conectaremos esto con el worker de render.
         */
        printf("[INFO] Invitación de render recibida.\n");
    }
}

int main(int argc, char **argv)
{
    const char *hub_host = "127.0.0.1";
    UWORD hub_port = 7900;
    const char *node_id = "amiga_node_01";
    const char *cpu = "68080";
    int ram_mb = 256;
    const char *engines = "lwsn,povray";

    HubConn hc;
    char hello_msg[512];
    char line_out[512];
    BOOL running = TRUE;
    ULONG last_ping_sec = 0;
    ULONG cur_sec = 0, cur_usec = 0;

    if (argc > 1) hub_host = argv[1];
    if (argc > 2) hub_port = (UWORD)atoi(argv[2]);
    if (argc > 3) node_id  = argv[3];

    if (!Net_Init()) {
        return RETURN_FAIL;
    }

    if (!Hub_Connect(&hc, hub_host, hub_port)) {
        Net_Cleanup();
        return RETURN_ERROR;
    }

    snprintf(hello_msg, sizeof(hello_msg),
             "HELLO id=%s os=3.2 cpu=%s fpu=1 ram_mb=%d engines=%s role=both listen_port=7800\n",
             node_id, cpu, ram_mb, engines);

    if (!Hub_SendLine(&hc, hello_msg)) {
        printf("Error enviando HELLO\n");
        Hub_Disconnect(&hc);
        Net_Cleanup();
        return RETURN_ERROR;
    }

    GetSysTime(&(struct timeval){0, 0}); /* inicializa si hace falta */
    /* Usamos WaitSelect con timeout para latidos periódicos y Ctrl-C */
    while (running) {
        fd_set read_fds;
        struct timeval tv;
        ULONG sig_mask = SIGBREAKF_CTRL_C;
        LONG ready;

        FD_ZERO(&read_fds);
        FD_SET(hc.sock, &read_fds);

        tv.tv_secs = PING_INTERVAL_SEC;
        tv.tv_micro = 0;

        ready = WaitSelect(hc.sock + 1, &read_fds, NULL, NULL, &tv, &sig_mask);

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

        if (ready > 0 && FD_ISSET(hc.sock, &read_fds)) {
            while (running) {
                int r = Hub_ReadLine(&hc, line_out, sizeof(line_out));
                if (r > 0) {
                    handle_hub_line(&hc, line_out);
                } else if (r == 0) {
                    break;
                } else {
                    printf("[DISCONNECT] Conexión cerrada por el Hub\n");
                    running = FALSE;
                    break;
                }
            }
        }

        /* Enviar PING periódico para mantener presencia activa */
        if (running) {
            /* En un bucle real comprobamos tiempo transcurrido; aquí tras timeout o actividad */
            (void)cur_sec;
            (void)cur_usec;
            (void)last_ping_sec;
            if (ready == 0) {
                if (!Hub_SendLine(&hc, "PING state=IDLE load=0.05\n")) {
                    printf("[ERR] Error enviando PING\n");
                    running = FALSE;
                }
            }
        }
    }

    Hub_Disconnect(&hc);
    Net_Cleanup();
    return RETURN_OK;
}