#include "log.h"

#include <exec/exec.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Desplazamiento (LVO) de DateStamp() dentro de dos.library en AmigaOS 3.x.
 * Se llama así porque el "DOSBase" del entorno libnix no es necesariamente el
 * dos.library real del sistema.
 */
#define AMIRS_LVO_DATESTAMP (-192L)

/* Días entre 1970-01-01 (epoch Unix) y 1978-01-01 (epoch DOS). */
#define AMIRS_DOS_EPOCH_OFFSET 2922L

/* Longitud máxima de una línea de log completa (timestamp + etiquetas + texto). */
#define LOG_LINE_MAX 768

typedef struct DateStamp *(*DateStampFn)(struct DateStamp *);

static struct Library *g_dos_base     = NULL;
static DateStampFn    g_datetime     = NULL;
static LogLevel       g_level        = LOG_LEVEL_DEBUG;
static BPTR           g_log_fh       = 0;
static LONG           g_start_days   = 0;
static LONG           g_start_minute = 0;
static LONG           g_start_tick   = 0;
static BOOL           g_started      = FALSE;
static ULONG          g_lines        = 0;

/* Convierte días desde 1970-01-01 a fecha civil (algoritmo de Howard Hinnant).
 * Se usa porque el entorno de compilación cruzada no trae datos de zona
 * horaria utilizables para localtime().
 */
static void civil_from_days(LONG z, int *out_year, int *out_month, int *out_day)
{
    LONG era, doe, yoe, y, doy, mp;
    int  d, m;

    z += 719468L;
    era = (z >= 0 ? z : z - 146096L) / 146097L;
    doe = z - era * 146097L;
    yoe = (doe - doe / 1460L + doe / 36524L - doe / 146096L) / 365L;
    y   = yoe + era * 400L;
    doy = doe - (365L * yoe + yoe / 4L - yoe / 100L);
    mp  = (5L * doy + 2L) / 153L;

    d = (int)(doy - (153L * mp + 2L) / 5L + 1L);
    m = (int)(mp + (mp < 10L ? 3L : -9L));
    y += (m <= 2 ? 1L : 0L);

    *out_year  = (int)y;
    *out_month = m;
    *out_day   = d;
}

/* Lee la hora actual del dos.library del sistema. */
static BOOL log_now(struct DateStamp *out_ds)
{
    if (g_datetime == NULL || out_ds == NULL) {
        return FALSE;
    }

    memset(out_ds, 0, sizeof(*out_ds));
    g_datetime(out_ds);
    return TRUE;
}

/* Formatea "AAAA-MM-DD HH:MM:SS.mmm" a partir del DateStamp de DOS. */
static void format_timestamp(char *out, int max_len)
{
    struct DateStamp ds;
    LONG days, mins, ticks, secs;
    int  year, month, day;

    memset(&ds, 0, sizeof(ds));
    if (!log_now(&ds)) {
        snprintf(out, (size_t)max_len, "0000-00-00 00:00:00.000");
        return;
    }

    days  = ds.ds_Days + AMIRS_DOS_EPOCH_OFFSET;
    mins  = ds.ds_Minute;
    ticks = ds.ds_Tick;
    secs  = mins * 60L + ticks / TICKS_PER_SECOND;

    civil_from_days(days, &year, &month, &day);

    snprintf(out, (size_t)max_len, "%04d-%02d-%02d %02ld:%02ld:%02ld.%03ld",
             year, month, day,
             secs / 3600L,
             (secs / 60L) % 60L,
             secs % 60L,
             (ticks % TICKS_PER_SECOND) * 1000L / TICKS_PER_SECOND);
}

/* Segundos transcurridos desde Log_Init(). */
static LONG log_elapsed_seconds(void)
{
    struct DateStamp ds;
    LONG dd, dm, dt;

    if (!g_started) {
        return 0L;
    }

    memset(&ds, 0, sizeof(ds));
    if (!log_now(&ds)) {
        return 0L;
    }

    dd = ds.ds_Days   - g_start_days;
    dm = ds.ds_Minute - g_start_minute;
    dt = ds.ds_Tick   - g_start_tick;

    return dd * 86400L + dm * 60L + dt / TICKS_PER_SECOND;
}

/* Abre el fichero de log en modo "añadir al final".
 *
 * Se usa la API de DOS (y no fopen()) a propósito: fopen() arrastra la
 * maquinaria de ficheros de la biblioteca C de este entorno, que exige
 * Nudos.library y Nuutility.library en el Amiga. Con Open()/Write() sólo
 * hace falta dos.library, que el agente ya usa de todas formas.
 */
