#include "worker.h"
#include "proto.h"
#include "net.h"
#include "log.h"

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
        LOG_ERROR("WORKER", "No se puede iniciar el listener (socket base no disponible)");
        return FALSE;
    }

    *out_listen_sock = -1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        LOG_ERROR("WORKER", "Error creando el socket de escucha (errno=%ld)", (long)Errno());
        return FALSE;
    }
    LOG_DEBUG("WORKER", "Socket de escucha creado (fd=%ld)", (long)s);

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(listen_port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("WORKER", "Error en bind() del puerto %u (errno=%ld)",
                  (unsigned int)listen_port, (long)Errno());
        CloseSocket(s);
        return FALSE;
    }
    LOG_DEBUG("WORKER", "bind() correcto en el puerto %u", (unsigned int)listen_port);

    if (listen(s, 4) < 0) {
        LOG_ERROR("WORKER", "Error en listen() (errno=%ld)", (long)Errno());
        CloseSocket(s);
        return FALSE;
    }

    *out_listen_sock = s;
    LOG_INFO("WORKER", "Escuchando conexiones directas en el puerto %u", (unsigned int)listen_port);
    return TRUE;
}

void Worker_StopListener(LONG *listen_sock)
{
    if (listen_sock && *listen_sock >= 0) {
        LOG_INFO("WORKER", "Cerrando el socket de escucha (fd=%ld)", (long)*listen_sock);
        CloseSocket(*listen_sock);
        *listen_sock = -1;
    }
}

