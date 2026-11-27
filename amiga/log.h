#ifndef AMIRS_LOG_H
#define AMIRS_LOG_H

#include <exec/types.h>

/* Niveles de log, de menor a mayor gravedad.
 * LOG_LEVEL_INVALID sólo se usa como valor de retorno al parsear un nombre.
 */
typedef enum {
    LOG_LEVEL_INVALID = -1,
    LOG_LEVEL_DEBUG   = 0,
    LOG_LEVEL_INFO    = 1,
    LOG_LEVEL_WARN    = 2,
    LOG_LEVEL_ERROR   = 3
} LogLevel;

/* Longitud máxima del texto de un mensaje (sin timestamp ni etiquetas). */
#define LOG_MSG_MAX 512

/* Inicializa el subsistema de log.
 *
 *   level     : nivel mínimo que se escribe (LOG_LEVEL_DEBUG escribe todo).
 *   file_path : ruta opcional de un fichero de log; NULL = sólo consola.
 *               El fichero se abre en modo "append" (añade al final).
 */
void Log_Init(LogLevel level, const char *file_path);

/* Cierra el fichero de log (si lo hubiera) y libera la biblioteca de DOS. */
void Log_Shutdown(void);

/* TRUE si un mensaje de ese nivel se escribiría con la configuración actual. */
BOOL Log_Enabled(LogLevel level);

/* Escribe una línea de log con timestamp, nivel y etiqueta de componente. */
void Log_Write(LogLevel level, const char *tag, const char *fmt, ...);

/* Utilidades de nivel (usadas por el parseo de la línea de órdenes). */
const char *Log_LevelName(LogLevel level);
LogLevel     Log_LevelFromName(const char *name);

/* Macros de uso normal. El nivel se comprueba antes de formatear el mensaje
 * para no gastar CPU en cadenas que van a descartarse.
 */
#define LOG_DEBUG(tag, ...) \
    do { if (Log_Enabled(LOG_LEVEL_DEBUG)) Log_Write(LOG_LEVEL_DEBUG, (tag), __VA_ARGS__); } while (0)
#define LOG_INFO(tag, ...) \
    do { if (Log_Enabled(LOG_LEVEL_INFO)) Log_Write(LOG_LEVEL_INFO, (tag), __VA_ARGS__); } while (0)
#define LOG_WARN(tag, ...) \
    do { if (Log_Enabled(LOG_LEVEL_WARN)) Log_Write(LOG_LEVEL_WARN, (tag), __VA_ARGS__); } while (0)
#define LOG_ERROR(tag, ...) \
    do { if (Log_Enabled(LOG_LEVEL_ERROR)) Log_Write(LOG_LEVEL_ERROR, (tag), __VA_ARGS__); } while (0)

#endif /* AMIRS_LOG_H */
