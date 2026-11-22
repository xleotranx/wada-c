/* Todo product — an example with persistence.
 *
 * It shows how a product uses the "store" service without knowing the vendor:
 * the vtable comes from the registry, keys live under a private prefix, and
 * each record is a tiny versioned text format. There is no SQL here.
 *
 * Run: build\wada_todo.exe
 */
#include <cui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wada_app.h"
#include "wada_iface.h"
#include "wada_log.h"
#include "wada_product.h"

#define TODO_TEXT_CAP 256

typedef struct todo {
    char id[48];
    bool done;
    char text[TODO_TEXT_CAP];
} todo;

typedef struct todo_state {
    wada_app *app;
    const wada_store_ops *ops;
    wada_store *store;
    todo *items;
    int count;
    int cap;
    char draft[TODO_TEXT_CAP];
    char status[96];
} todo_state;

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

static void todo_key(const char *id, char *out, size_t cap) {
    snprintf(out, cap, "todo/%s", id);
}

/* The id is the suffix; every write and delete builds the key with the prefix,
 * so a record loaded from disk is written back under the key it came from. */
static bool id_taken(todo_state *st, const char *id) {
    char key[96];
    todo_key(id, key, sizeof key);
    wada_txn *txn = NULL;
    if (st->ops->begin(st->store, WADA_STORE_READ, &txn, NULL) != WADA_OK) return false;
    wada_slice k = {(const uint8_t *)key, strlen(key)};
    wada_slice value;
    bool taken = st->ops->get(txn, k, &value, NULL) == WADA_OK;
    st->ops->rollback(txn);
    return taken;
}

static void fresh_id(todo_state *st, char *out, size_t cap) {
    for (int attempt = 0; attempt < 8; attempt++) {
        wada_id_make(out, cap);
        if (!id_taken(st, out)) return;
    }
}

static bool append_item(todo_state *st, const todo *item) {
    if (st->count == st->cap) {
        int cap = st->cap ? st->cap * 2 : 16;
        todo *v = (todo *)realloc(st->items, (size_t)cap * sizeof *v);
        if (!v) return false;
        st->items = v;
        st->cap = cap;
    }
    st->items[st->count++] = *item;
    return true;
}

static wada_status load_all(todo_state *st, wada_err *err) {
    wada_txn *txn = NULL;
    wada_status status = st->ops->begin(st->store, WADA_STORE_READ, &txn, err);
    if (status != WADA_OK) return status;

    wada_iter *it = NULL;
    wada_slice prefix = {(const uint8_t *)"todo/", 5};
    status = st->ops->list(txn, prefix, &it, err);
    if (status != WADA_OK) {
        st->ops->rollback(txn);
        return status;
    }

    wada_slice key, value;
    while (st->ops->next(it, &key, &value, NULL) == WADA_OK) {
        /* Keys are "todo/<id>"; the record keeps the id, not the key. */
        if (key.n <= 5) continue;
        todo item;
        memset(&item, 0, sizeof item);
        size_t id_len = key.n - 5;
        if (id_len > sizeof item.id - 1) id_len = sizeof item.id - 1;
        memcpy(item.id, key.p + 5, id_len);

        const char *p = (const char *)value.p;
        const char *end = p + value.n;
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (nl) {
            p = nl + 1;
            if (p < end && (*p == '0' || *p == '1')) item.done = *p == '1';
            nl = (const char *)memchr(p, '\n', (size_t)(end - p));
            if (nl) {
                p = nl + 1;
                size_t len = (size_t)(end - p);
                if (len >= sizeof item.text) len = sizeof item.text - 1;
                memcpy(item.text, p, len);
                item.text[len] = '\0';
            }
        }
        if (!append_item(st, &item)) {
            st->ops->iter_close(it);
            st->ops->rollback(txn);
            return wada_err_set(err, WADA_ERR_NOMEM, "todo load");
        }
    }
    st->ops->iter_close(it);
    return st->ops->commit(txn, err);
}

static wada_status write_item(todo_state *st, const todo *item, wada_err *err) {
    char buf[TODO_TEXT_CAP + 64];
    int head = snprintf(buf, sizeof buf, "v1\n%c\n", item->done ? '1' : '0');
    size_t len = (size_t)head + strlen(item->text);
    memcpy(buf + head, item->text, strlen(item->text));

    wada_txn *txn = NULL;
    wada_status status = st->ops->begin(st->store, WADA_STORE_WRITE, &txn, err);
    if (status != WADA_OK) return status;
    char keybuf[96];
    todo_key(item->id, keybuf, sizeof keybuf);
    wada_slice key = {(const uint8_t *)keybuf, strlen(keybuf)};
    wada_slice value = {(const uint8_t *)buf, len};
    status = st->ops->put(txn, key, value, err);
    if (status == WADA_OK) {
        status = st->ops->commit(txn, err);
    } else {
        st->ops->rollback(txn);
    }
    return status;
}

static bool add_text(todo_state *st, const char *text, wada_err *err) {
    if (!text || !text[0]) return false;
    todo item;
    memset(&item, 0, sizeof item);
    fresh_id(st, item.id, sizeof item.id);
    sanitize(text, item.text, sizeof item.text);

    if (write_item(st, &item, err) != WADA_OK) return false;
    if (!append_item(st, &item)) {
        wada_err_set(err, WADA_ERR_NOMEM, "todo add");
        return false;
    }
    return true;
}

