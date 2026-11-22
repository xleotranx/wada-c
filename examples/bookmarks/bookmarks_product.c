/* Bookmarks product — an example built on wada_records, wada_ui and wada_bus.
 *
 * Stores bookmarks under "bm/" via wada_records, uses wada_ui for store/path/
 * timestamp/confirm boilerplate, and emits "bm/changed" on the bus so filter
 * caching stays decoupled from mutation sites.
 *
 * Run: build\wada_bookmarks.exe
 */
#include <cui.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wada_app.h"
#include "wada_log.h"
#include "wada_product.h"
#include "wada_records.h"
#include "wada_ui.h"

#define BM_ID_CAP 48
#define BM_TITLE_CAP 128
#define BM_URL_CAP 256
#define BM_TAG_CAP 48
#define BM_MAX_FILTERED 1024
#define BM_CONFIRM_FRAMES 300

typedef struct bookmark {
    char id[BM_ID_CAP];
    long long created;
    char tag[BM_TAG_CAP];
    char title[BM_TITLE_CAP];
    char url[BM_URL_CAP];
} bookmark;

typedef struct bm_state {
    wada_app *app;
    wada_records rec;
    bookmark *items;
    int count;
    int cap;
    int filtered[BM_MAX_FILTERED];
    int filtered_count;
    int selected;
    bool show_add;
    bool dirty_filter;
    int change_count;
    wada_ui_confirm confirm;
    char search[64];
    char last_query[64];
    char draft_title[BM_TITLE_CAP];
    char draft_url[BM_URL_CAP];
    char draft_tag[BM_TAG_CAP];
    char status[96];
} bm_state;

