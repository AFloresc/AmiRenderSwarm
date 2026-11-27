#include "net.h"
#include "log.h"

#include <exec/exec.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdio.h>

#ifndef FIONBIO
#define FIONBIO 0x8004667eUL
#endif

#ifndef EWOULDBLOCK
#define EWOULDBLOCK 35
#endif

#ifndef EAGAIN
#define EAGAIN EWOULDBLOCK
#endif

struct Library *SocketBase = NULL;

/* Copia una línea de señalización sin el '\n' final para poder registrarla. */
static void log_line_copy(const char *line, char *out, int max_len)
{
    int len;

    if (out == NULL || max_len < 1) {
        return;
    }

    out[0] = '\0';
    if (line == NULL) {
        return;
    }

    len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }

    if (len > max_len - 1) {
        len = max_len - 1;
    }

    memcpy(out, line, (size_t)len);
    out[len] = '\0';
}

void Net_FormatIPv4(ULONG s_addr, char *out, int max_len)
{
    ULONG host;

    if (out == NULL || max_len < 16) {
        return;
    }

    host = ntohl(s_addr);
    snprintf(out, (size_t)max_len, "%lu.%lu.%lu.%lu",
             (unsigned long)((host >> 24) & 0xFFUL),
             (unsigned long)((host >> 16) & 0xFFUL),
             (unsigned long)((host >>  8) & 0xFFUL),
             (unsigned long)( host        & 0xFFUL));
}

void Net_FormatPeer(const struct sockaddr_in *peer, char *out, int max_len)
{
    char ip[24];

    if (out == NULL || max_len < 1) {
        return;
    }

    out[0] = '\0';
    if (peer == NULL) {
        return;
    }

    Net_FormatIPv4(peer->sin_addr.s_addr, ip, sizeof(ip));
    snprintf(out, (size_t)max_len, "%s:%u", ip, (unsigned int)ntohs(peer->sin_port));
}

BOOL Net_Init(void)
{
    if (SocketBase != NULL) {
        LOG_DEBUG("NET", "bsdsocket.library ya estaba abierta");
        return TRUE;
    }

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4UL);
    if (SocketBase == NULL) {
        SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 0UL);
    }

    if (SocketBase == NULL) {
        LOG_ERROR("NET", "No se pudo abrir bsdsocket.library (Roadshow / AmiTCP)");
        return FALSE;
    }

    LOG_INFO("NET", "bsdsocket.library abierta correctamente");
    return TRUE;
}

void Net_Cleanup(void)
{
    if (SocketBase != NULL) {
        LOG_INFO("NET", "Cerrando bsdsocket.library");
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

BOOL Hub_Connect(HubConn *hc, const char *host, UWORD port)
{
    struct sockaddr_in addr;
    char ipbuf[24];
    LONG s;

    if (!hc || !host || host[0] == '\0' || port == 0 || SocketBase == NULL) {
        LOG_ERROR("NET", "Parámetros inválidos para conectar al Hub");
        return FALSE;
    }

    memset(hc, 0, sizeof(*hc));
    hc->sock = -1;

    LOG_INFO("NET", "Conectando al Hub %s:%u", host, (unsigned int)port);

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        LOG_ERROR("NET", "Error creando socket hacia el Hub (errno=%ld)", (long)Errno());
        return FALSE;
    }
    LOG_DEBUG("NET", "Socket TCP creado (fd=%ld)", (long)s);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_aton((STRPTR)host, &addr.sin_addr) == 0) {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
            LOG_ERROR("NET", "No se pudo resolver el host %s", host);
            CloseSocket(s);
            return FALSE;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
        Net_FormatIPv4(addr.sin_addr.s_addr, ipbuf, sizeof(ipbuf));
        LOG_INFO("NET", "Host %s resuelto a %s", host, ipbuf);
    } else {
        Net_FormatIPv4(addr.sin_addr.s_addr, ipbuf, sizeof(ipbuf));
        LOG_DEBUG("NET", "Dirección literal %s", ipbuf);
    }

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("NET", "Error conectando al Hub en %s:%u (errno=%ld)",
                  host, (unsigned int)port, (long)Errno());
        CloseSocket(s);
        return FALSE;
    }

    /* Socket no bloqueante para drenar líneas en Hub_ReadLine */
    {
        LONG nonblock = 1;
        IoctlSocket(s, (ULONG)FIONBIO, (char *)&nonblock);
        LOG_DEBUG("NET", "Socket pasado a modo no bloqueante");
    }

    hc->sock = s;
    LOG_INFO("NET", "Conectado al Hub en %s:%u (fd=%ld)", host, (unsigned int)port, (long)s);
    return TRUE;
}

