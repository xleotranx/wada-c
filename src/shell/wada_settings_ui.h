/* Settings panel: the shell's own screen, not a product.
 *
 * It is a surface above the current product (wada_app_set_surface), so it can
 * be opened from any product and it never enters or leaves a product scope.
 * Everything it writes goes through wada_settings; the boot code persists the
 * same keys on exit, so the panel and boot cannot disagree.
 */
#ifndef WADA_SETTINGS_UI_H
#define WADA_SETTINGS_UI_H

#include "wada.h"
#include "wada_app.h"
#include "wada_product.h"
#include "wada_settings.h"

typedef struct wada_settings_ui wada_settings_ui;

/* settings may be NULL (the panel then changes only the running state). */
WADA_API wada_settings_ui *wada_settings_ui_create(wada_app *app, wada_settings *settings,
                                                   wada_host *host, const char *data_dir);
WADA_API void wada_settings_ui_destroy(wada_settings_ui *ui);

WADA_API void wada_settings_ui_open(wada_settings_ui *ui);
WADA_API void wada_settings_ui_close(wada_settings_ui *ui);
WADA_API bool wada_settings_ui_is_open(wada_settings_ui *ui);

/* The actions the widgets perform. They queue the change for the next frame
 * start and persist it, exactly like a click; tests call them directly because
 * a hidden window cannot be clicked reliably. */
WADA_API void wada_settings_ui_set_theme(wada_settings_ui *ui, bool light);
WADA_API bool wada_settings_ui_set_language(wada_settings_ui *ui, const char *code);
WADA_API bool wada_settings_ui_set_product(wada_settings_ui *ui, const char *id);

#endif /* WADA_SETTINGS_UI_H */
