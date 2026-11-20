#include "net.h"

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

BOOL Net_Init(void)
{
    if (SocketBase != NULL) {
        return TRUE;
    }

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4UL);
    if (SocketBase == NULL) {
        SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 0UL);
    }

    if (SocketBase == NULL) {
        printf("[NET] No se pudo abrir bsdsocket.library (Roadshow / AmiTCP)\n");
        return FALSE;
    }

    return TRUE;
}

void Net_Cleanup(void)
{
    if (SocketBase != NULL) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

BOOL Hub_Connect(HubConn *hc, const char *host, UWORD port)
{
    struct sockaddr_in addr;
    LONG s;

    if (!hc || !host || host[0] == '\0' || port == 0 || SocketBase == NULL) {
        return FALSE;
    }

    memset(hc, 0, sizeof(*hc));
    hc->sock = -1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("[NET] Error creando socket hacia el Hub\n");
        return FALSE;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_aton((STRPTR)host, &addr.sin_addr) == 0) {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
            printf("[NET] No se pudo resolver host %s\n", host);
            CloseSocket(s);
            return FALSE;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    }

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[NET] Error conectando al Hub en %s:%u\n", host, (unsigned int)port);
        CloseSocket(s);
        return FALSE;
    }

    /* Socket no bloqueante para drenar líneas en Hub_ReadLine */
    {
        LONG nonblock = 1;
        IoctlSocket(s, (ULONG)FIONBIO, (char *)&nonblock);
    }

    hc->sock = s;
    printf("[NET] Conectado al Hub en %s:%u\n", host, (unsigned int)port);
    return TRUE;
}

void Hub_Disconnect(HubConn *hc)
{
    if (hc && hc->sock >= 0) {
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

    p = tmp;
    remaining = (LONG)len;

    while (remaining > 0) {
        LONG sent = send(hc->sock, (APTR)p, remaining, 0);
        if (sent < 0) {
            LONG err = Errno();
            if (err == EWOULDBLOCK || err == EAGAIN) {
                continue;
            }
            return FALSE;
        }
        if (sent == 0) {
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
        } else if (n == 0) {
            return -1; /* EOF limpio del Hub */
        } else {
            LONG err = Errno();
            if (err != EWOULDBLOCK && err != EAGAIN) {
                return -1;
            }
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

            return line_len > 0 ? line_len : 1;
        }
    }

    return 0;
}