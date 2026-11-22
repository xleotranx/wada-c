/* Event bus.
 *
 * Topics are strings (a trailing '*' on a subscription acts as a prefix
 * wildcard, e.g. "topic.*" or "wb/..."); payloads are typed structs owned by the
 * publisher and valid only for the duration of the call. Delivery is ordered by
 * priority (higher first), then registration order. Publishing iterates a
 * snapshot, so a listener may subscribe, cancel or publish during dispatch:
 *   - a listener added during dispatch is not called by that dispatch;
 *   - a listener cancelled during dispatch is not called again;
 *   - cancelled listeners are freed only when the outermost dispatch ends.
 */
#ifndef WADA_EVENT_H
#define WADA_EVENT_H

#include "wada.h"

typedef struct wada_bus wada_bus;
typedef struct wada_sub wada_sub;

typedef void (*wada_event_fn)(void *user, const char *topic, const void *data, size_t len);

WADA_API wada_bus *wada_bus_create(void);
WADA_API void wada_bus_destroy(wada_bus *bus);

WADA_API wada_sub *wada_bus_subscribe(wada_bus *bus, const char *topic, int priority, wada_event_fn fn, void *user);
/* Subscribes and attaches disposal to owner (when non-NULL). Returns NULL if
 * owner is already disposing or allocation fails. */
WADA_API wada_sub *wada_bus_subscribe_scoped(wada_bus *bus, wada_scope *owner, const char *topic,
                                             int priority, wada_event_fn fn, void *user);
WADA_API void wada_bus_cancel(wada_sub *sub);
WADA_API void wada_bus_publish(wada_bus *bus, const char *topic, const void *data, size_t len);

/* Live subscriptions, for diagnostics. */
WADA_API int wada_bus_count(const wada_bus *bus);

/* Adapter with the wada_dispose_fn signature, for scope ownership. */
WADA_API void wada_bus_cancel_owned(void *sub);

#endif /* WADA_EVENT_H */
