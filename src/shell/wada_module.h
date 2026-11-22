/* Provider modules: load a store provider from a shared library.
 *
 * The loader is the only file in the app that calls LoadLibrary/dlopen. It
 * validates three things before the vtable is ever used: the module speaks
 * WADA_PLUGIN_ABI, the plugin says which store ABI its vtable implements, and
 * the vtable header itself matches. A loaded module is never unloaded while the
 * process runs — the alternative (unload on switch) cannot be made safe in C,
 * because a callback or iterator may still point into the module.
 */
#ifndef WADA_MODULE_H
#define WADA_MODULE_H

#include "wada.h"
#include "wada_iface.h"

typedef struct wada_module wada_module;

/* A relative path is resolved against the executable's directory first, then
 * against the current directory. Returns NULL and sets err on failure. */
WADA_API wada_module *wada_module_load(const char *path, wada_err *err);
WADA_API void wada_module_unload(wada_module *module);

WADA_API const wada_plugin *wada_module_plugin(const wada_module *module);
WADA_API const wada_store_ops *wada_module_store(const wada_module *module);
WADA_API const char *wada_module_id(const wada_module *module);
WADA_API const char *wada_module_path(const wada_module *module);

#endif /* WADA_MODULE_H */
