#include "notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void key_meta(const char *id, char *out, size_t cap) {
    snprintf(out, cap, "note/%s", id);
}

static void key_body(const char *id, char *out, size_t cap) {
    snprintf(out, cap, "note-body/%s", id);
}

/* An id taken in the store is drawn again: the seed makes collisions
 * unlikely, the lookup makes them impossible (and covers a clock set back). */
static bool id_taken(notes *n, const char *id) {
    char key[96];
    key_meta(id, key, sizeof key);
    wada_txn *txn = NULL;
    if (n->ops->begin(n->store, WADA_STORE_READ, &txn, NULL) != WADA_OK) return false;
    wada_slice k = {(const uint8_t *)key, strlen(key)};
    wada_slice value;
    bool taken = n->ops->get(txn, k, &value, NULL) == WADA_OK;
    n->ops->rollback(txn);
    return taken;
}

static void fresh_id(notes *n, char *out, size_t cap) {
    for (int attempt = 0; attempt < 8; attempt++) {
        wada_id_make(out, cap);
        if (!id_taken(n, out)) return;
    }
}

static void sanitize_title(const char *in, char *out, size_t cap) {
    size_t i = 0;
    if (!in) in = "";
    for (; in[i] && i + 1 < cap; i++) {
        char c = in[i];
        if (c == '\n' || c == '\r') c = ' ';
        out[i] = c;
    }
    out[i] = '\0';
}

static size_t encode_meta(const note *n, char *buf, size_t cap) {
    int len = snprintf(buf, cap, "v1\n%lld\n%s", n->updated, n->title);
    return len < 0 ? 0 : (size_t)len;
}

/* Parses both layouts. When the record carries a body (legacy), *body points
 * at it; a metadata-only record leaves *body NULL. */
static bool parse_record(const char *id, const void *data, size_t len, note *out,
                         const uint8_t **body, size_t *body_len) {
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", id);

    const char *p = (const char *)data;
    const char *end = p + len;

    const char *nl1 = (const char *)memchr(p, '\n', (size_t)(end - p));
    if (!nl1 || (nl1 - p) != 2 || p[0] != 'v' || p[1] != '1') return false;
    p = nl1 + 1;

    const char *nl2 = (const char *)memchr(p, '\n', (size_t)(end - p));
    if (!nl2) return false;
    {
        char num[32];
        size_t n = (size_t)(nl2 - p);
        if (n >= sizeof num) n = sizeof num - 1;
        memcpy(num, p, n);
        num[n] = '\0';
        out->updated = atoll(num);
    }
    p = nl2 + 1;

    const char *nl3 = (const char *)memchr(p, '\n', (size_t)(end - p));
    const char *title_end = nl3 ? nl3 : end;
    size_t tlen = (size_t)(title_end - p);
    if (tlen >= sizeof out->title) tlen = sizeof out->title - 1;
    memcpy(out->title, p, tlen);
    out->title[tlen] = '\0';

    *body = NULL;
    *body_len = 0;
    if (nl3 && nl3 + 1 <= end) {
        *body = (const uint8_t *)(nl3 + 1);
        *body_len = (size_t)(end - (nl3 + 1));
    }
    return true;
}

static bool append_item(notes *n, const note *item) {
    if (n->count == n->cap) {
        int cap = n->cap ? n->cap * 2 : 16;
        note *v = (note *)realloc(n->items, (size_t)cap * sizeof *v);
        if (!v) return false;
        n->items = v;
        n->cap = cap;
    }
    n->items[n->count++] = *item;
    return true;
}

typedef struct legacy {
    note meta;
    uint8_t *body;
    size_t body_len;
} legacy;

static wada_status migrate_legacy(notes *n, legacy *list, int count, wada_err *err) {
    if (count == 0) return WADA_OK;
    wada_txn *txn = NULL;
    wada_status st = n->ops->begin(n->store, WADA_STORE_WRITE, &txn, err);
    if (st != WADA_OK) return st;

    for (int i = 0; i < count; i++) {
        char mbuf[256];
        char mk[96], bk[96];
        size_t mlen = encode_meta(&list[i].meta, mbuf, sizeof mbuf);
        key_meta(list[i].meta.id, mk, sizeof mk);
        key_body(list[i].meta.id, bk, sizeof bk);

        wada_slice mk_s = {(const uint8_t *)mk, strlen(mk)};
        wada_slice mv_s = {(const uint8_t *)mbuf, mlen};
        wada_slice bk_s = {(const uint8_t *)bk, strlen(bk)};
        wada_slice bv_s = {list[i].body, list[i].body_len};

        st = n->ops->put(txn, mk_s, mv_s, err);
        if (st == WADA_OK) st = n->ops->put(txn, bk_s, bv_s, err);
        if (st != WADA_OK) {
            n->ops->rollback(txn);
            return st;
        }
    }
    return n->ops->commit(txn, err);
}

