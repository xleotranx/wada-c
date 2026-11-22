/* Boot: the glue every app and example shares.
 *
 * Resolves the data directory, opens settings, publishes the store provider as
 * the "store" service, creates the app and the product host, runs the frame
 * loop, persists window state and the current product, and tears down. An app
 * is then just a product table and a call to wada_boot_run.
 */
#ifndef WADA_BOOT_H
#define WADA_BOOT_H

#include "wada_iface.h"
#include "wada_product.h"

typedef struct wada_boot_desc {
    const char *title;   /* window title before a product sets its own */
    int width, height;   /* fallback when no saved size exists */
    bool light_theme;    /* fallback when no saved theme exists */
    const wada_store_ops *store; /* static provider; may be NULL for a module-only app */
    const wada_product *const *products;
    int product_count;
    const char *default_product; /* may be NULL: first product */
    /* Subdirectory under the app data root; NULL means "Wada". Examples pass
     * their own so they cannot read or write the shipped app's database. */
    const char *data_subdir;
    /* Optional app-lifetime setup hook: runs once after the "store" service is
     * registered and before the product host is created. Use it to register
     * shared services or cross-product bus observers owned by app_scope. */
    wada_status (*setup)(wada_app *app, wada_scope *app_scope, const char *data_dir, wada_err *err);
} wada_boot_desc;

/* argc/argv are scanned for --app <id>, --provider <path>, --lang <code>,
 * --settings and --doctor. Returns the process exit code. */
WADA_API int wada_boot_run(const wada_boot_desc *desc, int argc, char **argv);

#endif /* WADA_BOOT_H */