static BOOL log_open_file(const char *path)
{
    BPTR fh;

    fh = Open((STRPTR)path, MODE_READWRITE);
    if (fh == 0) {
        return FALSE;
    }

    /* MODE_READWRITE no trunca el fichero: nos situamos al final para
     * continuar el log donde lo dejó la ejecución anterior.
     */
    if (Seek(fh, 0L, OFFSET_END) < 0L) {
        printf("No se pudo posicionar al final de %s (IoErr=%ld)\n", path, (long)IoErr());
    }

    g_log_fh = fh;
    return TRUE;
}

/* Escribe una línea en el fichero de log (el texto ya incluye el salto final). */
static void log_write_file(const char *text)
{
    LONG len;

    if (g_log_fh == 0 || text == NULL) {
        return;
    }

    len = (LONG)strlen(text);
    if (len > 0 && Write(g_log_fh, (APTR)text, len) != len) {
        /* Si el disco falla no hay mucho más que hacer: no podemos
         * escribir el error de log dentro del propio log.
         */
        Close(g_log_fh);
        g_log_fh = 0;
    }
}

void Log_Init(LogLevel level, const char *file_path)
{
    struct DateStamp ds;

    g_level = level;

    if (g_dos_base == NULL) {
        g_dos_base = OpenLibrary((STRPTR)"dos.library", 0UL);
        if (g_dos_base != NULL) {
            g_datetime = (DateStampFn)(((LONG)g_dos_base) + AMIRS_LVO_DATESTAMP);
        } else {
            printf("No se pudo abrir dos.library: las marcas de tiempo quedaran a cero\n");
        }
    }

    memset(&ds, 0, sizeof(ds));
    if (log_now(&ds)) {
        g_start_days   = ds.ds_Days;
        g_start_minute = ds.ds_Minute;
        g_start_tick   = ds.ds_Tick;
        g_started      = TRUE;
    }

    /* Salida por líneas para que la consola muestre cada mensaje al instante. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (file_path != NULL && file_path[0] != '\0') {
        if (!log_open_file(file_path)) {
            printf("No se pudo abrir el fichero de log %s (IoErr=%ld)\n",
                   file_path, (long)IoErr());
        }
    }
}

void Log_Shutdown(void)
{
    char ts[32];

    if (g_log_fh != 0) {
        char line[LOG_LINE_MAX];
        format_timestamp(ts, sizeof(ts));
        snprintf(line, sizeof(line), "%s [T+%06lds] [INFO ] [LOG ] Fin del log (%lu líneas escritas)\n",
                 ts, (long)log_elapsed_seconds(), (unsigned long)g_lines);
        log_write_file(line);
        Close(g_log_fh);
        g_log_fh = 0;
    }

    if (g_dos_base != NULL) {
        CloseLibrary(g_dos_base);
        g_dos_base = NULL;
        g_datetime = NULL;
    }
}

BOOL Log_Enabled(LogLevel level)
{
    return (level >= g_level) ? TRUE : FALSE;
}

void Log_Write(LogLevel level, const char *tag, const char *fmt, ...)
{
    char ts[32];
    char msg[LOG_MSG_MAX];
    char line[LOG_LINE_MAX];
    va_list ap;

    if (level < g_level) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    format_timestamp(ts, sizeof(ts));

    snprintf(line, sizeof(line), "%s [T+%06lds] [%-5s] [%s] %s\n",
             ts,
             (long)log_elapsed_seconds(),
             Log_LevelName(level),
             (tag != NULL && tag[0] != '\0') ? tag : "----",
             msg);

    printf("%s", line);
    fflush(stdout);

    log_write_file(line);

    g_lines++;
}

const char *Log_LevelName(LogLevel level)
{
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default:              return "?????";
    }
}

static int ascii_lower(int c)
{
    return ((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c;
}

static BOOL name_equals(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return FALSE;
    }

    while (*a != '\0' && *b != '\0') {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) {
            return FALSE;
        }
        a++;
        b++;
    }

    return ((*a == '\0') && (*b == '\0')) ? TRUE : FALSE;
}

LogLevel Log_LevelFromName(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return LOG_LEVEL_INVALID;
    }

    if (name_equals(name, "debug") || name_equals(name, "dbg") || name_equals(name, "verbose")) {
        return LOG_LEVEL_DEBUG;
    }
    if (name_equals(name, "info")) {
        return LOG_LEVEL_INFO;
    }
    if (name_equals(name, "warn") || name_equals(name, "warning")) {
        return LOG_LEVEL_WARN;
    }
    if (name_equals(name, "error") || name_equals(name, "err")) {
        return LOG_LEVEL_ERROR;
    }

    return LOG_LEVEL_INVALID;
}
