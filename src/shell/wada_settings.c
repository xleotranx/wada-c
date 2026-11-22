#include "wada_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wada_settings {
    const wada_store_ops *ops;
    wada_store *store;
};

static bool make_key(const char *key, char *out, size_t cap) {
    if (!key || !key[0]) return false;
    int n = snprintf(out, cap, "settings/%s", key);
    return n > 0 && (size_t)n < cap;
}

wada_settings *wada_settings_open(const wada_store_ops *ops, const char *url, wada_err *err) {
    if (!ops || !url) {
        wada_err_set(err, WADA_ERR_INVALID, "settings: missing provider or url");
        return NULL;
    }
    if (!wada_ops_ok(&ops->hdr, sizeof(wada_store_ops), WADA_STORE_ABI)) {
        wada_err_set(err, WADA_ERR_ABI, "settings: store ABI %u, expected %u", ops->hdr.version, WADA_STORE_ABI);
        return NULL;
    }
    wada_settings *s = (wada_settings *)calloc(1, sizeof *s);
    if (!s) {
        wada_err_set(err, WADA_ERR_NOMEM, "settings");
        return NULL;
    }
    s->ops = ops;
    if (ops->open(url, &s->store, err) != WADA_OK) {
        free(s);
        return NULL;
    }
    return s;
}

void wada_settings_close(wada_settings *s) {
    if (!s) return;
    if (s->ops && s->store) s->ops->close(s->store);
    free(s);
}

wada_status wada_settings_get(wada_settings *s, const char *key, char *out, size_t cap, wada_err *err) {
    if (out && cap) out[0] = '\0';
    if (!s || !out || cap == 0) return wada_err_set(err, WADA_ERR_INVALID, "settings get");

    char full[128];
    if (!make_key(key, full, sizeof full)) return wada_err_set(err, WADA_ERR_INVALID, "settings key");

    wada_txn *txn = NULL;
    wada_status st = s->ops->begin(s->store, WADA_STORE_READ, &txn, err);
    if (st != WADA_OK) return st;

    wada_slice k = {(const uint8_t *)full, strlen(full)};
    wada_slice value;
    st = s->ops->get(txn, k, &value, err);
    if (st == WADA_OK) {
        size_t n = value.n < cap - 1 ? value.n : cap - 1;
        memcpy(out, value.p, n);
        out[n] = '\0';
    }
    wada_status commit = s->ops->commit(txn, err);
    return st != WADA_OK ? st : commit;
}

wada_status wada_settings_set(wada_settings *s, const char *key, const char *value, wada_err *err) {
    if (!s || !value) return wada_err_set(err, WADA_ERR_INVALID, "settings set");

    char full[128];
    if (!make_key(key, full, sizeof full)) return wada_err_set(err, WADA_ERR_INVALID, "settings key");

    wada_txn *txn = NULL;
    wada_status st = s->ops->begin(s->store, WADA_STORE_WRITE, &txn, err);
    if (st != WADA_OK) return st;

    wada_slice k = {(const uint8_t *)full, strlen(full)};
    wada_slice v = {(const uint8_t *)value, strlen(value)};
    st = s->ops->put(txn, k, v, err);
    if (st == WADA_OK) {
        st = s->ops->commit(txn, err);
    } else {
        s->ops->rollback(txn);
    }
    return st;
}

int wada_settings_int(wada_settings *s, const char *key, int fallback) {
    char buf[32];
    if (!s) return fallback;
    if (wada_settings_get(s, key, buf, sizeof buf, NULL) != WADA_OK) return fallback;
    if (!buf[0]) return fallback;
    return atoi(buf);
}

wada_status wada_settings_set_int(wada_settings *s, const char *key, int value, wada_err *err) {
    char buf[32];
    snprintf(buf, sizeof buf, "%d", value);
    return wada_settings_set(s, key, buf, err);
}
