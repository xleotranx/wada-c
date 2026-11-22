/* Records: prefix-scoped key/value CRUD and line-record helpers.
 *
 * Products share the same boilerplate over wada_store_ops: open with ABI check,
 * namespace keys under "<prefix><id>", draw unique ids checked against the
 * store, iterate a prefix stripping it back to the suffix id, and encode/parse
 * single-line fields. Keeping that in one core helper keeps products small and
 * guarantees the suffix-only id discipline everywhere.
 */
#ifndef WADA_RECORDS_H
#define WADA_RECORDS_H

#include "wada.h"
#include "wada_iface.h"

typedef struct wada_records {
    const wada_store_ops *ops;
    wada_store *store;
    char prefix[32];
    size_t prefix_len;
} wada_records;

/* Visitor for wada_records_each: receives the suffix id (prefix stripped) and
 * the borrowed value slice. Return false to stop iteration early. */
typedef bool (*wada_records_visit_fn)(const char *id, wada_slice value, void *user);

WADA_API wada_status wada_records_open(wada_records *r, const wada_store_ops *ops,
                                       const char *url, const char *prefix, wada_err *err);
WADA_API void wada_records_close(wada_records *r);

WADA_API void wada_records_key(const wada_records *r, const char *id, char *out, size_t cap);
WADA_API bool wada_records_exists(wada_records *r, const char *id);
WADA_API void wada_records_fresh_id(wada_records *r, char *out, size_t cap);

WADA_API wada_status wada_records_put(wada_records *r, const char *id,
                                      const void *data, size_t len, wada_err *err);
WADA_API wada_status wada_records_get(wada_records *r, const char *id,
                                      char *out, size_t cap, size_t *out_len, wada_err *err);
WADA_API wada_status wada_records_del(wada_records *r, const char *id, wada_err *err);
WADA_API wada_status wada_records_each(wada_records *r, wada_records_visit_fn fn,
                                       void *user, wada_err *err);
/* Full-text search under this collection's prefix, best match first. Strips
 * the prefix before invoking fn. Returns WADA_ERR_UNSUPPORTED if the store
 * provider has no search index so callers can fall back. */
WADA_API wada_status wada_records_search(wada_records *r, const char *query, uint32_t limit,
                                         wada_records_visit_fn fn, void *user, wada_err *err);

/* Replaces '\n' and '\r' with spaces so a field stays on one record line. */
WADA_API void wada_records_sanitize_line(const char *in, char *out, size_t cap);
/* Reads up to the next '\n' (or end), advances *p, and returns false at end. */
WADA_API bool wada_records_next_line(const char **p, const char *end, char *out, size_t cap);

#endif /* WADA_RECORDS_H */
