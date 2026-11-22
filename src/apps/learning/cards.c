#include "cards.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Record format:
 *     v1
 *     <ease>
 *     <reps>
 *     <interval minutes>
 *     <due unix seconds>
 *     <front, one line>
 *     <back, one line>
 */

static void sanitize(const char *in, char *out, size_t cap) {
    size_t i = 0;
    if (!in) in = "";
    for (; in[i] && i + 1 < cap; i++) {
        char ch = in[i];
        if (ch == '\n' || ch == '\r') ch = ' ';
        out[i] = ch;
    }
    out[i] = '\0';
}

static bool next_line(const char **p, const char *end, char *out, size_t cap) {
    if (*p >= end) return false;
    const char *nl = (const char *)memchr(*p, '\n', (size_t)(end - *p));
    const char *stop = nl ? nl : end;
    size_t n = (size_t)(stop - *p);
    if (n >= cap) n = cap - 1;
    memcpy(out, *p, n);
    out[n] = '\0';
    *p = nl ? nl + 1 : end;
    return true;
}

static void parse_card(const char *id, const void *data, size_t len, card *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", id);
    out->ease = 2.5;

    const char *p = (const char *)data;
    const char *end = p + len;
    char line[64];

    if (!next_line(&p, end, line, sizeof line) || strcmp(line, "v1") != 0) return;
    if (!next_line(&p, end, line, sizeof line)) return;
    out->ease = atof(line);
    if (out->ease < 1.3 || out->ease > 3.0) out->ease = 2.5;
    if (!next_line(&p, end, line, sizeof line)) return;
    out->reps = atoi(line);
    if (!next_line(&p, end, line, sizeof line)) return;
    out->interval_min = atoi(line);
    if (!next_line(&p, end, line, sizeof line)) return;
    out->due = atoll(line);

    if (!next_line(&p, end, out->front, sizeof out->front)) return;
    size_t back_len = (size_t)(end - p);
    if (back_len >= sizeof out->back) back_len = sizeof out->back - 1;
    memcpy(out->back, p, back_len);
    out->back[back_len] = '\0';
}

static void card_key(const char *id, char *out, size_t cap) {
    snprintf(out, cap, "card/%s", id);
}

/* The id is the suffix; the key always gets the prefix from card_key(). A
 * reloaded card must stay writable with the same key it was read from. */
static bool id_taken(cards *c, const char *id) {
    char key[96];
    card_key(id, key, sizeof key);
    wada_txn *txn = NULL;
    if (c->ops->begin(c->store, WADA_STORE_READ, &txn, NULL) != WADA_OK) return false;
    wada_slice k = {(const uint8_t *)key, strlen(key)};
    wada_slice value;
    bool taken = c->ops->get(txn, k, &value, NULL) == WADA_OK;
    c->ops->rollback(txn);
    return taken;
}

static void fresh_id(cards *c, char *out, size_t cap) {
    for (int attempt = 0; attempt < 8; attempt++) {
        wada_id_make(out, cap);
        if (!id_taken(c, out)) return;
    }
}

static bool append_item(cards *c, const card *item) {
    if (c->count == c->cap) {
        int cap = c->cap ? c->cap * 2 : 16;
        card *v = (card *)realloc(c->items, (size_t)cap * sizeof *v);
        if (!v) return false;
        c->items = v;
        c->cap = cap;
    }
    c->items[c->count++] = *item;
    return true;
}

static wada_status load_all(cards *c, wada_err *err) {
    wada_txn *txn = NULL;
    wada_status st = c->ops->begin(c->store, WADA_STORE_READ, &txn, err);
    if (st != WADA_OK) return st;

    wada_iter *it = NULL;
    wada_slice prefix = {(const uint8_t *)"card/", 5};
    st = c->ops->list(txn, prefix, &it, err);
    if (st != WADA_OK) {
        c->ops->rollback(txn);
        return st;
    }

    wada_slice key, value;
    while (c->ops->next(it, &key, &value, NULL) == WADA_OK) {
        /* Keys are "card/<id>"; the record keeps the id, not the key. */
        if (key.n <= 5) continue;
        char id[CARD_ID_CAP];
        size_t id_len = key.n - 5;
        if (id_len > sizeof id - 1) id_len = sizeof id - 1;
        memcpy(id, key.p + 5, id_len);
        id[id_len] = '\0';

        card item;
        parse_card(id, value.p, value.n, &item);
        if (!append_item(c, &item)) {
            c->ops->iter_close(it);
            c->ops->rollback(txn);
            return wada_err_set(err, WADA_ERR_NOMEM, "cards load");
        }
    }
    c->ops->iter_close(it);
    return c->ops->commit(txn, err);
}

