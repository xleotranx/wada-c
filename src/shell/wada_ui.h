/* Product UI kit: shared shell-level helpers for Wada products.
 *
 * Keeps the common product boilerplate in one place: locating wada.db,
 * fetching the store service, formatting timestamps, reporting wada_err to
 * the status bar, and two-click destructive action confirmation.
 */
#ifndef WADA_UI_H
#define WADA_UI_H

#include "wada.h"
#include "wada_app.h"
#include "wada_iface.h"

typedef struct wada_ui_confirm {
    int target; /* index awaiting second click, or -1 */
    int frames; /* countdown frames remaining */
} wada_ui_confirm;

WADA_API void wada_ui_db_path(const char *data_dir, char *out, size_t cap);
WADA_API const wada_store_ops *wada_ui_store_ops(wada_app *app, const char *owner, wada_err *err);
WADA_API void wada_ui_format_when(long long unix_sec, char *out, size_t cap);
WADA_API void wada_ui_status_err(wada_app *app, char *buf, size_t cap, const wada_err *err);

WADA_API void wada_ui_confirm_init(wada_ui_confirm *c);
WADA_API void wada_ui_confirm_reset(wada_ui_confirm *c);
WADA_API bool wada_ui_confirm_active(const wada_ui_confirm *c, int index);
/* Returns true if already armed for index (second click -> proceed) and resets;
 * otherwise arms for index with timeout_frames and returns false. */
WADA_API bool wada_ui_confirm_step(wada_ui_confirm *c, int index, int timeout_frames);
/* Cancels if Escape pressed, selection changed, or timeout expired. */
WADA_API void wada_ui_confirm_tick(wada_ui_confirm *c, cui_ctx *ui, int current_index);

#endif /* WADA_UI_H */
