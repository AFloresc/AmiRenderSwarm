#include "worker.h"
#include "proto.h"

#include <exec/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern struct Library *SocketBase;

#define MAX_PAYLOAD_BYTES (16UL * 1024UL * 1024UL) /* 16 MB límite defensivo */

BOOL Worker_StartListener(UWORD listen_port, LONG *out_listen_sock)
{
    LONG s;
    struct sockaddr_in addr;
    LONG reuse = 1;

    if (!out_listen_sock || SocketBase == NULL) {
        return FALSE;
    }

    *out_listen_sock = -1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("[WORKER] Error creando socket de escucha\n");
        return FALSE;
    }

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(listen_port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[WORKER] Error en bind() puerto %u\n", (unsigned int)listen_port);
        CloseSocket(s);
        return FALSE;
    }

    if (listen(s, 4) < 0) {
        printf("[WORKER] Error en listen()\n");
        CloseSocket(s);
        return FALSE;
    }

    *out_listen_sock = s;
    printf("[WORKER] Escuchando conexiones directas en puerto %u\n", (unsigned int)listen_port);
    return TRUE;
}

void Worker_StopListener(LONG *listen_sock)
{
    if (listen_sock && *listen_sock >= 0) {
        CloseSocket(*listen_sock);
        *listen_sock = -1;
    }
}

static BOOL write_file_all(const char *path, const UBYTE *data, ULONG len)
{
    BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
    ULONG written = 0;

    if (!fh) {
        return FALSE;
    }

    while (written < len) {
        LONG chunk = (len - written > 32768UL) ? 32768L : (LONG)(len - written);
        LONG w = Write(fh, (APTR)(data + written), chunk);
        if (w <= 0) {
            Close(fh);
            return FALSE;
        }
        written += (ULONG)w;
    }

    Close(fh);
    return TRUE;
}

static BOOL send_file_as_frame(LONG client_sock, ULONG frame, const char *out_path)
{
    BPTR fh = Open((STRPTR)out_path, MODE_OLDFILE);
    struct FileInfoBlock *fib;
    ULONG file_size = 0;
    UBYTE *buf;
    BOOL ok = FALSE;

    if (!fh) {
        return FALSE;
    }

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        if (ExamineFH(fh, fib)) {
            file_size = (ULONG)fib->fib_Size;
        }
        FreeDosObject(DOS_FIB, fib);
    }

    if (file_size == 0 || file_size > MAX_PAYLOAD_BYTES) {
        Close(fh);
        return FALSE;
    }

    buf = (UBYTE *)AllocVec(file_size, MEMF_ANY);
    if (!buf) {
        Close(fh);
        return FALSE;
    }

    if (Amirs_RecvExact(0, NULL, 0)) { /* no-op */ }
    {
        ULONG total_read = 0;
        BOOL read_ok = TRUE;
        while (total_read < file_size) {
            LONG chunk = (file_size - total_read > 32768UL) ? 32768L : (LONG)(file_size - total_read);
            LONG r = Read(fh, buf + total_read, chunk);
            if (r <= 0) {
                read_ok = FALSE;
                break;
            }
            total_read += (ULONG)r;
        }
        if (read_ok) {
            ok = Amirs_SendPacket(client_sock, AMIRS_MSG_FRAME_DATA, 0, frame, buf, file_size);
        }
    }

    FreeVec(buf);
    Close(fh);
    return ok;
}

static BOOL run_render_job(ULONG frame, const char *scene_path, const char *out_path,
                           const char *cmd_template)
{
    char cmd_buf[512];
    LONG rc;

    /* Si no se pasa plantilla, usamos un comando por defecto que copia scene->out
     * para validar el transporte binario antes de enchufar lwsn/povray.
     */
    if (cmd_template && cmd_template[0] != '\0') {
        snprintf(cmd_buf, sizeof(cmd_buf), cmd_template, (unsigned long)frame, scene_path, out_path);
    } else {
        snprintf(cmd_buf, sizeof(cmd_buf), "Copy \"%s\" \"%s\"", scene_path, out_path);
    }

    printf("[WORKER] Ejecutando render frame %lu: %s\n", (unsigned long)frame, cmd_buf);

    rc = SystemTagList((STRPTR)cmd_buf, NULL);
    if (rc != 0) {
        printf("[WORKER] Comando de render devolvió rc=%ld\n", (long)rc);
        return FALSE;
    }

    return TRUE;
}

