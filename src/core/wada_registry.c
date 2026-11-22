#include "wada.h"

#include <stdlib.h>
#include <string.h>

typedef struct wada_service {
    wada_registry *registry;
    char *id;
    uint32_t version;
    const void *ops;
    bool alive;
} wada_service;

struct wada_registry {
    wada_service **v;
    int count;
    int cap;
};

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Removing a service only marks it dead: its allocation stays owned by the
 * registry, so a scope token that later disposes it can never touch freed
 * memory. Everything is freed by wada_registry_destroy. */
static void service_disable(void *user) {
    wada_service *svc = (wada_service *)user;
    if (svc) svc->alive = false;
}

static bool grow(wada_registry *registry) {
    int cap = registry->cap ? registry->cap * 2 : 8;
    wada_service **v = (wada_service **)realloc(registry->v, (size_t)cap * sizeof *v);
    if (!v) return false;
    registry->v = v;
    registry->cap = cap;
    return true;
}

wada_registry *wada_registry_create(void) {
    return (wada_registry *)calloc(1, sizeof(wada_registry));
}

void wada_registry_destroy(wada_registry *registry) {
    if (!registry) return;
    for (int i = 0; i < registry->count; i++) {
        free(registry->v[i]->id);
        free(registry->v[i]);
    }
    free(registry->v);
    free(registry);
}

wada_status wada_registry_add(wada_registry *registry, wada_scope *owner, const char *id,
                              uint32_t version, const void *ops) {
    if (!registry || !id || !id[0] || !ops) return WADA_ERR_INVALID;
    for (int i = 0; i < registry->count; i++) {
        if (registry->v[i]->alive && strcmp(registry->v[i]->id, id) == 0) return WADA_ERR_EXISTS;
    }
    if (registry->count == registry->cap && !grow(registry)) return WADA_ERR_NOMEM;

    wada_service *svc = (wada_service *)calloc(1, sizeof *svc);
    if (!svc) return WADA_ERR_NOMEM;
    svc->id = dup_str(id);
    if (!svc->id) {
        free(svc);
        return WADA_ERR_NOMEM;
    }
    svc->registry = registry;
    svc->version = version;
    svc->ops = ops;
    svc->alive = true;

    if (owner && wada_scope_own(owner, service_disable, svc) == 0) {
        free(svc->id);
        free(svc);
        return WADA_ERR_STATE;
    }
    registry->v[registry->count++] = svc;
    return WADA_OK;
}

const void *wada_registry_get(wada_registry *registry, const char *id, uint32_t min_version,
                              uint32_t *version_out) {
    if (!registry || !id) return NULL;
    const void *best_ops = NULL;
    uint32_t best_version = 0;
    for (int i = 0; i < registry->count; i++) {
        wada_service *svc = registry->v[i];
        if (!svc->alive || svc->version < min_version || strcmp(svc->id, id) != 0) continue;
        if (svc->version >= best_version) {
            best_version = svc->version;
            best_ops = svc->ops;
        }
    }
    if (best_ops && version_out) *version_out = best_version;
    return best_ops;
}

bool wada_registry_remove(wada_registry *registry, const char *id) {
    if (!registry || !id) return false;
    for (int i = 0; i < registry->count; i++) {
        wada_service *svc = registry->v[i];
        if (svc->alive && strcmp(svc->id, id) == 0) {
            svc->alive = false;
            return true;
        }
    }
    return false;
}

int wada_registry_count(const wada_registry *registry) {
    if (!registry) return 0;
    int n = 0;
    for (int i = 0; i < registry->count; i++) {
        if (registry->v[i]->alive) n++;
    }
    return n;
}

bool wada_registry_at(const wada_registry *registry, int index, const char **id,
                      uint32_t *version, const void **ops) {
    if (!registry || index < 0) return false;
    int n = 0;
    for (int i = 0; i < registry->count; i++) {
        wada_service *svc = registry->v[i];
        if (!svc->alive) continue;
        if (n++ != index) continue;
        if (id) *id = svc->id;
        if (version) *version = svc->version;
        if (ops) *ops = svc->ops;
        return true;
    }
    return false;
}
