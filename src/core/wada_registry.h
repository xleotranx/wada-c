/* Service registry.
 *
 * An interface is a versioned vtable published under a name:
 *
 *     wada_registry_add(reg, scope, "store", 1, &sqlite_store_ops);
 *     const wada_store_ops *store = wada_registry_get(reg, "store", 1, NULL);
 *
 * A consumer asks for the minimum version it understands; the provider keeps
 * its own ABI in the header of the vtable. Providers are statically linked
 * (in-tree "vendors") or loaded from a module at the same seam — the registry
 * does not care which.
 */
#ifndef WADA_REGISTRY_H
#define WADA_REGISTRY_H

#include "wada.h"

typedef struct wada_registry wada_registry;

WADA_API wada_registry *wada_registry_create(void);
WADA_API void wada_registry_destroy(wada_registry *registry);

/* owner may be NULL. With an owner, destroying that scope removes the service. */
WADA_API wada_status wada_registry_add(wada_registry *registry, wada_scope *owner, const char *id,
                                       uint32_t version, const void *ops);

/* Returns the ops of the highest-version live provider with version >= min_version. */
WADA_API const void *wada_registry_get(wada_registry *registry, const char *id, uint32_t min_version,
                                       uint32_t *version_out);

WADA_API bool wada_registry_remove(wada_registry *registry, const char *id);

/* Live services, for diagnostics. */
WADA_API int wada_registry_count(const wada_registry *registry);
WADA_API bool wada_registry_at(const wada_registry *registry, int index, const char **id,
                               uint32_t *version, const void **ops);

#endif /* WADA_REGISTRY_H */