static wada_status load_all(notes *n, wada_err *err) {
    legacy *old = NULL;
    int old_count = 0;
    int old_cap = 0;

    wada_txn *txn = NULL;
    wada_status st = n->ops->begin(n->store, WADA_STORE_READ, &txn, err);
    if (st != WADA_OK) return st;

    wada_iter *it = NULL;
    wada_slice prefix = {(const uint8_t *)"note/", 5};
    st = n->ops->list(txn, prefix, &it, err);
    if (st != WADA_OK) {
        n->ops->rollback(txn);
        return st;
    }

    wada_slice key, value;
    while (n->ops->next(it, &key, &value, NULL) == WADA_OK) {
        /* Keys are "note/<id>"; the record keeps the id, not the key. */
        if (key.n <= 5) continue;
        char id[NOTE_ID_CAP];
        size_t id_len = key.n - 5;
        if (id_len > sizeof id - 1) id_len = sizeof id - 1;
        memcpy(id, key.p + 5, id_len);
        id[id_len] = '\0';

        note meta;
        const uint8_t *body = NULL;
        size_t body_len = 0;
        if (!parse_record(id, value.p, value.n, &meta, &body, &body_len)) continue;

        if (!append_item(n, &meta)) {
            n->ops->iter_close(it);
            n->ops->rollback(txn);
            free(old);
            return wada_err_set(err, WADA_ERR_NOMEM, "notes load");
        }

        if (body) {
            if (old_count == old_cap) {
                int cap = old_cap ? old_cap * 2 : 8;
                legacy *v = (legacy *)realloc(old, (size_t)cap * sizeof *v);
                if (!v) {
                    n->ops->iter_close(it);
                    n->ops->rollback(txn);
                    free(old);
                    return wada_err_set(err, WADA_ERR_NOMEM, "notes migrate");
                }
                old = v;
                old_cap = cap;
            }
            uint8_t *copy = (uint8_t *)malloc(body_len);
            if (!copy) {
                n->ops->iter_close(it);
                n->ops->rollback(txn);
                free(old);
                return wada_err_set(err, WADA_ERR_NOMEM, "notes migrate");
            }
            memcpy(copy, body, body_len);
            old[old_count].meta = meta;
            old[old_count].body = copy;
            old[old_count].body_len = body_len;
            old_count++;
        }
    }
    n->ops->iter_close(it);
    st = n->ops->commit(txn, err);
    if (st != WADA_OK) {
        free(old);
        return st;
    }

    if (old_count > 0) st = migrate_legacy(n, old, old_count, err);
    for (int i = 0; i < old_count; i++) free(old[i].body);
    free(old);
    return st;
}

wada_status notes_open(notes *n, const wada_store_ops *ops, const char *url, wada_err *err) {
    if (!n || !ops || !url) return wada_err_set(err, WADA_ERR_INVALID, "notes_open");
    memset(n, 0, sizeof *n);
    if (!wada_ops_ok(&ops->hdr, sizeof(wada_store_ops), WADA_STORE_ABI)) {
        return wada_err_set(err, WADA_ERR_ABI, "store provider ABI %u, expected %u", ops->hdr.version, WADA_STORE_ABI);
    }
    n->ops = ops;
    wada_status st = ops->open(url, &n->store, err);
    if (st != WADA_OK) {
        n->ops = NULL;
        return st;
    }
    st = load_all(n, err);
    if (st != WADA_OK) {
        ops->close(n->store);
        free(n->items);
        memset(n, 0, sizeof *n);
    }
    return st;
}

void notes_close(notes *n) {
    if (!n) return;
    if (n->ops && n->store) n->ops->close(n->store);
    free(n->items);
    memset(n, 0, sizeof *n);
}

int notes_count(const notes *n) {
    return n ? n->count : 0;
}

const note *notes_at(const notes *n, int index) {
    if (!n || index < 0 || index >= n->count) return NULL;
    return &n->items[index];
}

int notes_find(const notes *n, const char *id) {
    if (!n || !id) return -1;
    for (int i = 0; i < n->count; i++) {
        if (strcmp(n->items[i].id, id) == 0) return i;
    }
    return -1;
}

