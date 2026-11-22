#include "wada_records.h"

#include <stdio.h>
#include <string.h>

wada_status wada_records_open(wada_records *r, const wada_store_ops *ops,
                              const char *url, const char *prefix, wada_err *err) {
    if (!r || !ops || !url || !prefix || !prefix[0]) {
        return wada_err_set(err, WADA_ERR_INVALID, "records_open");
    }
    size_t plen = strlen(prefix);
    if (plen >= sizeof r->prefix) {
        return wada_err_set(err, WADA_ERR_INVALID, "records prefix too long");
    }
    memset(r, 0, sizeof *r);
    if (!wada_ops_ok(&ops->hdr, sizeof(wada_store_ops), WADA_STORE_ABI)) {
        return wada_err_set(err, WADA_ERR_ABI, "store provider ABI %u, expected %u",
                            ops->hdr.version, WADA_STORE_ABI);
    }

    r->ops = ops;
    memcpy(r->prefix, prefix, plen + 1);
    r->prefix_len = plen;

    wada_status st = ops->open(url, &r->store, err);
    if (st != WADA_OK) memset(r, 0, sizeof *r);
    return st;
}

void wada_records_close(wada_records *r) {
    if (!r) return;
    if (r->ops && r->store) r->ops->close(r->store);
    memset(r, 0, sizeof *r);
}

void wada_records_key(const wada_records *r, const char *id, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (!r || !id) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%s%s", r->prefix, id);
}

bool wada_records_exists(wada_records *r, const char *id) {
    if (!r || !r->ops || !r->store || !id || !id[0]) return false;
    char key[128];
    wada_records_key(r, id, key, sizeof key);
    wada_txn *txn = NULL;
    if (r->ops->begin(r->store, WADA_STORE_READ, &txn, NULL) != WADA_OK) return false;
    wada_slice k = {(const uint8_t *)key, strlen(key)};
    wada_slice value;
    bool taken = r->ops->get(txn, k, &value, NULL) == WADA_OK;
    r->ops->rollback(txn);
    return taken;
}

void wada_records_fresh_id(wada_records *r, char *out, size_t cap) {
    if (!out || cap == 0) return;
    for (int attempt = 0; attempt < 8; attempt++) {
        wada_id_make(out, cap);
        if (!r || !wada_records_exists(r, out)) return;
    }
}

wada_status wada_records_put(wada_records *r, const char *id,
                             const void *data, size_t len, wada_err *err) {
    if (!r || !r->ops || !r->store || !id || !id[0]) {
        return wada_err_set(err, WADA_ERR_INVALID, "records_put");
    }
    char keybuf[128];
    wada_records_key(r, id, keybuf, sizeof keybuf);

    wada_txn *txn = NULL;
    wada_status st = r->ops->begin(r->store, WADA_STORE_WRITE, &txn, err);
    if (st != WADA_OK) return st;

    wada_slice key = {(const uint8_t *)keybuf, strlen(keybuf)};
    wada_slice value = {(const uint8_t *)(data ? data : ""), data ? len : 0};
    st = r->ops->put(txn, key, value, err);
    if (st == WADA_OK) {
        st = r->ops->commit(txn, err);
    } else {
        r->ops->rollback(txn);
    }
    return st;
}

wada_status wada_records_get(wada_records *r, const char *id,
                             char *out, size_t cap, size_t *out_len, wada_err *err) {
    if (out && cap > 0) out[0] = '\0';
    if (out_len) *out_len = 0;
    if (!r || !r->ops || !r->store || !id || !id[0]) {
        return wada_err_set(err, WADA_ERR_INVALID, "records_get");
    }
    char keybuf[128];
    wada_records_key(r, id, keybuf, sizeof keybuf);

    wada_txn *txn = NULL;
    wada_status st = r->ops->begin(r->store, WADA_STORE_READ, &txn, err);
    if (st != WADA_OK) return st;

    wada_slice key = {(const uint8_t *)keybuf, strlen(keybuf)};
    wada_slice value;
    st = r->ops->get(txn, key, &value, err);
    if (st == WADA_OK) {
        if (out_len) *out_len = value.n;
        if (out && cap > 0) {
            size_t n = value.n < cap - 1 ? value.n : cap - 1;
            memcpy(out, value.p, n);
            out[n] = '\0';
        }
    }
    wada_status commit = r->ops->commit(txn, st == WADA_OK ? err : NULL);
    return st != WADA_OK ? st : commit;
}

