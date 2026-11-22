#include "wada.h"

#include <stdio.h>
#include <string.h>

void wada_err_clear(wada_err *e) {
    if (!e) return;
    e->code = WADA_OK;
    e->msg[0] = '\0';
}

wada_status wada_err_set(wada_err *e, wada_status code, const char *fmt, ...) {
    if (e) {
        e->code = (int)code;
        if (fmt && fmt[0]) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(e->msg, sizeof e->msg, fmt, ap);
            va_end(ap);
        } else {
            e->msg[0] = '\0';
        }
    }
    return code;
}

const char *wada_err_msg(const wada_err *e) {
    if (!e || e->msg[0] == '\0') return "";
    return e->msg;
}

const char *wada_status_name(wada_status status) {
    switch (status) {
    case WADA_OK: return "ok";
    case WADA_ERR: return "error";
    case WADA_ERR_INVALID: return "invalid";
    case WADA_ERR_NOMEM: return "out of memory";
    case WADA_ERR_NOT_FOUND: return "not found";
    case WADA_ERR_EXISTS: return "already exists";
    case WADA_ERR_STATE: return "bad state";
    case WADA_ERR_UNSUPPORTED: return "unsupported";
    case WADA_ERR_IO: return "io error";
    case WADA_ERR_ABI: return "abi mismatch";
    }
    return "unknown";
}