wada_status notes_body(notes *n, int index, char *out, size_t cap, wada_err *err) {
    if (!n || !out || cap == 0) return wada_err_set(err, WADA_ERR_INVALID, "notes_body");
    out[0] = '\0';
    if (index < 0 || index >= n->count) return wada_err_set(err, WADA_ERR_NOT_FOUND, "note %d", index);

    char bk[96];
    key_body(n->items[index].id, bk, sizeof bk);
    wada_txn *txn = NULL;
    wada_status st = n->ops->begin(n->store, WADA_STORE_READ, &txn, err);
    if (st != WADA_OK) return st;

    wada_slice k = {(const uint8_t *)bk, strlen(bk)};
    wada_slice value;
    st = n->ops->get(txn, k, &value, err);
    if (st == WADA_OK) {
        size_t len = value.n < cap - 1 ? value.n : cap - 1;
        memcpy(out, value.p, len);
        out[len] = '\0';
    } else if (st == WADA_ERR_NOT_FOUND) {
        out[0] = '\0';
        wada_err_clear(err);
        st = WADA_OK;
    }
    wada_status commit = n->ops->commit(txn, err);
    return st != WADA_OK ? st : commit;
}

static wada_status write_note(notes *n, const note *item, const char *body, wada_err *err) {
    char mbuf[256];
    size_t mlen = encode_meta(item, mbuf, sizeof mbuf);
    char mk[96], bk[96];
    key_meta(item->id, mk, sizeof mk);
    key_body(item->id, bk, sizeof bk);

    wada_txn *txn = NULL;
    wada_status st = n->ops->begin(n->store, WADA_STORE_WRITE, &txn, err);
    if (st != WADA_OK) return st;

    wada_slice mk_s = {(const uint8_t *)mk, strlen(mk)};
    wada_slice mv_s = {(const uint8_t *)mbuf, mlen};
    wada_slice bk_s = {(const uint8_t *)bk, strlen(bk)};
    wada_slice bv_s = {(const uint8_t *)(body ? body : ""), body ? strlen(body) : 0};

    st = n->ops->put(txn, mk_s, mv_s, err);
    if (st == WADA_OK) st = n->ops->put(txn, bk_s, bv_s, err);
    if (st == WADA_OK) {
        st = n->ops->commit(txn, err);
    } else {
        n->ops->rollback(txn);
    }
    return st;
}

int notes_create(notes *n, wada_err *err) {
    if (!n) return -1;
    note item;
    memset(&item, 0, sizeof item);
    long long now = (long long)time(NULL);
    fresh_id(n, item.id, sizeof item.id);
    snprintf(item.title, sizeof item.title, "Untitled");
    item.updated = now;

    if (write_note(n, &item, "", err) != WADA_OK) return -1;
    if (!append_item(n, &item)) {
        wada_err_set(err, WADA_ERR_NOMEM, "notes_create");
        return -1;
    }
    return n->count - 1;
}

wada_status notes_save(notes *n, int index, const char *title, const char *body, wada_err *err) {
    if (!n || index < 0 || index >= n->count) return wada_err_set(err, WADA_ERR_NOT_FOUND, "note %d", index);

    note updated = n->items[index];
    sanitize_title(title, updated.title, sizeof updated.title);
    updated.updated = (long long)time(NULL);

    wada_status st = write_note(n, &updated, body ? body : "", err);
    if (st == WADA_OK) n->items[index] = updated;
    return st;
}

wada_status notes_remove(notes *n, int index, wada_err *err) {
    if (!n || index < 0 || index >= n->count) return wada_err_set(err, WADA_ERR_NOT_FOUND, "note %d", index);

    char mk[96], bk[96];
    key_meta(n->items[index].id, mk, sizeof mk);
    key_body(n->items[index].id, bk, sizeof bk);

    wada_txn *txn = NULL;
    wada_status st = n->ops->begin(n->store, WADA_STORE_WRITE, &txn, err);
    if (st != WADA_OK) return st;

    wada_slice mk_s = {(const uint8_t *)mk, strlen(mk)};
    wada_slice bk_s = {(const uint8_t *)bk, strlen(bk)};
    st = n->ops->del(txn, mk_s, err);
    if (st == WADA_OK) st = n->ops->del(txn, bk_s, err);
    if (st == WADA_OK) {
        st = n->ops->commit(txn, err);
    } else {
        n->ops->rollback(txn);
    }
    if (st != WADA_OK) return st;

    memmove(&n->items[index], &n->items[index + 1], (size_t)(n->count - index - 1) * sizeof(note));
    n->count--;
    return WADA_OK;
}

