/* Logging.
 *
 * One line per call, no allocation in the formatting path beyond the stack
 * buffer, no locks: log from the thread that owns the message. The shell may
 * install a sink to mirror lines into a console panel.
 */
#ifndef WADA_LOG_H
#define WADA_LOG_H

#include "wada.h"

typedef enum wada_log_level {
    WADA_LOG_DEBUG = 0,
    WADA_LOG_INFO = 1,
    WADA_LOG_WARN = 2,
    WADA_LOG_ERROR = 3
} wada_log_level;

typedef void (*wada_log_sink_fn)(void *user, wada_log_level level, const char *line);

WADA_API void wada_log_set_level(wada_log_level level);
WADA_API wada_log_level wada_log_level_get(void);
WADA_API void wada_log_set_sink(wada_log_sink_fn fn, void *user);
WADA_API void wada_logf(wada_log_level level, const char *tag, const char *fmt, ...) WADA_PRINTF(3, 4);

#define WADA_LOGD(tag, ...) wada_logf(WADA_LOG_DEBUG, (tag), __VA_ARGS__)
#define WADA_LOGI(tag, ...) wada_logf(WADA_LOG_INFO, (tag), __VA_ARGS__)
#define WADA_LOGW(tag, ...) wada_logf(WADA_LOG_WARN, (tag), __VA_ARGS__)
#define WADA_LOGE(tag, ...) wada_logf(WADA_LOG_ERROR, (tag), __VA_ARGS__)

#endif /* WADA_LOG_H */
