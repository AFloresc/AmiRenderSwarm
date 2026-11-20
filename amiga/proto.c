#include "proto.h"

#include <proto/bsdsocket.h>
#include <string.h>
#include <stdio.h>

extern struct Library *SocketBase;

BOOL Amirs_SendExact(LONG sock, const void *buf, ULONG len)
{
    const UBYTE *p = (const UBYTE *)buf;
    ULONG remaining = len;

    if (sock < 0) {
        return FALSE;
    }

    while (remaining > 0) {
        LONG chunk = (remaining > 65536UL) ? 65536L : (LONG)remaining;
        LONG n = send(sock, (APTR)p, chunk, 0);
        if (n <= 0) {
            return FALSE;
        }
        p += (ULONG)n;
        remaining -= (ULONG)n;
    }
    return TRUE;
}

BOOL Amirs_RecvExact(LONG sock, void *buf, ULONG len)
{
    UBYTE *p = (UBYTE *)buf;
    ULONG remaining = len;

    if (sock < 0) {
        return FALSE;
    }

    while (remaining > 0) {
        LONG chunk = (remaining > 65536UL) ? 65536L : (LONG)remaining;
        LONG n = recv(sock, (APTR)p, chunk, 0);
        if (n <= 0) {
            return FALSE;
        }
        p += (ULONG)n;
        remaining -= (ULONG)n;
    }
    return TRUE;
}

BOOL Amirs_SendPacket(LONG sock, UBYTE type, UBYTE flags, ULONG frame,
                      const void *payload, ULONG payload_len)
{
    UBYTE wire[16];
    ULONG net_magic = htonl(AMIRS_MAGIC);
    ULONG net_frame = htonl(frame);
    ULONG net_len   = htonl(payload_len);

    memcpy(wire + 0,  &net_magic, 4);
    wire[4] = type;
    wire[5] = flags;
    wire[6] = 0;
    wire[7] = 0;
    memcpy(wire + 8,  &net_frame, 4);
    memcpy(wire + 12, &net_len,   4);

    if (!Amirs_SendExact(sock, wire, sizeof(wire))) {
        return FALSE;
    }

    if (payload_len > 0 && payload != NULL) {
        if (!Amirs_SendExact(sock, payload, payload_len)) {
            return FALSE;
        }
    }

    return TRUE;
}

int Amirs_RecvHeader(LONG sock, AmirsPacketHeader *out_hdr)
{
    UBYTE wire[16];
    LONG first = recv(sock, (APTR)wire, 1, 0);
    if (first == 0) {
        return 0; /* cierre limpio */
    }
    if (first < 0) {
        return -1;
    }

    if (!Amirs_RecvExact(sock, wire + 1, 15)) {
        return -1;
    }

    {
        ULONG raw_magic, raw_frame, raw_len;
        UWORD raw_res;

        memcpy(&raw_magic, wire + 0, 4);
        out_hdr->type     = wire[4];
        out_hdr->flags    = wire[5];
        memcpy(&raw_res,   wire + 6, 2);
        memcpy(&raw_frame, wire + 8, 4);
        memcpy(&raw_len,   wire + 12, 4);

        out_hdr->magic       = ntohl(raw_magic);
        out_hdr->reserved    = ntohs(raw_res);
        out_hdr->frame       = ntohl(raw_frame);
        out_hdr->payload_len = ntohl(raw_len);
    }

    if (out_hdr->magic != AMIRS_MAGIC) {
        printf("[PROTO] Magic inválido: 0x%08lx\n", (unsigned long)out_hdr->magic);
        return -1;
    }

    return 1;
}