static int search_index_for_key(notes *n, const uint8_t *key, size_t len) {
    const char *s = (const char *)key;
    static const char body_prefix[] = "note-body/";
    static const char meta_prefix[] = "note/";
    const char *id = NULL;
    size_t id_len = 0;
    if (len > sizeof body_prefix - 1 && memcmp(s, body_prefix, sizeof body_prefix - 1) == 0) {
        id = s + sizeof body_prefix - 1;
        id_len = len - (sizeof body_prefix - 1);
    } else if (len > sizeof meta_prefix - 1 && memcmp(s, meta_prefix, sizeof meta_prefix - 1) == 0) {
        id = s + sizeof meta_prefix - 1;
        id_len = len - (sizeof meta_prefix - 1);
    } else {
        return -1;
    }
    for (int i = 0; i < n->count; i++) {
        if (strlen(n->items[i].id) == id_len && memcmp(n->items[i].id, id, id_len) == 0) return i;
    }
    return -1;
}

int notes_search(notes *n, const char *query, int *out, int cap) {
    if (!n || !out || cap <= 0 || !query || !query[0]) return 0;
    if (!(n->ops->caps(n->store) & WADA_STORE_CAP_SEARCH)) return -1;
    if (!n->ops->search) return -1;

    wada_txn *txn = NULL;
    wada_err err;
    wada_err_clear(&err);
    if (n->ops->begin(n->store, WADA_STORE_READ, &txn, &err) != WADA_OK) return 0;

    wada_iter *it = NULL;
    wada_slice prefix = {(const uint8_t *)"note", 4};
    int count = 0;
    if (n->ops->search(txn, prefix, query, 200, &it, &err) == WADA_OK) {
        wada_slice key, value;
        while (count < cap && n->ops->next(it, &key, &value, &err) == WADA_OK) {
            int index = search_index_for_key(n, key.p, key.n);
            if (index < 0) continue;
            bool seen = false;
            for (int i = 0; i < count; i++) {
                if (out[i] == index) {
                    seen = true;
                    break;
                }
            }
            if (!seen) out[count++] = index;
        }
        n->ops->iter_close(it);
    }
    n->ops->commit(txn, &err);
    return count;
}

wada_status notes_body_size(notes *n, int index, size_t *out_len, wada_err *err) {
    if (!n || !out_len) return wada_err_set(err, WADA_ERR_INVALID, "notes_body_size");
    *out_len = 0;
    if (index < 0 || index >= n->count) return wada_err_set(err, WADA_ERR_NOT_FOUND, "note %d", index);

    char bk[96];
    key_body(n->items[index].id, bk, sizeof bk);
    wada_txn *txn = NULL;
    wada_status st = n->ops->begin(n->store, WADA_STORE_READ, &txn, err);
    if (st != WADA_OK) return st;

    wada_slice k = {(const uint8_t *)bk, strlen(bk)};
    wada_slice value;
    st = n->ops->get(txn, k, &value, err);
    if (st == WADA_OK) {
        *out_len = value.n;
    } else if (st == WADA_ERR_NOT_FOUND) {
        wada_err_clear(err);
        st = WADA_OK;
    }
    wada_status commit = n->ops->commit(txn, err);
    return st != WADA_OK ? st : commit;
}

/* --- Backup ---------------------------------------------------------------
 * Layout:
 *   wada-notes 1\n
 *   <count>\n
 *   <updated> <title_len> <body_len>\n
 *   <title bytes><body bytes>\n
 */

wada_status notes_export(notes *n, const char *path, wada_err *err) {
    if (!n || !path) return wada_err_set(err, WADA_ERR_INVALID, "notes_export");
    FILE *f = fopen(path, "wb");
    if (!f) return wada_err_set(err, WADA_ERR_IO, "cannot write %s", path);

    wada_status st = WADA_OK;
    fprintf(f, "wada-notes 1\n%d\n", n->count);
    for (int i = 0; i < n->count && st == WADA_OK; i++) {
        size_t body_len = 0;
        st = notes_body_size(n, i, &body_len, err);
        if (st != WADA_OK) break;

        char *body = (char *)malloc(body_len + 1);
        if (!body) {
            st = wada_err_set(err, WADA_ERR_NOMEM, "notes_export");
            break;
        }
        st = notes_body(n, i, body, body_len + 1, err);
        if (st == WADA_OK) {
            const char *title = n->items[i].title;
            size_t title_len = strlen(title);
            fprintf(f, "%lld %zu %zu\n", n->items[i].updated, title_len, body_len);
            fwrite(title, 1, title_len, f);
            fwrite(body, 1, body_len, f);
            fputc('\n', f);
        }
        free(body);
    }
    if (ferror(f)) st = wada_err_set(err, WADA_ERR_IO, "write failed: %s", path);
    fclose(f);
    return st;
}

