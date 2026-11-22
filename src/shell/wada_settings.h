/* Settings: small string key/value store on the store interface.
 *
 * Keys are namespaced under "settings/" so a product's data and the app's
 * preferences can share one database file. Settings are read at boot (theme,
 * window size, last product) and written on change.
 */
#ifndef WADA_SETTINGS_H
#define WADA_SETTINGS_H

#include "wada.h"
#include "wada_iface.h"

typedef struct wada_settings wada_settings;

WADA_API wada_settings *wada_settings_open(const wada_store_ops *ops, const char *url, wada_err *err);
WADA_API void wada_settings_close(wada_settings *settings);

WADA_API wada_status wada_settings_get(wada_settings *settings, const char *key, char *out, size_t cap, wada_err *err);
WADA_API wada_status wada_settings_set(wada_settings *settings, const char *key, const char *value, wada_err *err);

WADA_API int wada_settings_int(wada_settings *settings, const char *key, int fallback);
WADA_API wada_status wada_settings_set_int(wada_settings *settings, const char *key, int value, wada_err *err);

#endif /* WADA_SETTINGS_H */