void Hub_Disconnect(HubConn *hc)
{
    if (hc && hc->sock >= 0) {
        LOG_INFO("NET", "Cerrando la conexión con el Hub (fd=%ld)", (long)hc->sock);
        CloseSocket(hc->sock);
        hc->sock = -1;
    }
    if (hc) {
        hc->rx_len = 0;
    }
}

BOOL Hub_SendLine(HubConn *hc, const char *line)
{
    const char *p;
    LONG remaining;
    char tmp[HUB_LINE_BUF_SIZE + 2];
    char logbuf[HUB_LINE_BUF_SIZE + 2];
    size_t len;

    if (!hc || hc->sock < 0 || !line) {
        return FALSE;
    }

    len = strlen(line);
    if (len == 0) {
        return TRUE;
    }

    if (len >= sizeof(tmp) - 2) {
        len = sizeof(tmp) - 2;
    }

    memcpy(tmp, line, len);
    if (tmp[len - 1] != '\n') {
        tmp[len++] = '\n';
    }
    tmp[len] = '\0';

    log_line_copy(tmp, logbuf, sizeof(logbuf));
    LOG_INFO("NET", "TX -> %s", logbuf);

    p = tmp;
    remaining = (LONG)len;

    while (remaining > 0) {
        LONG sent = send(hc->sock, (APTR)p, remaining, 0);
        if (sent < 0) {
            LONG err = Errno();
            if (err == EWOULDBLOCK || err == EAGAIN) {
                LOG_DEBUG("NET", "TX en espera (EWOULDBLOCK), reintentando");
                continue;
            }
            LOG_ERROR("NET", "Error enviando al Hub (errno=%ld): %s", (long)err, logbuf);
            return FALSE;
        }
        if (sent == 0) {
            LOG_ERROR("NET", "El Hub cerró la conexión al enviar: %s", logbuf);
            return FALSE;
        }
        p += sent;
        remaining -= sent;
    }

    return TRUE;
}

int Hub_ReadLine(HubConn *hc, char *out_line, int max_len)
{
    int i;

    if (!hc || hc->sock < 0 || !out_line || max_len < 2) {
        return -1;
    }

    /* Intentar leer más bytes si hay hueco en el buffer */
    if (hc->rx_len < (int)sizeof(hc->rx_buf) - 1) {
        LONG n = recv(hc->sock,
                      (APTR)(hc->rx_buf + hc->rx_len),
                      (LONG)(sizeof(hc->rx_buf) - 1 - (size_t)hc->rx_len),
                      0);
        if (n > 0) {
            hc->rx_len += (int)n;
            hc->rx_buf[hc->rx_len] = '\0';
            LOG_DEBUG("NET", "RX <- %ld bytes del Hub (buffer=%d bytes)",
                      (long)n, hc->rx_len);
        } else if (n == 0) {
            LOG_INFO("NET", "EOF del Hub (cierre limpio de la conexión)");
            return -1; /* EOF limpio del Hub */
        } else {
            LONG err = Errno();
            if (err != EWOULDBLOCK && err != EAGAIN) {
                LOG_ERROR("NET", "Error leyendo del Hub (errno=%ld)", (long)err);
                return -1;
            }
            LOG_DEBUG("NET", "RX sin datos disponibles todavía (EWOULDBLOCK)");
        }
    }

    /* Buscar '\n' en rx_buf */
    for (i = 0; i < hc->rx_len; i++) {
        if (hc->rx_buf[i] == '\n') {
            int line_len = i;
            int consume = i + 1;
            int rem;

            if (line_len > 0 && hc->rx_buf[line_len - 1] == '\r') {
                line_len--;
            }

            if (line_len >= max_len) {
                line_len = max_len - 1;
            }

            memcpy(out_line, hc->rx_buf, (size_t)line_len);
            out_line[line_len] = '\0';

            rem = hc->rx_len - consume;
            if (rem > 0) {
                memmove(hc->rx_buf, hc->rx_buf + consume, (size_t)rem);
            }
            hc->rx_len = rem;
            hc->rx_buf[hc->rx_len] = '\0';

            {
                char logbuf[HUB_LINE_BUF_SIZE + 2];
                log_line_copy(out_line, logbuf, sizeof(logbuf));
                LOG_DEBUG("NET", "Línea completa del Hub (%d bytes): %s", line_len, logbuf);
            }

            return line_len > 0 ? line_len : 1;
        }
    }

    return 0;
}