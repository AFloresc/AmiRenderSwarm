#ifndef AMIRS_WORKER_H
#define AMIRS_WORKER_H

#include <exec/types.h>

BOOL Worker_StartListener(UWORD listen_port, LONG *out_listen_sock);
void Worker_StopListener(LONG *listen_sock);

/* Acepta una conexión entrante de un director y atiende su sesión binaria.
 * Devuelve TRUE si se completó limpiamente, FALSE en error.
 */
BOOL Worker_HandleDirectorSession(LONG listen_sock, const char *render_cmd_template);

#endif /* AMIRS_WORKER_H */