static bool contains_ci(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return true;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

static bool append_item(bm_state *st, const bookmark *item) {
    if (st->count == st->cap) {
        int cap = st->cap ? st->cap * 2 : 16;
        bookmark *v = (bookmark *)realloc(st->items, (size_t)cap * sizeof *v);
        if (!v) return false;
        st->items = v;
        st->cap = cap;
    }
    st->items[st->count++] = *item;
    return true;
}

typedef struct bm_load_ctx {
    bm_state *st;
    bool oom;
} bm_load_ctx;

static bool load_visitor(const char *id, wada_slice value, void *user) {
    bm_load_ctx *ctx = (bm_load_ctx *)user;
    bookmark item;
    memset(&item, 0, sizeof item);
    snprintf(item.id, sizeof item.id, "%s", id);

    const char *p = (const char *)value.p;
    const char *end = p + value.n;
    char line[64];
    if (wada_records_next_line(&p, end, line, sizeof line) && strcmp(line, "v1") == 0) {
        if (wada_records_next_line(&p, end, line, sizeof line)) item.created = atoll(line);
        wada_records_next_line(&p, end, item.tag, sizeof item.tag);
        wada_records_next_line(&p, end, item.title, sizeof item.title);
        wada_records_next_line(&p, end, item.url, sizeof item.url);
    }
    if (!append_item(ctx->st, &item)) {
        ctx->oom = true;
        return false;
    }
    return true;
}

static wada_status write_bookmark(bm_state *st, const bookmark *b, wada_err *err) {
    char buf[BM_TAG_CAP + BM_TITLE_CAP + BM_URL_CAP + 64];
    int len = snprintf(buf, sizeof buf, "v1\n%lld\n%s\n%s\n%s",
                       b->created, b->tag, b->title, b->url);
    if (len < 0 || (size_t)len >= sizeof buf) {
        return wada_err_set(err, WADA_ERR, "encode bookmark");
    }
    return wada_records_put(&st->rec, b->id, buf, (size_t)len, err);
}

static int add_bookmark(bm_state *st, const char *title, const char *url,
                        const char *tag, wada_err *err) {
    if (!title || !title[0] || !url || !url[0]) {
        wada_err_set(err, WADA_ERR_INVALID, "title and URL are required");
        return -1;
    }
    bookmark b;
    memset(&b, 0, sizeof b);
    wada_records_fresh_id(&st->rec, b.id, sizeof b.id);
    b.created = (long long)time(NULL);
    wada_records_sanitize_line(tag, b.tag, sizeof b.tag);
    wada_records_sanitize_line(title, b.title, sizeof b.title);
    wada_records_sanitize_line(url, b.url, sizeof b.url);

    if (write_bookmark(st, &b, err) != WADA_OK) return -1;
    if (!append_item(st, &b)) {
        wada_err_set(err, WADA_ERR_NOMEM, "bookmark add");
        return -1;
    }
    return st->count - 1;
}

static void on_changed(void *user, const char *topic, const void *data, size_t len) {
    (void)topic;
    (void)data;
    (void)len;
    bm_state *st = (bm_state *)user;
    st->dirty_filter = true;
    st->change_count++;
}

static void refresh_filter(bm_state *st) {
    if (!st->dirty_filter && strcmp(st->search, st->last_query) == 0) return;
    snprintf(st->last_query, sizeof st->last_query, "%s", st->search);
    st->dirty_filter = false;
    st->filtered_count = 0;

    for (int i = st->count - 1; i >= 0 && st->filtered_count < BM_MAX_FILTERED; i--) {
        const bookmark *b = &st->items[i];
        if (!st->search[0] || contains_ci(b->title, st->search) ||
            contains_ci(b->url, st->search) || contains_ci(b->tag, st->search)) {
            st->filtered[st->filtered_count++] = i;
        }
    }
}

static void select_bookmark(bm_state *st, int index) {
    st->selected = index;
    st->show_add = false;
    wada_ui_confirm_reset(&st->confirm);
}

static void cmd_new(void *user) {
    bm_state *st = (bm_state *)user;
    st->show_add = true;
    st->draft_title[0] = '\0';
    st->draft_url[0] = '\0';
    st->draft_tag[0] = '\0';
    wada_ui_confirm_reset(&st->confirm);
}

static void submit_add(bm_state *st) {
    wada_err err;
    int index = add_bookmark(st, st->draft_title, st->draft_url, st->draft_tag, &err);
    if (index < 0) {
        wada_ui_status_err(st->app, st->status, sizeof st->status, &err);
        return;
    }
    st->draft_title[0] = '\0';
    st->draft_url[0] = '\0';
    st->draft_tag[0] = '\0';
    select_bookmark(st, index);
    wada_app_emit(st->app, "bm/changed", NULL, 0);
    snprintf(st->status, sizeof st->status, "bookmark added");
    wada_app_set_status(st->app, st->status);
}

static void cmd_delete(void *user) {
    bm_state *st = (bm_state *)user;
    if (st->selected < 0 || st->selected >= st->count) return;
    st->show_add = false;
    if (!wada_ui_confirm_step(&st->confirm, st->selected, BM_CONFIRM_FRAMES)) {
        wada_app_set_status(st->app, "click Delete again to confirm");
        return;
    }
    int old = st->selected;
    wada_err err;
    if (wada_records_del(&st->rec, st->items[old].id, &err) != WADA_OK) {
        wada_ui_status_err(st->app, st->status, sizeof st->status, &err);
        return;
    }
    for (int i = old; i + 1 < st->count; i++) st->items[i] = st->items[i + 1];
    st->count--;
    st->selected = st->count > 0 ? (old < st->count ? old : st->count - 1) : -1;
    wada_app_emit(st->app, "bm/changed", NULL, 0);
    snprintf(st->status, sizeof st->status, "bookmark deleted");
    wada_app_set_status(st->app, st->status);
}

static void cmd_clear_search(void *user) {
    bm_state *st = (bm_state *)user;
    st->search[0] = '\0';
    st->dirty_filter = true;
    wada_app_set_status(st->app, "filter cleared");
}

static void sidebar(bm_state *st) {
    cui_ctx *ui = wada_app_ui(st->app);

    cui_column(ui, .width = CUI_PX(280), .height = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED,
               .pad_left = 10, .pad_right = 10, .pad_top = 14, .gap = 6) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
            cui_text(ui, "Bookmarks", .font = CUI_FONT_DISPLAY, .size = 18);
            cui_textf(ui, "%d", st->count);
        }
        cui_search(ui, "bm-search", st->search, sizeof st->search, .placeholder = "Filter title, URL, tag");

        if (st->filtered_count == 0) {
            cui_text(ui, st->search[0] ? "No matches" : "No bookmarks yet",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        } else {
            int first = 0;
            int last = 0;
            cui_virtual_list_begin(ui, "bm-list", st->filtered_count, 56.0f, CUI_FILL, &first, &last);
            for (int row = first; row < last && row < st->filtered_count; row++) {
                int idx = st->filtered[row];
                const bookmark *b = &st->items[idx];
                char when[32];
                char sub[96];
                wada_ui_format_when(b->created, when, sizeof when);
                if (b->tag[0]) snprintf(sub, sizeof sub, "%s · %s", b->tag, when);
                else snprintf(sub, sizeof sub, "%s", when);
                if (cui_list_item(ui, b->title, .selected = !st->show_add && idx == st->selected,
                                  .subtitle = sub)) {
                    select_bookmark(st, idx);
                }
            }
            cui_virtual_list_end(ui);
        }

        if (cui_button(ui, "Add bookmark", .icon = CUI_ICON_PLUS,
                       .variant = CUI_BUTTON_PRIMARY, .width = CUI_FILL)) {
            cmd_new(st);
        }
    }
}

