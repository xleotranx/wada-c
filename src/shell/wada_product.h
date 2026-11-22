/* Products.
 *
 * A product is a self-contained app screen: it enters with a scope of its own,
 * draws one compose callback, and leaves when the host switches away. The host
 * owns the product's scope, so every command and subscription a product
 * registered disappears with it — switching products is scope disposal, not a
 * hand-written teardown.
 */
#ifndef WADA_PRODUCT_H
#define WADA_PRODUCT_H

#include "wada.h"
#include "wada_app.h"
#include "wada_i18n.h"

typedef struct wada_settings wada_settings;

typedef struct wada_product {
    const char *id;    /* "notes" — used in command ids and settings */
    const char *title; /* "Notes" — the window title while active */
    void *(*enter)(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err);
    void (*leave)(void *state);
    void (*frame)(void *state, wada_app *app);
    /* Optional translation table, static data, registered by the host for the
     * life of the app: a product's strings must be readable even while the
     * product is not active (the settings panel and the palette show titles of
     * every product). */
    const wada_i18n_pair *strings;
    int string_count;
    /* Optional: commands --doctor invokes, one per frame, to reach states a
     * single frame does not show (a review session, a delete confirmation, an
     * add form). They run against a throwaway database, so list only commands
     * that are safe there — never import/export or anything that opens a
     * dialog. */
    const char *const *walk;
    int walk_count;
    /* Optional: store key prefixes owned by this product (e.g. {"note/", "note-body/"}),
     * reported by --doctor without hardcoding product names in boot. */
    const char *const *prefixes;
    int prefix_count;
} wada_product;

typedef struct wada_host wada_host;

/* Products are expected to stay valid for the life of the host (static data). */
WADA_API wada_host *wada_host_create(wada_app *app, wada_settings *settings, const char *data_dir,
                                     const wada_product *const *products, int count, wada_err *err);
WADA_API void wada_host_destroy(wada_host *host);

/* Switches at the start of the next frame, never inside a widget handler. */
WADA_API bool wada_host_switch(wada_host *host, const char *id);
WADA_API void wada_host_frame(wada_host *host);
WADA_API const char *wada_host_current(wada_host *host);

/* The product list, for the settings panel: ids are stable, titles are the
 * English source strings (pass them through wada_tr for display). */
WADA_API int wada_host_count(wada_host *host);
WADA_API const char *wada_host_id_at(wada_host *host, int index);
WADA_API const char *wada_host_title_at(wada_host *host, int index);
/* The current product's title, translated. */
WADA_API const char *wada_host_title(wada_host *host);

#endif /* WADA_PRODUCT_H */
