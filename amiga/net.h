#ifndef AMIRS_NET_H
#define AMIRS_NET_H

#include <exec/types.h>

#define HUB_LINE_BUF_SIZE 512

/* Forward declaration: sólo se usa como puntero, no hace falta el tipo completo. */
struct sockaddr_in;

typedef struct {
    LONG sock;
    char rx_buf[HUB_LINE_BUF_SIZE];
    int  rx_len;
} HubConn;

BOOL Net_Init(void);
void Net_Cleanup(void);

BOOL Hub_Connect(HubConn *hc, const char *host, UWORD port);
void Hub_Disconnect(HubConn *hc);

BOOL Hub_SendLine(HubConn *hc, const char *line);

/* Formatea una dirección IPv4 (en network byte order) como "a.b.c.d". */
void Net_FormatIPv4(ULONG s_addr, char *out, int max_len);

/* Formatea el peer de una sockaddr_in como "a.b.c.d:puerto". */
void Net_FormatPeer(const struct sockaddr_in *peer, char *out, int max_len);

/* Lee una línea terminada en '\n' (sin '\r' ni '\n').
 * Devuelve:
 *   >0 -> longitud de la línea leída
 *    0 -> aún no hay línea completa disponible
 *   -1 -> error o conexión cerrada por el servidor
 */
int Hub_ReadLine(HubConn *hc, char *out_line, int max_len);

#endif /* AMIRS_NET_H */