static void detail(bm_state *st) {
    cui_ctx *ui = wada_app_ui(st->app);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad_left = 28, .pad_right = 28,
               .pad_top = 20, .gap = 10) {
        if (st->show_add) {
            cui_text(ui, "Add a bookmark", .font = CUI_FONT_DISPLAY, .size = 24);
            cui_text(ui, "Stored under bm/ via wada_records; emits bm/changed on the bus.",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            cui_input(ui, "bm-title", st->draft_title, sizeof st->draft_title,
                      .label = "Title", .placeholder = "Page or document title");
            cui_input(ui, "bm-url", st->draft_url, sizeof st->draft_url,
                      .label = "URL", .placeholder = "https://example.org");
            cui_input(ui, "bm-tag", st->draft_tag, sizeof st->draft_tag,
                      .label = "Tag", .placeholder = "docs, reference, reading");
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                if (cui_button(ui, "Save bookmark", .variant = CUI_BUTTON_PRIMARY)) submit_add(st);
                if (cui_button(ui, "Cancel", .variant = CUI_BUTTON_OUTLINE)) st->show_add = false;
            }
        } else if (st->selected < 0 || st->selected >= st->count) {
            cui_text(ui, "No bookmark selected", .font = CUI_FONT_DISPLAY, .size = 22);
            cui_text(ui, "Add a bookmark or pick one from the list.",
                     .size = 13, .color = CUI_COLOR_TEXT_MUTED);
            if (cui_button(ui, "Add bookmark", .variant = CUI_BUTTON_PRIMARY)) cmd_new(st);
        } else {
            const bookmark *b = &st->items[st->selected];
            bool confirming = wada_ui_confirm_active(&st->confirm, st->selected);
            char when[32];
            wada_ui_format_when(b->created, when, sizeof when);

            cui_text(ui, b->title, .font = CUI_FONT_DISPLAY, .size = 24, .width = CUI_FILL);
            cui_text(ui, b->url, .font = CUI_FONT_MONO, .size = 13, .color = CUI_COLOR_ACCENT,
                     .width = CUI_FILL);
            cui_row(ui, .width = CUI_FILL, .gap = 12) {
                if (b->tag[0]) cui_textf(ui, "Tag: %s", b->tag);
                cui_textf(ui, "Saved: %s", when);
                cui_textf(ui, "Bus events: %d", st->change_count);
            }
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
                if (confirming) {
                    cui_text(ui, "Delete this bookmark?", .size = 12, .color = CUI_COLOR_ACCENT);
                }
                if (cui_button(ui, confirming ? "Confirm delete" : "Delete",
                               .variant = confirming ? CUI_BUTTON_PRIMARY : CUI_BUTTON_OUTLINE,
                               .icon = CUI_ICON_TRASH_2)) {
                    cmd_delete(st);
                }
                if (confirming && cui_button(ui, "Keep", .variant = CUI_BUTTON_GHOST)) {
                    wada_ui_confirm_reset(&st->confirm);
                }
            }
        }
        cui_text(ui, st->status, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
    }
}

