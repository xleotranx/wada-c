/* A deliberately incompatible provider module: it answers the handshake but
 * claims a plugin ABI the host does not speak. The loader must refuse it
 * without ever touching the vtable. */
#include <string.h>

#include "wada_iface.h"

WADA_PLUGIN_EXPORT wada_status wada_plugin_entry(uint32_t size, uint32_t abi, wada_plugin *out,
                                                wada_err *err) {
    (void)abi;
    (void)err;
    if (!out || size < sizeof(wada_plugin)) return WADA_ERR_INVALID;
    memset(out, 0, size);
    out->hdr.size = (uint32_t)sizeof(wada_plugin);
    out->hdr.version = 99; /* not WADA_PLUGIN_ABI */
    out->id = "bad";
    out->store_abi = WADA_STORE_ABI;
    out->store = NULL; /* the host must reject on the version before this */
    return WADA_OK;
}
