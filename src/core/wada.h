/* Wada core — base types, error model, versioning.
 *
 * The core owns the event loop, the services and the interfaces. Everything
 * that is not the core (storage, models, providers, plugins) implements a
 * versioned vtable published through wada_registry.h. No third-party header is
 * included here: the core compiles alone.
 */
#ifndef WADA_H
#define WADA_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WADA_VERSION_MAJOR 0
#define WADA_VERSION_MINOR 1
#define WADA_VERSION_PATCH 0
#define WADA_VERSION_STRING "0.1.0"

/* ABI of every published vtable. Bump only on an incompatible change. */
#define WADA_ABI 1u

#if defined(__GNUC__) || defined(__clang__)
#  define WADA_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define WADA_PRINTF(fmt_idx, arg_idx)
#endif

#if defined(_WIN32) && defined(WADA_BUILD_DLL)
#  define WADA_API __declspec(dllexport)
#else
#  define WADA_API
#endif

/* Platform path separator for the few places that build a sibling path. */
#if defined(_WIN32)
#  define WADA_PATH_SEP "\\"
#else
#  define WADA_PATH_SEP "/"
#endif

typedef enum wada_status {
    WADA_OK = 0,
    WADA_ERR = -1,
    WADA_ERR_INVALID = -2,
    WADA_ERR_NOMEM = -3,
    WADA_ERR_NOT_FOUND = -4,
    WADA_ERR_EXISTS = -5,
    WADA_ERR_STATE = -6,
    WADA_ERR_UNSUPPORTED = -7,
    WADA_ERR_IO = -8,
    WADA_ERR_ABI = -9
} wada_status;

#define WADA_ERR_MSG_CAP 192

/* Errors are values: every fallible call takes an out parameter and returns a
 * status. A caller that only needs success/failure may pass NULL. */
typedef struct wada_err {
    int code;
    char msg[WADA_ERR_MSG_CAP];
} wada_err;

WADA_API void wada_err_clear(wada_err *e);
WADA_API wada_status wada_err_set(wada_err *e, wada_status code, const char *fmt, ...) WADA_PRINTF(3, 4);
WADA_API const char *wada_err_msg(const wada_err *e);
WADA_API const char *wada_status_name(wada_status status);

#include "wada_scope.h"
#include "wada_event.h"
#include "wada_registry.h"
#include "wada_id.h"

#endif /* WADA_H */