static void clear_done(todo_state *st) {
    wada_err err;
    wada_txn *txn = NULL;
    if (st->ops->begin(st->store, WADA_STORE_WRITE, &txn, &err) != WADA_OK) return;
    for (int i = 0; i < st->count; i++) {
        if (!st->items[i].done) continue;
        char keybuf[96];
        todo_key(st->items[i].id, keybuf, sizeof keybuf);
        wada_slice key = {(const uint8_t *)keybuf, strlen(keybuf)};
        if (st->ops->del(txn, key, &err) != WADA_OK) {
            st->ops->rollback(txn);
            snprintf(st->status, sizeof st->status, "%s", wada_err_msg(&err));
            return;
        }
    }
    if (st->ops->commit(txn, &err) != WADA_OK) {
        snprintf(st->status, sizeof st->status, "%s", wada_err_msg(&err));
        return;
    }

    int write = 0;
    for (int i = 0; i < st->count; i++) {
        if (!st->items[i].done) st->items[write++] = st->items[i];
    }
    st->count = write;
    snprintf(st->status, sizeof st->status, "cleared done items");
}

static void cmd_add(void *user) {
    todo_state *st = (todo_state *)user;
    wada_err err;
    if (add_text(st, st->draft, &err)) {
        st->draft[0] = '\0';
        wada_app_set_status(st->app, "added");
    } else {
        wada_app_set_status(st->app, wada_err_msg(&err));
    }
}

static void cmd_clear(void *user) {
    todo_state *st = (todo_state *)user;
    clear_done(st);
    wada_app_set_status(st->app, st->status);
}

static void *todo_enter(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err) {
    todo_state *st = (todo_state *)calloc(1, sizeof *st);
    if (!st) {
        wada_err_set(err, WADA_ERR_NOMEM, "todo");
        return NULL;
    }
    st->app = app;
    st->ops = (const wada_store_ops *)wada_registry_get(wada_app_registry(app), "store",
                                                       WADA_STORE_ABI, NULL);
    if (!st->ops) {
        wada_err_set(err, WADA_ERR_NOT_FOUND, "todo: no store service");
        free(st);
        return NULL;
    }
    char path[640];
    snprintf(path, sizeof path, "%s" WADA_PATH_SEP "wada.db", data_dir);
    if (st->ops->open(path, &st->store, err) != WADA_OK) {
        free(st);
        return NULL;
    }
    if (load_all(st, err) != WADA_OK) {
        st->ops->close(st->store);
        free(st);
        return NULL;
    }
    if (st->count == 0) {
        /* First run: one record so the example shows the whole loop. */
        add_text(st, "Try checking this off — the record lives in SQLite.", err);
    }

    wada_app_command(app, scope, "todo.add", "Add a todo", "Ctrl+N", cmd_add, st);
    wada_app_command(app, scope, "todo.clear", "Clear finished todos", "Ctrl+Shift+C", cmd_clear, st);
    return st;
}

static void todo_leave(void *state) {
    todo_state *st = (todo_state *)state;
    if (!st) return;
    st->ops->close(st->store);
    free(st->items);
    free(st);
}

static void todo_frame(void *state, wada_app *app) {
    todo_state *st = (todo_state *)state;
    st->app = app;
    cui_ctx *ui = wada_app_ui(app);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad_left = 28, .pad_right = 28,
               .pad_top = 20, .gap = 10) {
        cui_text(ui, "Todo", .font = CUI_FONT_DISPLAY, .size = 26);
        cui_text(ui, "An example product: versioned records behind the store service.",
                 .size = 12, .color = CUI_COLOR_TEXT_MUTED);

        cui_row(ui, .width = CUI_FILL, .gap = 8) {
            cui_input(ui, "todo-draft", st->draft, sizeof st->draft,
                      .placeholder = "What needs doing?", .width = CUI_FILL);
            if (cui_button(ui, "Add", .variant = CUI_BUTTON_PRIMARY)) cmd_add(st);
        }

        if (st->count == 0) {
            cui_text(ui, "Nothing yet.", .size = 13, .color = CUI_COLOR_TEXT_MUTED);
        } else {
            int pending = 0;
            for (int i = 0; i < st->count; i++) {
                if (!st->items[i].done) pending++;
            }
            cui_textf(ui, "%d open / %d total", pending, st->count);

            cui_scroll(ui, .width = CUI_FILL, .height = CUI_FILL) {
                for (int i = 0; i < st->count; i++) {
                    bool before = st->items[i].done;
                    if (cui_checkbox(ui, st->items[i].text, &st->items[i].done)) {
                        wada_err err;
                        if (write_item(st, &st->items[i], &err) != WADA_OK) {
                            st->items[i].done = before;
                            snprintf(st->status, sizeof st->status, "%s", wada_err_msg(&err));
                            wada_app_set_status(st->app, st->status);
                        }
                    }
                }
            }
        }

        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_END, .gap = 8) {
            if (cui_button(ui, "Clear done", .variant = CUI_BUTTON_OUTLINE)) cmd_clear(st);
            if (cui_button(ui, "Quit", .variant = CUI_BUTTON_GHOST)) wada_app_quit(app);
        }
        cui_text(ui, st->status, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
    }
}

const wada_product todo_product = {
    "todo",
    "Todo",
    todo_enter,
    todo_leave,
    todo_frame,
};
