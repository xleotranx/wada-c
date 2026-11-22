/* Dynamic entry point of the SQLite provider.
 *
 * This file is compiled only into wada_store_plugin (the shared library). The
 * static build calls wada_store_sqlite() directly; a module build answers the
 * host's handshake and hands it the same vtable. One provider implementation,
 * two ways to deliver it.
 */
#include <string.h>

#include "wada_iface.h"
#include "wada_store_sqlite.h"

WADA_PLUGIN_EXPORT wada_status wada_plugin_entry(uint32_t size, uint32_t abi, wada_plugin *out,
                                                wada_err *err) {
    if (!out) return wada_err_set(err, WADA_ERR_INVALID, "plugin: no output");
    if (size < sizeof(wada_plugin)) {
        return wada_err_set(err, WADA_ERR_ABI, "plugin: host struct %u bytes, provider needs %u", size,
                            (unsigned)sizeof(wada_plugin));
    }
    if (abi != WADA_PLUGIN_ABI) {
        return wada_err_set(err, WADA_ERR_ABI, "plugin: host ABI %u, provider speaks %u", abi,
                            WADA_PLUGIN_ABI);
    }
    memset(out, 0, size);
    out->hdr.size = (uint32_t)sizeof(wada_plugin);
    out->hdr.version = WADA_PLUGIN_ABI;
    out->id = "sqlite";
    out->store_abi = WADA_STORE_ABI;
    out->store = wada_store_sqlite();
    return WADA_OK;
}