BOOL Worker_HandleDirectorSession(LONG listen_sock, const char *render_cmd_template)
{
    struct sockaddr_in peer_addr;
    LONG addr_len = sizeof(peer_addr);
    LONG csock;
    BOOL session_ok = TRUE;

    if (listen_sock < 0) {
        return FALSE;
    }

    csock = accept(listen_sock, (struct sockaddr *)&peer_addr, &addr_len);
    if (csock < 0) {
        printf("[WORKER] Error en accept()\n");
        return FALSE;
    }

    printf("[WORKER] Director conectado directamente\n");

    for (;;) {
        AmirsPacketHeader hdr;
        int hres = Amirs_RecvHeader(csock, &hdr);

        if (hres == 0) {
            printf("[WORKER] Director cerró sesión limpiamente\n");
            break;
        }
        if (hres < 0) {
            printf("[WORKER] Error leyendo cabecera binaria\n");
            session_ok = FALSE;
            break;
        }

        if (hdr.payload_len > MAX_PAYLOAD_BYTES) {
            const char *err_msg = "payload_too_large";
            Amirs_SendPacket(csock, AMIRS_MSG_ERR, 0, hdr.frame, err_msg, (ULONG)strlen(err_msg));
            session_ok = FALSE;
            break;
        }

        {
            UBYTE *payload = NULL;
            if (hdr.payload_len > 0) {
                payload = (UBYTE *)AllocVec(hdr.payload_len, MEMF_ANY);
                if (!payload) {
                    session_ok = FALSE;
                    break;
                }
                if (!Amirs_RecvExact(csock, payload, hdr.payload_len)) {
                    FreeVec(payload);
                    session_ok = FALSE;
                    break;
                }
            }

            if (hdr.type == AMIRS_MSG_JOB_INIT) {
                printf("[WORKER] JOB_INIT frame=%lu info=%.*s\n",
                       (unsigned long)hdr.frame,
                       (int)hdr.payload_len,
                       payload ? (const char *)payload : "");
            } else if (hdr.type == AMIRS_MSG_SCENE_CHUNK) {
                char scene_path[128];
                char out_path[128];

                snprintf(scene_path, sizeof(scene_path), "T:amirs_scene_%lu.dat", (unsigned long)hdr.frame);
                snprintf(out_path, sizeof(out_path), "T:amirs_out_%lu.dat", (unsigned long)hdr.frame);

                if (!write_file_all(scene_path, payload ? payload : (const UBYTE *)"", hdr.payload_len)) {
                    const char *err_msg = "write_scene_failed";
                    Amirs_SendPacket(csock, AMIRS_MSG_ERR, 0, hdr.frame, err_msg, (ULONG)strlen(err_msg));
                    session_ok = FALSE;
                } else if (!run_render_job(hdr.frame, scene_path, out_path, render_cmd_template)) {
                    const char *err_msg = "render_failed";
                    Amirs_SendPacket(csock, AMIRS_MSG_ERR, 0, hdr.frame, err_msg, (ULONG)strlen(err_msg));
                    session_ok = FALSE;
                } else {
                    Amirs_SendPacket(csock, AMIRS_MSG_RENDER_DONE, 0, hdr.frame, NULL, 0);
                    if (!send_file_as_frame(csock, hdr.frame, out_path)) {
                        session_ok = FALSE;
                    }
                }

                DeleteFile((STRPTR)scene_path);
                DeleteFile((STRPTR)out_path);
            }

            if (payload) {
                FreeVec(payload);
            }

            if (!session_ok) {
                break;
            }
        }
    }

    CloseSocket(csock);
    return session_ok;
}