static void bm_frame(void *state, wada_app *app) {
    bm_state *st = (bm_state *)state;
    st->app = app;
    refresh_filter(st);
    wada_ui_confirm_tick(&st->confirm, wada_app_ui(app), st->selected);

    cui_row(wada_app_ui(app), .width = CUI_FILL, .height = CUI_FILL) {
        sidebar(st);
        detail(st);
    }
}

static const char *const bm_walk[] = {"bm.new", "bm.delete"};

static void *bm_enter(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err) {
    bm_state *st = (bm_state *)calloc(1, sizeof *st);
    if (!st) {
        wada_err_set(err, WADA_ERR_NOMEM, "bookmarks");
        return NULL;
    }
    st->app = app;
    st->selected = -1;
    st->dirty_filter = true;
    wada_ui_confirm_init(&st->confirm);

    const wada_store_ops *ops = wada_ui_store_ops(app, "bookmarks", err);
    if (!ops) {
        free(st);
        return NULL;
    }
    char path[640];
    wada_ui_db_path(data_dir, path, sizeof path);
    if (wada_records_open(&st->rec, ops, path, "bm/", err) != WADA_OK) {
        free(st);
        return NULL;
    }
    bm_load_ctx ctx = {st, false};
    wada_status status = wada_records_each(&st->rec, load_visitor, &ctx, err);
    if (ctx.oom) status = wada_err_set(err, WADA_ERR_NOMEM, "bookmarks load");
    if (status != WADA_OK) {
        wada_records_close(&st->rec);
        free(st->items);
        free(st);
        return NULL;
    }
    if (st->count == 0) {
        add_bookmark(st, "Wada core design record", "docs/CORE.md", "wada", err);
        add_bookmark(st, "SQLite WAL and VACUUM INTO", "https://sqlite.org/wal.html", "sqlite", err);
    }
    if (st->count > 0) st->selected = st->count - 1;

    wada_app_command(app, scope, "bm.new", "Add bookmark", "Ctrl N", cmd_new, st);
    wada_app_command(app, scope, "bm.delete", "Delete bookmark", NULL, cmd_delete, st);
    wada_app_command(app, scope, "bm.clear_search", "Clear filter", "Ctrl L", cmd_clear_search, st);

    wada_sub *sub = wada_bus_subscribe(wada_app_bus(app), "bm/changed", 0, on_changed, st);
    if (sub) wada_scope_own(scope, wada_bus_cancel_owned, sub);
    return st;
}

static void bm_leave(void *state) {
    bm_state *st = (bm_state *)state;
    if (!st) return;
    wada_records_close(&st->rec);
    free(st->items);
    free(st);
}

static const char *const bm_prefixes[] = {"bm/"};

const wada_product bookmarks_product = {
    "bookmarks",
    "Bookmarks",
    bm_enter,
    bm_leave,
    bm_frame,
    NULL,
    0,
    bm_walk,
    (int)(sizeof bm_walk / sizeof bm_walk[0]),
    bm_prefixes,
    (int)(sizeof bm_prefixes / sizeof bm_prefixes[0]),
};
