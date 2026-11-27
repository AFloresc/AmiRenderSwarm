#ifndef AMIRS_PROTO_H
#define AMIRS_PROTO_H

#include <exec/types.h>
#include "net.h"

#define AMIRS_MAGIC 0x414D4952UL /* "AMIR" */

enum {
    AMIRS_MSG_JOB_INIT    = 1,
    AMIRS_MSG_SCENE_CHUNK = 2,
    AMIRS_MSG_RENDER_DONE = 3,
    AMIRS_MSG_FRAME_DATA  = 4,
    AMIRS_MSG_ERR         = 5
};

/* Cabecera binaria fija de 16 bytes en wire format (big-endian) */
typedef struct {
    ULONG magic;       /* 0..3:   0x414D4952 ("AMIR") */
    UBYTE type;        /* 4:      tipo de mensaje */
    UBYTE flags;       /* 5:      flags */
    UWORD reserved;    /* 6..7:   reservado */
    ULONG frame;       /* 8..11:  número de frame (network byte order) */
    ULONG payload_len; /* 12..15: tamaño del payload (network byte order) */
} AmirsPacketHeader;

BOOL Amirs_SendPacket(LONG sock, UBYTE type, UBYTE flags, ULONG frame,
                      const void *payload, ULONG payload_len);

/* Nombre legible de un tipo de mensaje binario ("JOB_INIT", "SCENE_CHUNK"...). */
const char *Amirs_MsgTypeName(UBYTE type);

/* Lee una cabecera de 16 bytes y valida magic.
 * Devuelve:
 *   1  -> cabecera válida leída en out_hdr (en host byte order)
 *   0  -> EOF limpio antes de leer cabecera
 *  -1  -> error de red o magic inválido
 */
int Amirs_RecvHeader(LONG sock, AmirsPacketHeader *out_hdr);

/* Lee exactamente len bytes de sock en buf */
BOOL Amirs_RecvExact(LONG sock, void *buf, ULONG len);

/* Envía exactamente len bytes de buf por sock */
BOOL Amirs_SendExact(LONG sock, const void *buf, ULONG len);

#endif /* AMIRS_PROTO_H */