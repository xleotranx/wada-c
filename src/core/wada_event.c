#include "wada.h"

#include <stdlib.h>
#include <string.h>

struct wada_sub {
    wada_bus *bus;
    char *topic;
    int priority;
    uint64_t seq;
    wada_event_fn fn;
    void *user;
    bool canceled;
};

typedef struct snapbuf {
    wada_sub **v;
    int n;
    int cap;
} snapbuf;

struct wada_bus {
    wada_sub **subs;
    int count;
    int cap;
    uint64_t seq;
    int depth;
    snapbuf **snaps;
    int snap_cap;
};

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static bool grow_subs(wada_bus *bus) {
    int cap = bus->cap ? bus->cap * 2 : 8;
    wada_sub **v = (wada_sub **)realloc(bus->subs, (size_t)cap * sizeof *v);
    if (!v) return false;
    bus->subs = v;
    bus->cap = cap;
    return true;
}

static snapbuf *snap_at(wada_bus *bus, int depth) {
    if (depth >= bus->snap_cap) {
        int cap = bus->snap_cap ? bus->snap_cap : 4;
        while (cap <= depth) cap *= 2;
        snapbuf **v = (snapbuf **)realloc(bus->snaps, (size_t)cap * sizeof *v);
        if (!v) return NULL;
        for (int i = bus->snap_cap; i < cap; i++) v[i] = NULL;
        bus->snaps = v;
        bus->snap_cap = cap;
    }
    if (!bus->snaps[depth]) {
        bus->snaps[depth] = (snapbuf *)calloc(1, sizeof(snapbuf));
    }
    return bus->snaps[depth];
}

static bool snap_push(snapbuf *snap, wada_sub *sub) {
    if (snap->n == snap->cap) {
        int cap = snap->cap ? snap->cap * 2 : 8;
        wada_sub **v = (wada_sub **)realloc(snap->v, (size_t)cap * sizeof *v);
        if (!v) return false;
        snap->v = v;
        snap->cap = cap;
    }
    snap->v[snap->n++] = sub;
    return true;
}

static void free_sub(wada_sub *sub) {
    if (!sub) return;
    free(sub->topic);
    free(sub);
}

static void sweep(wada_bus *bus) {
    int write = 0;
    for (int i = 0; i < bus->count; i++) {
        wada_sub *sub = bus->subs[i];
        if (sub->canceled) {
            free_sub(sub);
        } else {
            bus->subs[write++] = sub;
        }
    }
    bus->count = write;
}

wada_bus *wada_bus_create(void) {
    return (wada_bus *)calloc(1, sizeof(wada_bus));
}

void wada_bus_destroy(wada_bus *bus) {
    if (!bus) return;
    for (int i = 0; i < bus->count; i++) free_sub(bus->subs[i]);
    for (int i = 0; i < bus->snap_cap; i++) {
        if (bus->snaps[i]) {
            free(bus->snaps[i]->v);
            free(bus->snaps[i]);
        }
    }
    free(bus->snaps);
    free(bus->subs);
    free(bus);
}

wada_sub *wada_bus_subscribe(wada_bus *bus, const char *topic, int priority, wada_event_fn fn, void *user) {
    if (!bus || !topic || !fn) return NULL;
    if (bus->count == bus->cap && !grow_subs(bus)) return NULL;

    wada_sub *sub = (wada_sub *)calloc(1, sizeof *sub);
    if (!sub) return NULL;
    sub->topic = dup_str(topic);
    if (!sub->topic) {
        free(sub);
        return NULL;
    }
    sub->bus = bus;
    sub->priority = priority;
    sub->seq = ++bus->seq;
    sub->fn = fn;
    sub->user = user;
    bus->subs[bus->count++] = sub;
    return sub;
}

void wada_bus_cancel(wada_sub *sub) {
    if (!sub || sub->canceled) return;
    sub->canceled = true;
    if (sub->bus->depth == 0) sweep(sub->bus);
}

void wada_bus_cancel_owned(void *sub) {
    wada_bus_cancel((wada_sub *)sub);
}

void wada_bus_publish(wada_bus *bus, const char *topic, const void *data, size_t len) {
    if (!bus || !topic) return;

    snapbuf *snap = snap_at(bus, bus->depth);
    if (!snap) return;
    snap->n = 0;
    for (int i = 0; i < bus->count; i++) {
        wada_sub *sub = bus->subs[i];
        if (!sub->canceled && strcmp(sub->topic, topic) == 0) {
            if (!snap_push(snap, sub)) return;
        }
    }

    /* Stable insertion sort: higher priority first, then registration order. */
    for (int i = 1; i < snap->n; i++) {
        wada_sub *key = snap->v[i];
        int j = i - 1;
        while (j >= 0 && (snap->v[j]->priority < key->priority ||
                          (snap->v[j]->priority == key->priority && snap->v[j]->seq > key->seq))) {
            snap->v[j + 1] = snap->v[j];
            j--;
        }
        snap->v[j + 1] = key;
    }

    bus->depth++;
    for (int i = 0; i < snap->n; i++) {
        wada_sub *sub = snap->v[i];
        if (!sub->canceled) sub->fn(sub->user, topic, data, len);
    }
    bus->depth--;

    if (bus->depth == 0) sweep(bus);
}

int wada_bus_count(const wada_bus *bus) {
    if (!bus) return 0;
    int n = 0;
    for (int i = 0; i < bus->count; i++) {
        if (!bus->subs[i]->canceled) n++;
    }
    return n;
}
