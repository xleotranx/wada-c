/* Todo product — an example with persistence.
 *
 * It shows how a product uses the "store" service through wada_records and
 * wada_ui: keys live under a private prefix ("todo/"), and each record is a
 * tiny versioned text format. There is no SQL here.
 *
 * Run: build\wada_todo.exe
 */
#include <cui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wada_app.h"
#include "wada_log.h"
#include "wada_product.h"
#include "wada_records.h"
#include "wada_ui.h"

#define TODO_TEXT_CAP 256

typedef struct todo {
    char id[48];
    bool done;
    char text[TODO_TEXT_CAP];
} todo;

typedef struct todo_state {
    wada_app *app;
    wada_records rec;
    todo *items;
    int count;
    int cap;
    char draft[TODO_TEXT_CAP];
    char status[96];
} todo_state;

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

typedef struct todo_load_ctx {
    todo_state *st;
    bool oom;
} todo_load_ctx;

static bool load_visitor(const char *id, wada_slice value, void *user) {
    todo_load_ctx *ctx = (todo_load_ctx *)user;
    todo item;
    memset(&item, 0, sizeof item);
    snprintf(item.id, sizeof item.id, "%s", id);

    const char *p = (const char *)value.p;
    const char *end = p + value.n;
    char line[16];
    if (wada_records_next_line(&p, end, line, sizeof line) &&
        wada_records_next_line(&p, end, line, sizeof line)) {
        item.done = line[0] == '1';
        size_t len = (size_t)(end - p);
        if (len >= sizeof item.text) len = sizeof item.text - 1;
        memcpy(item.text, p, len);
        item.text[len] = '\0';
    }
    if (!append_item(ctx->st, &item)) {
        ctx->oom = true;
        return false;
    }
    return true;
}

static wada_status write_item(todo_state *st, const todo *item, wada_err *err) {
    char buf[TODO_TEXT_CAP + 64];
    size_t text_len = strlen(item->text);
    int head = snprintf(buf, sizeof buf, "v1\n%c\n", item->done ? '1' : '0');
    if (head < 0 || (size_t)head + text_len >= sizeof buf) {
        return wada_err_set(err, WADA_ERR, "encode todo");
    }
    memcpy(buf + head, item->text, text_len);
    return wada_records_put(&st->rec, item->id, buf, (size_t)head + text_len, err);
}

static bool add_text(todo_state *st, const char *text, wada_err *err) {
    if (!text || !text[0]) return false;
    todo item;
    memset(&item, 0, sizeof item);
    wada_records_fresh_id(&st->rec, item.id, sizeof item.id);
    wada_records_sanitize_line(text, item.text, sizeof item.text);

    if (write_item(st, &item, err) != WADA_OK) return false;
    if (!append_item(st, &item)) {
        wada_err_set(err, WADA_ERR_NOMEM, "todo add");
        return false;
    }
    return true;
}

static void clear_done(todo_state *st) {
    wada_err err;
    int write = 0;
    for (int i = 0; i < st->count; i++) {
        if (st->items[i].done) {
            if (wada_records_del(&st->rec, st->items[i].id, &err) != WADA_OK) {
                /* Keep items not yet deleted. */
                for (int j = i; j < st->count; j++) st->items[write++] = st->items[j];
                st->count = write;
                wada_ui_status_err(st->app, st->status, sizeof st->status, &err);
                return;
            }
        } else {
            st->items[write++] = st->items[i];
        }
    }
    st->count = write;
    snprintf(st->status, sizeof st->status, "cleared done items");
    wada_app_set_status(st->app, st->status);
}

static void cmd_add(void *user) {
    todo_state *st = (todo_state *)user;
    wada_err err;
    wada_err_clear(&err);
    if (add_text(st, st->draft, &err)) {
        st->draft[0] = '\0';
        wada_app_set_status(st->app, "added");
    } else if (err.code != WADA_OK) {
        wada_ui_status_err(st->app, st->status, sizeof st->status, &err);
    }
}

static void cmd_clear(void *user) {
    clear_done((todo_state *)user);
}

static void *todo_enter(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err) {
    todo_state *st = (todo_state *)calloc(1, sizeof *st);
    if (!st) {
        wada_err_set(err, WADA_ERR_NOMEM, "todo");
        return NULL;
    }
    st->app = app;
    const wada_store_ops *ops = wada_ui_store_ops(app, "todo", err);
    if (!ops) {
        free(st);
        return NULL;
    }
    char path[640];
    wada_ui_db_path(data_dir, path, sizeof path);
    if (wada_records_open(&st->rec, ops, path, "todo/", err) != WADA_OK) {
        free(st);
        return NULL;
    }
    todo_load_ctx ctx = {st, false};
    wada_status status = wada_records_each(&st->rec, load_visitor, &ctx, err);
    if (ctx.oom) status = wada_err_set(err, WADA_ERR_NOMEM, "todo load");
    if (status != WADA_OK) {
        wada_records_close(&st->rec);
        free(st->items);
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
    wada_records_close(&st->rec);
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
                            wada_ui_status_err(st->app, st->status, sizeof st->status, &err);
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

static const char *const todo_prefixes[] = {"todo/"};

const wada_product todo_product = {
    "todo",
    "Todo",
    todo_enter,
    todo_leave,
    todo_frame,
    NULL,
    0,
    NULL,
    0,
    todo_prefixes,
    (int)(sizeof todo_prefixes / sizeof todo_prefixes[0]),
};
