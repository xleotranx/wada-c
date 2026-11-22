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

static void parse_card(const char *id, const void *data, size_t len, card *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", id);
    out->ease = 2.5;

    const char *p = (const char *)data;
    const char *end = p + len;
    char line[64];

    if (!wada_records_next_line(&p, end, line, sizeof line) || strcmp(line, "v1") != 0) return;
    if (!wada_records_next_line(&p, end, line, sizeof line)) return;
    out->ease = atof(line);
    if (out->ease < 1.3 || out->ease > 3.0) out->ease = 2.5;
    if (!wada_records_next_line(&p, end, line, sizeof line)) return;
    out->reps = atoi(line);
    if (!wada_records_next_line(&p, end, line, sizeof line)) return;
    out->interval_min = atoi(line);
    if (!wada_records_next_line(&p, end, line, sizeof line)) return;
    out->due = atoll(line);

    if (!wada_records_next_line(&p, end, out->front, sizeof out->front)) return;
    size_t back_len = (size_t)(end - p);
    if (back_len >= sizeof out->back) back_len = sizeof out->back - 1;
    memcpy(out->back, p, back_len);
    out->back[back_len] = '\0';
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

typedef struct cards_load_ctx {
    cards *c;
    bool oom;
} cards_load_ctx;

static bool load_visitor(const char *id, wada_slice value, void *user) {
    cards_load_ctx *ctx = (cards_load_ctx *)user;
    card item;
    parse_card(id, value.p, value.n, &item);
    if (!append_item(ctx->c, &item)) {
        ctx->oom = true;
        return false;
    }
    return true;
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
    wada_status st = wada_records_put(&c->rec, item->id, buf, (size_t)head + back_len, err);
    free(buf);
    return st;
}

wada_status cards_open(cards *c, const wada_store_ops *ops, const char *url, wada_err *err) {
    if (!c || !ops || !url) return wada_err_set(err, WADA_ERR_INVALID, "cards_open");
    memset(c, 0, sizeof *c);
    wada_status st = wada_records_open(&c->rec, ops, url, "card/", err);
    if (st != WADA_OK) return st;

    cards_load_ctx ctx = {c, false};
    st = wada_records_each(&c->rec, load_visitor, &ctx, err);
    if (ctx.oom) st = wada_err_set(err, WADA_ERR_NOMEM, "cards load");
    if (st != WADA_OK) cards_close(c);
    return st;
}

void cards_close(cards *c) {
    if (!c) return;
    wada_records_close(&c->rec);
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
    wada_records_fresh_id(&c->rec, item.id, sizeof item.id);
    wada_records_sanitize_line(front, item.front, sizeof item.front);
    wada_records_sanitize_line(back, item.back, sizeof item.back);
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