typedef struct reader {
    const char *p;
    const char *end;
} reader;

static bool read_line(reader *r, const char **start, size_t *len) {
    if (r->p >= r->end) return false;
    const char *nl = (const char *)memchr(r->p, '\n', (size_t)(r->end - r->p));
    const char *stop = nl ? nl : r->end;
    *start = r->p;
    *len = (size_t)(stop - r->p);
    r->p = nl ? nl + 1 : r->end;
    return true;
}

static bool line_is(const char *s, size_t len, const char *text) {
    return len == strlen(text) && memcmp(s, text, len) == 0;
}

wada_status notes_import(notes *n, const char *path, int *added, wada_err *err) {
    if (!n || !path) return wada_err_set(err, WADA_ERR_INVALID, "notes_import");
    if (added) *added = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return wada_err_set(err, WADA_ERR_IO, "cannot read %s", path);
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return wada_err_set(err, WADA_ERR_IO, "cannot seek %s", path);
    }
    long size = ftell(f);
    if (size < 0 || size > 256L * 1024L * 1024L) {
        fclose(f);
        return wada_err_set(err, WADA_ERR_INVALID, "file too large");
    }
    rewind(f);
    char *data = (char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return wada_err_set(err, WADA_ERR_NOMEM, "notes_import");
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[got] = '\0';

    reader r = {data, data + got};
    const char *line = NULL;
    size_t line_len = 0;
    if (!read_line(&r, &line, &line_len) || !line_is(line, line_len, "wada-notes 1")) {
        free(data);
        return wada_err_set(err, WADA_ERR_INVALID, "not a wada-notes file");
    }
    if (!read_line(&r, &line, &line_len)) {
        free(data);
        return wada_err_set(err, WADA_ERR_INVALID, "missing count");
    }
    char count_buf[32];
    size_t copy = line_len < sizeof count_buf - 1 ? line_len : sizeof count_buf - 1;
    memcpy(count_buf, line, copy);
    count_buf[copy] = '\0';
    int count = atoi(count_buf);
    if (count < 0) count = 0;

    int imported = 0;
    wada_status st = WADA_OK;
    for (int i = 0; i < count; i++) {
        if (r.p < r.end && *r.p == '\n') r.p++;
        if (!read_line(&r, &line, &line_len)) {
            st = wada_err_set(err, WADA_ERR_INVALID, "truncated header");
            break;
        }
        char header[128];
        copy = line_len < sizeof header - 1 ? line_len : sizeof header - 1;
        memcpy(header, line, copy);
        header[copy] = '\0';

        long long updated = 0;
        size_t title_len = 0;
        size_t body_len = 0;
        if (sscanf(header, "%lld %zu %zu", &updated, &title_len, &body_len) != 3) {
            st = wada_err_set(err, WADA_ERR_INVALID, "bad record header");
            break;
        }
        if ((size_t)(r.end - r.p) < title_len + body_len) {
            st = wada_err_set(err, WADA_ERR_INVALID, "truncated record");
            break;
        }

        note item;
        memset(&item, 0, sizeof item);
        fresh_id(n, item.id, sizeof item.id);
        /* The title is length-delimited in the file; sanitize a terminated copy
         * so it cannot run into the body bytes that follow. */
        char raw_title[NOTE_TITLE_CAP * 4];
        size_t raw_len = title_len < sizeof raw_title - 1 ? title_len : sizeof raw_title - 1;
        memcpy(raw_title, r.p, raw_len);
        raw_title[raw_len] = '\0';
        sanitize_title(raw_title, item.title, sizeof item.title);
        item.updated = updated;

        char *body = (char *)malloc(body_len + 1);
        if (!body) {
            st = wada_err_set(err, WADA_ERR_NOMEM, "notes_import");
            break;
        }
        memcpy(body, r.p + title_len, body_len);
        body[body_len] = '\0';
        r.p += title_len + body_len;

        st = write_note(n, &item, body, err);
        free(body);
        if (st != WADA_OK) break;
        if (!append_item(n, &item)) {
            st = wada_err_set(err, WADA_ERR_NOMEM, "notes_import");
            break;
        }
        imported++;
    }

    free(data);
    if (added) *added = imported;
    return st;
}
