/* Workbench Activity product: cross-product audit log & summary metrics.
 *
 * Reads the "wb.activity" service registered at boot, reloads "act/" records
 * whenever "wb/*" events arrive, and lets the user filter or clear the audit
 * trail.
 */
#include <cui.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wb_store.h"
#include "wada_app.h"
#include "wada_product.h"
#include "wada_ui.h"

#define ACT_MAX_FILTERED 1024
#define ACT_CONFIRM_FRAMES 300

typedef struct act_state {
    wada_app *app;
    wb_store repo;
    const wb_activity_ops *activity_svc;
    bool dirty;
    char filter[64];
    char last_filter[64];
    int filtered[ACT_MAX_FILTERED];
    int filtered_count;
    wada_ui_confirm confirm;
    char status[96];
} act_state;

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

static void on_any_wb(void *user, const char *topic, const void *data, size_t len) {
    (void)topic;
    (void)data;
    (void)len;
    act_state *st = (act_state *)user;
    st->dirty = true;
}

static void reload_now(act_state *st) {
    wada_err err;
    if (wb_store_reload_activity(&st->repo, &err) != WADA_OK) {
        wada_ui_status_err(st->app, st->status, sizeof st->status, &err);
        return;
    }
    st->dirty = true;
}

static void refresh_filter(act_state *st) {
    if (!st->dirty && strcmp(st->filter, st->last_filter) == 0) return;
    if (st->dirty) {
        wada_err err;
        wb_store_reload_activity(&st->repo, &err);
    }
    snprintf(st->last_filter, sizeof st->last_filter, "%s", st->filter);
    st->dirty = false;
    st->filtered_count = 0;

    for (int i = st->repo.activity_count - 1; i >= 0 && st->filtered_count < ACT_MAX_FILTERED; i--) {
        const wb_activity *a = &st->repo.activities[i];
        if (!st->filter[0] || contains_ci(a->topic, st->filter) ||
            contains_ci(a->summary, st->filter)) {
            st->filtered[st->filtered_count++] = i;
        }
    }
}

static void cmd_refresh(void *user) {
    act_state *st = (act_state *)user;
    reload_now(st);
    wada_app_set_status(st->app, "activity refreshed");
}

static void cmd_clear(void *user) {
    act_state *st = (act_state *)user;
    if (!wada_ui_confirm_step(&st->confirm, 0, ACT_CONFIRM_FRAMES)) {
        wada_app_set_status(st->app, "click Clear again to confirm");
        return;
    }
    wada_err err;
    if (wb_activity_clear(&st->repo, &err) != WADA_OK) {
        wada_ui_status_err(st->app, st->status, sizeof st->status, &err);
        return;
    }
    st->dirty = true;
    snprintf(st->status, sizeof st->status, "activity log cleared");
    wada_app_set_status(st->app, st->status);
}

