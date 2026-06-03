/*
 * log_stub.c — host-side replacement for openomf-master/src/utils/log.c.
 *
 * The original log.c pulls in <SDL_mutex.h> for thread-safe logging. For the
 * port_3do host pipeline we're building a single-threaded asset converter and
 * don't want to drag in SDL2 just to keep its mutex API happy.
 *
 * This stub provides the same symbol surface as utils/log.h, routing all
 * messages to stderr without locks. log_init/log_close/log_set_level/etc.
 * are no-ops because we don't need configurable outputs.
 */

#include "utils/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static log_level g_min_level = LOG_INFO;
static char g_last_error[512] = {0};

void log_init(void) {}
void log_close(void) {}
void log_set_colors(bool toggle) { (void)toggle; }
void log_add_stderr(log_level level, bool colors) { (void)level; (void)colors; }
void log_add_file(const char *filename, log_level level) { (void)filename; (void)level; }

void log_set_level(log_level level) {
    g_min_level = level;
}

void log_msg(log_level level, const char *fmt, ...) {
    va_list ap;
    const char *tag;
    if (level < g_min_level) {
        return;
    }
    switch (level) {
        case LOG_DEBUG: tag = "DEBUG"; break;
        case LOG_INFO:  tag = "INFO";  break;
        case LOG_WARN:  tag = "WARN";  break;
        case LOG_ERROR: tag = "ERROR"; break;
        default:        tag = "?";     break;
    }
    fprintf(stderr, "[%s] ", tag);
    va_start(ap, fmt);
    if (level == LOG_ERROR) {
        /* keep a copy for log_last_error */
        vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
        va_end(ap);
        fputs(g_last_error, stderr);
    } else {
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
}

log_level log_level_text_to_enum(const char *level, log_level default_value) {
    if (level == NULL) return default_value;
    if (strcasecmp(level, "DEBUG") == 0) return LOG_DEBUG;
    if (strcasecmp(level, "INFO")  == 0) return LOG_INFO;
    if (strcasecmp(level, "WARN")  == 0) return LOG_WARN;
    if (strcasecmp(level, "ERROR") == 0) return LOG_ERROR;
    return default_value;
}

bool is_log_level(const char *level) {
    if (level == NULL) return false;
    return strcasecmp(level, "DEBUG") == 0 ||
           strcasecmp(level, "INFO")  == 0 ||
           strcasecmp(level, "WARN")  == 0 ||
           strcasecmp(level, "ERROR") == 0;
}

const char *log_last_error(void) {
    return g_last_error[0] ? g_last_error : NULL;
}
