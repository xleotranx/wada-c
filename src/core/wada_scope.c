#include "wada.h"

#include <stdlib.h>
#include <string.h>

typedef struct wada_slot {
    uint64_t id;
    wada_dispose_fn fn;
    void *user;
    bool alive;
} wada_slot;

struct wada_scope {
    wada_scope *parent;
    wada_scope **children;
    int child_count;
    int child_cap;
    wada_slot *slots;
    int count;
    int cap;
    uint64_t next_id;
    char name[40];
    bool disposing;
};

static bool grow_slots(wada_scope *s) {
    int cap = s->cap ? s->cap * 2 : 8;
    wada_slot *v = (wada_slot *)realloc(s->slots, (size_t)cap * sizeof *v);
    if (!v) return false;
    s->slots = v;
    s->cap = cap;
    return true;
}

wada_scope *wada_scope_create(wada_scope *parent, const char *name) {
    wada_scope *s = (wada_scope *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->parent = parent;
    s->next_id = 1;
    if (name) {
        strncpy(s->name, name, sizeof s->name - 1);
    }
    if (parent) {
        if (parent->child_count == parent->child_cap) {
            int cap = parent->child_cap ? parent->child_cap * 2 : 4;
            wada_scope **v = (wada_scope **)realloc(parent->children, (size_t)cap * sizeof *v);
            if (!v) {
                free(s);
                return NULL;
            }
            parent->children = v;
            parent->child_cap = cap;
        }
        parent->children[parent->child_count++] = s;
    }
    return s;
}

wada_token wada_scope_own(wada_scope *scope, wada_dispose_fn fn, void *user) {
    if (!scope || !fn || scope->disposing) return 0;

    int idx = -1;
    for (int i = 0; i < scope->count; i++) {
        if (!scope->slots[i].alive) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (scope->count == scope->cap && !grow_slots(scope)) return 0;
        idx = scope->count++;
    }

    wada_slot *slot = &scope->slots[idx];
    slot->id = scope->next_id++;
    if (slot->id == 0) slot->id = scope->next_id++;
    slot->fn = fn;
    slot->user = user;
    slot->alive = true;
    return slot->id;
}

bool wada_scope_release(wada_scope *scope, wada_token token) {
    if (!scope || !token) return false;
    for (int i = 0; i < scope->count; i++) {
        wada_slot *slot = &scope->slots[i];
        if (!slot->alive || slot->id != token) continue;
        wada_dispose_fn fn = slot->fn;
        void *user = slot->user;
        slot->alive = false;
        slot->fn = NULL;
        slot->user = NULL;
        if (fn) fn(user);
        return true;
    }
    return false;
}

void wada_scope_destroy(wada_scope *scope) {
    if (!scope || scope->disposing) return;
    scope->disposing = true;

    for (int i = scope->child_count - 1; i >= 0; i--) {
        if (scope->children[i]) wada_scope_destroy(scope->children[i]);
    }
    for (int i = scope->count - 1; i >= 0; i--) {
        wada_slot *slot = &scope->slots[i];
        if (!slot->alive) continue;
        wada_dispose_fn fn = slot->fn;
        void *user = slot->user;
        slot->alive = false;
        slot->fn = NULL;
        slot->user = NULL;
        if (fn) fn(user);
    }

    if (scope->parent) {
        for (int i = 0; i < scope->parent->child_count; i++) {
            if (scope->parent->children[i] == scope) {
                scope->parent->children[i] = NULL;
                break;
            }
        }
    }
    free(scope->slots);
    free(scope->children);
    free(scope);
}

const char *wada_scope_name(const wada_scope *scope) {
    return scope ? scope->name : "";
}

int wada_scope_count(const wada_scope *scope) {
    if (!scope) return 0;
    int n = 0;
    for (int i = 0; i < scope->count; i++) {
        if (scope->slots[i].alive) n++;
    }
    return n;
}