static wada_status write_card(cards *c, const card *item, wada_err *err) {
    size_t front_len = strlen(item->front);
    size_t back_len = strlen(item->back);
    size_t need = 3 + 32 + 16 + 16 + 32 + front_len + 1 + back_len + 1;
    char *buf = (char *)malloc(need);
    if (!buf) return wada_err_set(err, WADA_ERR_NOMEM, "encode card");

    int head = snprintf(buf, need, "v1\n%.3f\n%d\n%d\n%lld\n%s\n", item->ease, item->reps,
                        item->interval_min, item->due, item->front);
    if (head < 0 || (size_t)head >= need) {
        free(buf);
        return wada_err_set(err, WADA_ERR, "encode card");
    }
    memcpy(buf + head, item->back, back_len);
    size_t len = (size_t)head + back_len;

    wada_txn *txn = NULL;
    wada_status st = c->ops->begin(c->store, WADA_STORE_WRITE, &txn, err);
    if (st != WADA_OK) {
        free(buf);
        return st;
    }
    char keybuf[96];
    card_key(item->id, keybuf, sizeof keybuf);
    wada_slice key = {(const uint8_t *)keybuf, strlen(keybuf)};
    wada_slice value = {(const uint8_t *)buf, len};
    st = c->ops->put(txn, key, value, err);
    if (st == WADA_OK) {
        st = c->ops->commit(txn, err);
    } else {
        c->ops->rollback(txn);
    }
    free(buf);
    return st;
}

wada_status cards_open(cards *c, const wada_store_ops *ops, const char *url, wada_err *err) {
    if (!c || !ops || !url) return wada_err_set(err, WADA_ERR_INVALID, "cards_open");
    memset(c, 0, sizeof *c);
    if (!wada_ops_ok(&ops->hdr, sizeof(wada_store_ops), WADA_STORE_ABI)) {
        return wada_err_set(err, WADA_ERR_ABI, "store provider ABI %u, expected %u", ops->hdr.version, WADA_STORE_ABI);
    }
    c->ops = ops;
    wada_status st = ops->open(url, &c->store, err);
    if (st != WADA_OK) {
        c->ops = NULL;
        return st;
    }
    st = load_all(c, err);
    if (st != WADA_OK) {
        ops->close(c->store);
        free(c->items);
        memset(c, 0, sizeof *c);
    }
    return st;
}

void cards_close(cards *c) {
    if (!c) return;
    if (c->ops && c->store) c->ops->close(c->store);
    free(c->items);
    memset(c, 0, sizeof *c);
}

int cards_count(const cards *c) {
    return c ? c->count : 0;
}

const card *cards_at(const cards *c, int index) {
    if (!c || index < 0 || index >= c->count) return NULL;
    return &c->items[index];
}

int cards_find(const cards *c, const char *id) {
    if (!c || !id) return -1;
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->items[i].id, id) == 0) return i;
    }
    return -1;
}

int cards_create(cards *c, const char *front, const char *back, wada_err *err) {
    if (!c) return -1;
    card item;
    memset(&item, 0, sizeof item);
    long long now = (long long)time(NULL);
    fresh_id(c, item.id, sizeof item.id);
    sanitize(front, item.front, sizeof item.front);
    sanitize(back, item.back, sizeof item.back);
    item.ease = 2.5;
    item.reps = 0;
    item.interval_min = 0;
    item.due = now;

    if (write_card(c, &item, err) != WADA_OK) return -1;
    if (!append_item(c, &item)) {
        wada_err_set(err, WADA_ERR_NOMEM, "cards_create");
        return -1;
    }
    return c->count - 1;
}

static void schedule(card *item, int grade, long long now) {
    if (grade <= 0) {
        item->ease -= 0.20;
        if (item->ease < 1.3) item->ease = 1.3;
        item->reps = 0;
        item->interval_min = 10;
    } else {
        double factor = grade == 1 ? 0.8 : grade == 2 ? 1.0 : 1.3;
        int base;
        if (item->reps == 0) base = 1440;           /* 1 day */
        else if (item->reps == 1) base = 6 * 1440;  /* 6 days */
        else base = (int)((double)item->interval_min * item->ease * factor);
        if (base < 10) base = 10;
        item->interval_min = base;

        item->ease += grade == 1 ? -0.15 : grade == 3 ? 0.15 : 0.0;
        if (item->ease < 1.3) item->ease = 1.3;
        if (item->ease > 3.0) item->ease = 3.0;
        item->reps++;
    }
    item->due = now + (long long)item->interval_min * 60;
}

wada_status cards_review(cards *c, int index, int grade, wada_err *err) {
    if (!c || index < 0 || index >= c->count) return wada_err_set(err, WADA_ERR_NOT_FOUND, "card %d", index);
    if (grade < 0) grade = 0;
    if (grade > 3) grade = 3;

    card updated = c->items[index];
    schedule(&updated, grade, (long long)time(NULL));
    wada_status st = write_card(c, &updated, err);
    if (st == WADA_OK) c->items[index] = updated;
    return st;
}

int cards_due(const cards *c, long long now, int *out, int cap) {
    if (!c || !out || cap <= 0) return 0;
    int n = 0;
    for (int i = 0; i < c->count && n < cap; i++) {
        if (c->items[i].due <= now) out[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int key = out[i];
        long long key_due = c->items[key].due;
        int j = i - 1;
        while (j >= 0 && c->items[out[j]].due > key_due) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}

int cards_due_count(const cards *c, long long now) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->count; i++) {
        if (c->items[i].due <= now) n++;
    }
    return n;
}

long long cards_next_due(const cards *c) {
    if (!c || c->count == 0) return 0;
    long long next = c->items[0].due;
    for (int i = 1; i < c->count; i++) {
        if (c->items[i].due < next) next = c->items[i].due;
    }
    return next;
}
