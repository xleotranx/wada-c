#include "wada_log.h"

#include <stdio.h>

static wada_log_level g_level = WADA_LOG_INFO;
static wada_log_sink_fn g_sink = NULL;
static void *g_sink_user = NULL;

void wada_log_set_level(wada_log_level level) {
    g_level = level;
}

wada_log_level wada_log_level_get(void) {
    return g_level;
}

void wada_log_set_sink(wada_log_sink_fn fn, void *user) {
    g_sink = fn;
    g_sink_user = user;
}

static const char *level_name(wada_log_level level) {
    switch (level) {
    case WADA_LOG_DEBUG: return "debug";
    case WADA_LOG_INFO: return "info";
    case WADA_LOG_WARN: return "warn";
    case WADA_LOG_ERROR: return "error";
    }
    return "log";
}

void wada_logf(wada_log_level level, const char *tag, const char *fmt, ...) {
    if ((int)level < (int)g_level) return;

    char line[512];
    int used = snprintf(line, sizeof line, "%-5s %-12s ", level_name(level), tag ? tag : "-");
    if (used < 0 || used >= (int)sizeof line) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + used, sizeof line - (size_t)used, fmt, ap);
    va_end(ap);

    if (g_sink) {
        g_sink(g_sink_user, level, line);
    } else {
        fprintf(stderr, "%s\n", line);
    }
}
