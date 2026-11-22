#include "wada_product.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wada_settings.h"

typedef struct host_command {
    wada_host *host;
    int index;
} host_command;

struct wada_host {
    wada_app *app;
    wada_settings *settings;
    const char *data_dir;
    const wada_product *const *products;
    int count;
    int current;
    wada_scope *product_scope;
    void *state;
    wada_scope *command_scope;
    wada_scope *i18n_scope;
    host_command *commands;
    char pending[32];
    bool has_pending;
};

static int find_product(const wada_host *h, const char *id) {
    if (!h || !id) return -1;
    for (int i = 0; i < h->count; i++) {
        if (strcmp(h->products[i]->id, id) == 0) return i;
    }
    return -1;
}

static void run_host_command(void *user) {
    host_command *c = (host_command *)user;
    wada_host_switch(c->host, c->host->products[c->index]->id);
}

static bool enter_index(wada_host *h, int index) {
    if (!h || index < 0 || index >= h->count) return false;
    const wada_product *next = h->products[index];

    if (h->state) {
        const wada_product *cur = h->current >= 0 ? h->products[h->current] : NULL;
        if (cur && cur->leave) cur->leave(h->state);
        h->state = NULL;
    }
    if (h->product_scope) {
        wada_scope_destroy(h->product_scope);
        h->product_scope = NULL;
    }

    wada_scope *scope = wada_scope_create(wada_app_scope(h->app), next->id);
    if (!scope) {
        wada_app_set_status(h->app, "out of memory");
        return false;
    }

    wada_err err;
    wada_err_clear(&err);
    void *state = next->enter ? next->enter(h->app, scope, h->data_dir, &err) : NULL;
    if (!state) {
        wada_app_set_status(h->app, wada_err_msg(&err)[0] ? wada_err_msg(&err) : "product failed to start");
        wada_scope_destroy(scope);
        return false;
    }

    h->product_scope = scope;
    h->state = state;
    h->current = index;
    wada_app_set_title(h->app, wada_tr(next->title));
    if (h->settings) wada_settings_set(h->settings, "app/product", next->id, NULL);
    wada_app_set_status(h->app, wada_tr(next->title));
    return true;
}

wada_host *wada_host_create(wada_app *app, wada_settings *settings, const char *data_dir,
                            const wada_product *const *products, int count, wada_err *err) {
    if (!app || !products || count <= 0) {
        wada_err_set(err, WADA_ERR_INVALID, "host: no products");
        return NULL;
    }
    wada_host *h = (wada_host *)calloc(1, sizeof *h);
    if (!h) {
        wada_err_set(err, WADA_ERR_NOMEM, "host");
        return NULL;
    }
    h->app = app;
    h->settings = settings;
    h->data_dir = data_dir;
    h->products = products;
    h->count = count;
    h->current = -1;
    h->command_scope = wada_scope_create(wada_app_scope(app), "host");
    h->i18n_scope = wada_scope_create(wada_app_scope(app), "i18n");

    h->commands = (host_command *)calloc((size_t)count, sizeof *h->commands);
    if (!h->command_scope || !h->i18n_scope || !h->commands) {
        wada_host_destroy(h);
        wada_err_set(err, WADA_ERR_NOMEM, "host");
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        if (products[i]->strings && products[i]->string_count > 0) {
            if (wada_i18n_register(h->i18n_scope, products[i]->strings, products[i]->string_count) !=
                WADA_OK) {
                WADA_LOGW("host", "i18n table for %s not registered", products[i]->id);
            }
        }
        char id[64];
        snprintf(id, sizeof id, "app.product.%s", products[i]->id);
        h->commands[i].host = h;
        h->commands[i].index = i;
        /* The title stays English; the palette translates it at draw time, so a
         * language change needs no re-registration. */
        wada_app_command(app, h->command_scope, id, products[i]->title, NULL, run_host_command,
                         &h->commands[i]);
    }
    return h;
}

void wada_host_destroy(wada_host *h) {
    if (!h) return;
    if (h->state && h->current >= 0 && h->products[h->current]->leave) {
        h->products[h->current]->leave(h->state);
    }
    if (h->product_scope) wada_scope_destroy(h->product_scope);
    if (h->command_scope) wada_scope_destroy(h->command_scope);
    if (h->i18n_scope) wada_scope_destroy(h->i18n_scope);
    free(h->commands);
    free(h);
}

bool wada_host_switch(wada_host *h, const char *id) {
    if (!h || !id) return false;
    int index = find_product(h, id);
    if (index < 0) return false;
    if (index == h->current && h->state) return false;
    snprintf(h->pending, sizeof h->pending, "%s", id);
    h->has_pending = true;
    return true;
}

void wada_host_frame(wada_host *h) {
    if (!h || !h->app) return;
    if (h->has_pending) {
        h->has_pending = false;
        int index = find_product(h, h->pending);
        if (index >= 0) enter_index(h, index);
    }
    if (h->state && h->current >= 0) {
        const wada_product *p = h->products[h->current];
        if (p->frame) p->frame(h->state, h->app);
    }
}

const char *wada_host_current(wada_host *h) {
    if (!h || h->current < 0) return "";
    return h->products[h->current]->id;
}

int wada_host_count(wada_host *h) {
    return h ? h->count : 0;
}

const char *wada_host_id_at(wada_host *h, int index) {
    if (!h || index < 0 || index >= h->count) return "";
    return h->products[index]->id;
}

const char *wada_host_title_at(wada_host *h, int index) {
    if (!h || index < 0 || index >= h->count) return "";
    return h->products[index]->title;
}

const char *wada_host_title(wada_host *h) {
    if (!h || h->current < 0) return "";
    return wada_tr(h->products[h->current]->title);
}
