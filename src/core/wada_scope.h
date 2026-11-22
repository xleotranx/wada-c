/* Scopes own registrations.
 *
 * Anything a plugin, provider or screen registers — a bus subscription, a
 * service, a command, a widget — is owned by a scope. Destroying the scope
 * disposes its children and its own entries in reverse registration order, so
 * unload is mechanical instead of a list of hand-written teardown calls.
 */
#ifndef WADA_SCOPE_H
#define WADA_SCOPE_H

#include "wada.h"

typedef struct wada_scope wada_scope;
typedef uint64_t wada_token;
typedef void (*wada_dispose_fn)(void *user);

/* A NULL parent creates a root scope. */
WADA_API wada_scope *wada_scope_create(wada_scope *parent, const char *name);
WADA_API void wada_scope_destroy(wada_scope *scope);
WADA_API const char *wada_scope_name(const wada_scope *scope);

/* Registers fn(user) to run when the token is released or the scope dies.
 * Returns 0 when the scope is null or already disposing. */
WADA_API wada_token wada_scope_own(wada_scope *scope, wada_dispose_fn fn, void *user);

/* Disposes one entry now. The same token is never disposed twice. */
WADA_API bool wada_scope_release(wada_scope *scope, wada_token token);

/* Live entries (excluding children). */
WADA_API int wada_scope_count(const wada_scope *scope);

#endif /* WADA_SCOPE_H */