static void act_frame(void *state, wada_app *app) {
    act_state *st = (act_state *)state;
    st->app = app;
    refresh_filter(st);
    wada_ui_confirm_tick(&st->confirm, wada_app_ui(app), 0);

    cui_ctx *ui = wada_app_ui(app);
    wb_counts_info counts = wb_counts(&st->repo, NULL);
    bool confirming = wada_ui_confirm_active(&st->confirm, 0);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad_left = 28, .pad_right = 28,
               .pad_top = 20, .gap = 10) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 12) {
            cui_text(ui, "Activity & Metrics", .font = CUI_FONT_DISPLAY, .size = 24);
            if (cui_button(ui, "Refresh", .icon = CUI_ICON_REFRESH_CW, .variant = CUI_BUTTON_OUTLINE)) {
                cmd_refresh(st);
            }
            if (cui_button(ui, confirming ? "Confirm clear" : "Clear log",
                           .icon = CUI_ICON_TRASH_2,
                           .variant = confirming ? CUI_BUTTON_PRIMARY : CUI_BUTTON_OUTLINE)) {
                cmd_clear(st);
            }
            if (confirming && cui_button(ui, "Keep", .variant = CUI_BUTTON_GHOST)) {
                wada_ui_confirm_reset(&st->confirm);
            }
        }

        cui_row(ui, .width = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED, .pad = 12, .gap = 18) {
            cui_textf(ui, "Projects: %d", counts.projects);
            cui_textf(ui, "Open: %d", counts.open);
            cui_textf(ui, "Doing: %d", counts.doing);
            cui_textf(ui, "Done: %d", counts.done);
            cui_textf(ui, "High priority: %d", counts.high);
            cui_textf(ui, "Stored events: %d", counts.activities);
            if (st->activity_svc) {
                cui_textf(ui, "Session events: %d",
                          st->activity_svc->event_count(st->activity_svc->ctx));
            }
        }

        if (st->activity_svc && st->activity_svc->last_summary(st->activity_svc->ctx)[0]) {
            cui_textf(ui, "Latest: %s", st->activity_svc->last_summary(st->activity_svc->ctx));
        }

        cui_search(ui, "wb-act-filter", st->filter, sizeof st->filter,
                   .placeholder = "Filter audit trail by topic or summary");

        if (st->filtered_count == 0) {
            cui_text(ui, "No activity events recorded yet.", .size = 13, .color = CUI_COLOR_TEXT_MUTED);
        } else {
            int first = 0;
            int last = 0;
            cui_virtual_list_begin(ui, "wb-act-list", st->filtered_count, 54.0f, CUI_FILL,
                                   &first, &last);
            for (int row = first; row < last && row < st->filtered_count; row++) {
                const wb_activity *a = &st->repo.activities[st->filtered[row]];
                char when[32];
                char sub[96];
                wada_ui_format_when(a->when, when, sizeof when);
                snprintf(sub, sizeof sub, "%s · %s", a->topic, when);
                cui_list_item(ui, a->summary[0] ? a->summary : a->topic, .subtitle = sub);
            }
            cui_virtual_list_end(ui);
        }
        cui_text(ui, st->status, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
    }
}

static void *act_enter(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err) {
    act_state *st = (act_state *)calloc(1, sizeof *st);
    if (!st) {
        wada_err_set(err, WADA_ERR_NOMEM, "wb.activity");
        return NULL;
    }
    st->app = app;
    st->dirty = true;
    wada_ui_confirm_init(&st->confirm);

    const wada_store_ops *ops = wada_ui_store_ops(app, "wb.activity", err);
    if (!ops) {
        free(st);
        return NULL;
    }
    char path[640];
    wada_ui_db_path(data_dir, path, sizeof path);
    if (wb_store_open(&st->repo, ops, path, err) != WADA_OK) {
        free(st);
        return NULL;
    }

    const wb_activity_ops *svc = (const wb_activity_ops *)wada_registry_get(
        wada_app_registry(app), "wb.activity", WADA_WB_ACTIVITY_ABI, NULL);
    if (svc && wada_ops_ok(&svc->hdr, sizeof *svc, WADA_WB_ACTIVITY_ABI)) {
        st->activity_svc = svc;
    }

    wada_app_command(app, scope, "wb.activity.refresh", "Refresh activity", "Ctrl R", cmd_refresh, st);
    wada_app_command(app, scope, "wb.activity.clear", "Clear activity log", NULL, cmd_clear, st);
    wada_bus_subscribe_scoped(wada_app_bus(app), scope, "wb/*", 0, on_any_wb, st);
    return st;
}

static void act_leave(void *state) {
    act_state *st = (act_state *)state;
    if (!st) return;
    wb_store_close(&st->repo);
    free(st);
}

static const char *const act_walk[] = {"wb.activity.refresh"};
static const char *const act_prefixes[] = {"act/"};

const wada_product wb_activity_product = {
    "wb.activity",
    "Activity",
    act_enter,
    act_leave,
    act_frame,
    NULL,
    0,
    act_walk,
    (int)(sizeof act_walk / sizeof act_walk[0]),
    act_prefixes,
    (int)(sizeof act_prefixes / sizeof act_prefixes[0]),
};