wada_status wada_records_del(wada_records *r, const char *id, wada_err *err) {
    if (!r || !r->ops || !r->store || !id || !id[0]) {
        return wada_err_set(err, WADA_ERR_INVALID, "records_del");
    }
    char keybuf[128];
    wada_records_key(r, id, keybuf, sizeof keybuf);

    wada_txn *txn = NULL;
    wada_status st = r->ops->begin(r->store, WADA_STORE_WRITE, &txn, err);
    if (st != WADA_OK) return st;

    wada_slice key = {(const uint8_t *)keybuf, strlen(keybuf)};
    st = r->ops->del(txn, key, err);
    if (st == WADA_OK) {
        st = r->ops->commit(txn, err);
    } else {
        r->ops->rollback(txn);
    }
    return st;
}

static wada_status iter_stripped(wada_records *r, wada_txn *txn, wada_iter *it,
                                 wada_records_visit_fn fn, void *user, wada_err *err) {
    wada_slice key, value;
    while (r->ops->next(it, &key, &value, NULL) == WADA_OK) {
        /* Keys are "<prefix><id>"; callers only ever see the suffix id. */
        if (key.n <= r->prefix_len) continue;
        char id[96];
        size_t id_len = key.n - r->prefix_len;
        if (id_len >= sizeof id) id_len = sizeof id - 1;
        memcpy(id, key.p + r->prefix_len, id_len);
        id[id_len] = '\0';
        if (!fn(id, value, user)) break;
    }
    r->ops->iter_close(it);
    return r->ops->commit(txn, err);
}

wada_status wada_records_each(wada_records *r, wada_records_visit_fn fn,
                              void *user, wada_err *err) {
    if (!r || !r->ops || !r->store || !fn) {
        return wada_err_set(err, WADA_ERR_INVALID, "records_each");
    }

    wada_txn *txn = NULL;
    wada_status st = r->ops->begin(r->store, WADA_STORE_READ, &txn, err);
    if (st != WADA_OK) return st;

    wada_iter *it = NULL;
    wada_slice prefix = {(const uint8_t *)r->prefix, r->prefix_len};
    st = r->ops->list(txn, prefix, &it, err);
    if (st != WADA_OK) {
        r->ops->rollback(txn);
        return st;
    }
    return iter_stripped(r, txn, it, fn, user, err);
}

wada_status wada_records_search(wada_records *r, const char *query, uint32_t limit,
                                wada_records_visit_fn fn, void *user, wada_err *err) {
    if (!r || !r->ops || !r->store || !fn) {
        return wada_err_set(err, WADA_ERR_INVALID, "records_search");
    }
    if (!r->ops->search) {
        return wada_err_set(err, WADA_ERR_UNSUPPORTED, "store has no search");
    }

    wada_txn *txn = NULL;
    wada_status st = r->ops->begin(r->store, WADA_STORE_READ, &txn, err);
    if (st != WADA_OK) return st;

    wada_iter *it = NULL;
    wada_slice prefix = {(const uint8_t *)r->prefix, r->prefix_len};
    st = r->ops->search(txn, prefix, query ? query : "", limit, &it, err);
    if (st != WADA_OK) {
        r->ops->rollback(txn);
        return st;
    }
    return iter_stripped(r, txn, it, fn, user, err);
}

void wada_records_sanitize_line(const char *in, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (!in) in = "";
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) {
        char ch = in[i];
        if (ch == '\n' || ch == '\r') ch = ' ';
        out[i] = ch;
    }
    out[i] = '\0';
}

bool wada_records_next_line(const char **p, const char *end, char *out, size_t cap) {
    if (!p || !*p || !end || *p >= end) return false;
    const char *nl = (const char *)memchr(*p, '\n', (size_t)(end - *p));
    const char *stop = nl ? nl : end;
    size_t n = (size_t)(stop - *p);
    if (out && cap > 0) {
        if (n >= cap) n = cap - 1;
        memcpy(out, *p, n);
        out[n] = '\0';
    }
    *p = nl ? nl + 1 : end;
    return true;
}