static BOOL write_file_all(const char *path, const UBYTE *data, ULONG len)
{
    BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
    ULONG written = 0;

    if (!fh) {
        LOG_ERROR("WORKER", "No se pudo crear el fichero %s (IoErr=%ld)", path, (long)IoErr());
        return FALSE;
    }

    LOG_DEBUG("WORKER", "Escribiendo %lu bytes en %s", (unsigned long)len, path);

    while (written < len) {
        LONG chunk = (len - written > 32768UL) ? 32768L : (LONG)(len - written);
        LONG w = Write(fh, (APTR)(data + written), chunk);
        if (w <= 0) {
            LOG_ERROR("WORKER", "Error escribiendo en %s tras %lu bytes (IoErr=%ld)",
                      path, (unsigned long)written, (long)IoErr());
            Close(fh);
            return FALSE;
        }
        written += (ULONG)w;
    }

    Close(fh);
    LOG_INFO("WORKER", "Fichero %s escrito correctamente (%lu bytes)", path, (unsigned long)written);
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
        LOG_ERROR("WORKER", "No se pudo abrir %s para enviarlo (IoErr=%ld)", out_path, (long)IoErr());
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
        LOG_ERROR("WORKER", "Tamaño inválido en %s: %lu bytes (límite=%lu bytes)",
                  out_path, (unsigned long)file_size, (unsigned long)MAX_PAYLOAD_BYTES);
        Close(fh);
        return FALSE;
    }

    LOG_INFO("WORKER", "Enviando %s como FRAME_DATA frame=%lu (%lu bytes)",
             out_path, (unsigned long)frame, (unsigned long)file_size);

    buf = (UBYTE *)AllocVec(file_size, MEMF_ANY);
    if (!buf) {
        LOG_ERROR("WORKER", "Sin memoria para leer %lu bytes de %s",
                  (unsigned long)file_size, out_path);
        Close(fh);
        return FALSE;
    }

    {
        ULONG total_read = 0;
        BOOL read_ok = TRUE;
        while (total_read < file_size) {
            LONG chunk = (file_size - total_read > 32768UL) ? 32768L : (LONG)(file_size - total_read);
            LONG r = Read(fh, buf + total_read, chunk);
            if (r <= 0) {
                LOG_ERROR("WORKER", "Error leyendo %s tras %lu bytes (IoErr=%ld)",
                          out_path, (unsigned long)total_read, (long)IoErr());
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

    LOG_INFO("WORKER", "Ejecutando render del frame %lu: %s", (unsigned long)frame, cmd_buf);

    rc = SystemTagList((STRPTR)cmd_buf, NULL);
    if (rc != 0) {
        LOG_ERROR("WORKER", "El comando de render devolvió rc=%ld: %s", (long)rc, cmd_buf);
        return FALSE;
    }

    LOG_INFO("WORKER", "Render del frame %lu finalizado correctamente", (unsigned long)frame);
    return TRUE;
}

BOOL Worker_HandleDirectorSession(LONG listen_sock, const char *render_cmd_template)
{
    struct sockaddr_in peer_addr;
    char peer_str[32];
    LONG addr_len = (LONG)sizeof(peer_addr);
    LONG csock;
    BOOL session_ok = TRUE;

    if (listen_sock < 0) {
        LOG_ERROR("WORKER", "No hay socket de escucha disponible");
        return FALSE;
    }

    LOG_INFO("WORKER", "Esperando una conexión directa de un director...");

    csock = accept(listen_sock, (struct sockaddr *)&peer_addr, (LONG *)&addr_len);
    if (csock < 0) {
        LOG_ERROR("WORKER", "Error en accept() (errno=%ld)", (long)Errno());
        return FALSE;
    }

    Net_FormatPeer(&peer_addr, peer_str, sizeof(peer_str));
    LOG_INFO("WORKER", "Director conectado directamente desde %s (fd=%ld)", peer_str, (long)csock);

    for (;;) {
        AmirsPacketHeader hdr;
        int hres = Amirs_RecvHeader(csock, &hdr);

        if (hres == 0) {
            LOG_INFO("WORKER", "El director cerró la sesión limpiamente");
            break;
        }
        if (hres < 0) {
            LOG_ERROR("WORKER", "Error leyendo la cabecera binaria del director");
            session_ok = FALSE;
            break;
        }

        if (hdr.payload_len > MAX_PAYLOAD_BYTES) {
            const char *err_msg = "payload_too_large";
            LOG_ERROR("WORKER", "Payload demasiado grande: %lu bytes (límite=%lu bytes)",
                      (unsigned long)hdr.payload_len, (unsigned long)MAX_PAYLOAD_BYTES);
            Amirs_SendPacket(csock, AMIRS_MSG_ERR, 0, hdr.frame, err_msg, (ULONG)strlen(err_msg));
            session_ok = FALSE;
            break;
        }

        {
            UBYTE *payload = NULL;
            if (hdr.payload_len > 0) {
                payload = (UBYTE *)AllocVec(hdr.payload_len, MEMF_ANY);
                if (!payload) {
                    LOG_ERROR("WORKER", "Sin memoria para un payload de %lu bytes",
                              (unsigned long)hdr.payload_len);
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
                LOG_INFO("WORKER", "JOB_INIT frame=%lu info=%.*s",
                         (unsigned long)hdr.frame,
                         (int)hdr.payload_len,
                         payload ? (const char *)payload : "");
            } else if (hdr.type == AMIRS_MSG_SCENE_CHUNK) {
                char scene_path[128];
                char out_path[128];

                snprintf(scene_path, sizeof(scene_path), "T:amirs_scene_%lu.dat", (unsigned long)hdr.frame);
                snprintf(out_path, sizeof(out_path), "T:amirs_out_%lu.dat", (unsigned long)hdr.frame);

                LOG_INFO("WORKER", "SCENE_CHUNK frame=%lu (%lu bytes) -> %s",
                         (unsigned long)hdr.frame, (unsigned long)hdr.payload_len, scene_path);

                if (!write_file_all(scene_path, payload ? payload : (const UBYTE *)"", hdr.payload_len)) {
                    const char *err_msg = "write_scene_failed";
                    LOG_ERROR("WORKER", "No se pudo escribir la escena %s", scene_path);
                    Amirs_SendPacket(csock, AMIRS_MSG_ERR, 0, hdr.frame, err_msg, (ULONG)strlen(err_msg));
                    session_ok = FALSE;
                } else if (!run_render_job(hdr.frame, scene_path, out_path, render_cmd_template)) {
                    const char *err_msg = "render_failed";
                    LOG_ERROR("WORKER", "El render del frame %lu falló", (unsigned long)hdr.frame);
                    Amirs_SendPacket(csock, AMIRS_MSG_ERR, 0, hdr.frame, err_msg, (ULONG)strlen(err_msg));
                    session_ok = FALSE;
                } else {
                    LOG_INFO("WORKER", "Enviando RENDER_DONE frame=%lu", (unsigned long)hdr.frame);
                    Amirs_SendPacket(csock, AMIRS_MSG_RENDER_DONE, 0, hdr.frame, NULL, 0);
                    if (!send_file_as_frame(csock, hdr.frame, out_path)) {
                        LOG_ERROR("WORKER", "No se pudo enviar el frame %lu", (unsigned long)hdr.frame);
                        session_ok = FALSE;
                    }
                }

                LOG_DEBUG("WORKER", "Borrando los ficheros temporales %s y %s", scene_path, out_path);
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

    LOG_INFO("WORKER", "Cerrando la conexión con el director (fd=%ld)", (long)csock);
    CloseSocket(csock);
    return session_ok;
}