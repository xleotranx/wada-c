/* Provider interfaces.
 *
 * Every provider vtable starts with a {size, version} header so a caller can
 * validate it before reading any slot:
 *
 *   - size    = sizeof(the vtable the provider compiled against); a caller
 *               checks it is at least the size it needs and that unknown
 *               trailing bytes are absent (append-only evolution);
 *   - version = the ABI constant the provider implements.
 *
 * Slots are append-only. A change to the meaning of an existing slot, or a
 * reordered table, is a new ABI and a new version number.
 *
 * The store interface below is the contract a storage vendor implements
 * (SQLite first, files or a remote database later). It deliberately contains
 * no SQL, no file names and no provider-specific types: keys and values are
 * borrowed byte slices, valid until the next call on the same handle.
 */
#ifndef WADA_IFACE_H
#define WADA_IFACE_H

#include "wada.h"

typedef struct wada_ops_header {
    uint32_t size;    /* sizeof(the concrete ops struct) */
    uint32_t version; /* ABI the provider implements */
} wada_ops_header;

static inline bool wada_ops_ok(const wada_ops_header *hdr, size_t need, uint32_t abi) {
    return hdr && hdr->size >= need && hdr->version == abi;
}

/* ---- Store ------------------------------------------------------------- */

#define WADA_STORE_ABI 3u
#define WADA_STORE_OPS_SIZE (sizeof(wada_store_ops))

typedef struct wada_store wada_store;
typedef struct wada_txn wada_txn;
typedef struct wada_iter wada_iter;

typedef struct wada_slice {
    const uint8_t *p;
    size_t n;
} wada_slice;

typedef enum wada_store_mode {
    WADA_STORE_READ = 0,
    WADA_STORE_WRITE = 1
} wada_store_mode;

enum {
    WADA_STORE_CAP_TRANSACTIONS = 1u << 0,
    WADA_STORE_CAP_SEARCH = 1u << 1,
    WADA_STORE_CAP_MIGRATIONS = 1u << 2
};

/* ---- Provider modules ---------------------------------------------------
 *
 * A provider may live in a shared library instead of the executable. The
 * module exports exactly one symbol, WADA_PLUGIN_SYMBOL, with this signature:
 *
 *   wada_status wada_plugin_entry(uint32_t size, uint32_t abi,
 *                                 wada_plugin *out, wada_err *err);
 *
 * `size` is the size of the caller's wada_plugin, so an older module writes
 * only the fields it knows; `abi` is the plugin ABI the caller speaks. The
 * module fills `out`, then the host validates the header before reading any
 * pointer — a mismatch is WADA_ERR_ABI, not a crash. The module stays loaded
 * for the process lifetime (see wada_module.h); nothing is ever unmapped while
 * a function pointer into it is alive.
 */
#define WADA_PLUGIN_ABI 1u
#define WADA_PLUGIN_SYMBOL "wada_plugin_entry"

#if defined(_WIN32)
#  define WADA_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#  define WADA_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#  define WADA_PLUGIN_EXPORT
#endif

struct wada_store_ops;

typedef struct wada_plugin {
    wada_ops_header hdr;      /* size, WADA_PLUGIN_ABI */
    const char *id;           /* "sqlite" — shown by --doctor */
    uint32_t store_abi;       /* WADA_STORE_ABI the store vtable implements */
    const struct wada_store_ops *store;
} wada_plugin;

typedef wada_status (*wada_plugin_entry_fn)(uint32_t size, uint32_t abi, wada_plugin *out, wada_err *err);

typedef struct wada_store_ops {
    wada_ops_header hdr; /* size, WADA_STORE_ABI */

    wada_status (*open)(const char *url, wada_store **out, wada_err *err);
    void (*close)(wada_store *store);

    wada_status (*begin)(wada_store *store, wada_store_mode mode, wada_txn **out, wada_err *err);
    wada_status (*commit)(wada_txn *txn, wada_err *err);
    void (*rollback)(wada_txn *txn);

    /* Borrowed slices, valid until the next call on txn. */
    wada_status (*get)(wada_txn *txn, wada_slice key, wada_slice *value, wada_err *err);
    wada_status (*put)(wada_txn *txn, wada_slice key, wada_slice value, wada_err *err);
    wada_status (*del)(wada_txn *txn, wada_slice key, wada_err *err);

    wada_status (*list)(wada_txn *txn, wada_slice prefix, wada_iter **out, wada_err *err);
    wada_status (*next)(wada_iter *it, wada_slice *key, wada_slice *value, wada_err *err);
    void (*iter_close)(wada_iter *it);

    wada_status (*migrate)(wada_store *store, uint32_t target, wada_err *err);
    uint32_t (*caps)(const wada_store *store);

    /* ---- ABI 2 (append-only) ------------------------------------------ */

    /* The schema version the provider currently has open. */
    uint32_t (*schema_version)(const wada_store *store);

    /* Full-text search over the values stored under prefix, best match first.
     * The query is plain text (terms are ANDed, the last one is a prefix).
     * Results iterate with next(); value is the indexed text, not necessarily
     * the stored record, so a caller that needs the record calls get(). An
     * empty query lists the prefix. Providers without search index support
     * return WADA_ERR_UNSUPPORTED; callers then fall back to their own scan.
     * limit 0 asks for the provider default. */
    wada_status (*search)(wada_txn *txn, wada_slice prefix, const char *query, uint32_t limit,
                          wada_iter **out, wada_err *err);

    /* ---- ABI 3 (append-only) ------------------------------------------ */

    /* The most recent backup the provider wrote or found for this store, or ""
     * when it has none (an in-memory store, a provider without backups). The
     * string is owned by the store and valid until close. Providers that back
     * up before a migration and once a day report it here so the app can show
     * where the safety copy is. */
    const char *(*last_backup)(const wada_store *store);
} wada_store_ops;

#endif /* WADA_IFACE_H */
