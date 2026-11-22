#include "wada_ui.h"

#include <cui.h>
#include <stdio.h>
#include <time.h>

void wada_ui_db_path(const char *data_dir, char *out, size_t cap) {
    if (!out || cap == 0) return;
    const char *dir = (data_dir && data_dir[0]) ? data_dir : ".";
    snprintf(out, cap, "%s" WADA_PATH_SEP "wada.db", dir);
}

const wada_store_ops *wada_ui_store_ops(wada_app *app, const char *owner, wada_err *err) {
    if (!app) {
        wada_err_set(err, WADA_ERR_INVALID, "%s: no app", owner ? owner : "app");
        return NULL;
    }
    const wada_store_ops *ops =
        (const wada_store_ops *)wada_registry_get(wada_app_registry(app), "store", WADA_STORE_ABI, NULL);
    if (!ops) {
        wada_err_set(err, WADA_ERR_NOT_FOUND, "%s: no store service", owner ? owner : "app");
    }
    return ops;
}

void wada_ui_format_when(long long unix_sec, char *out, size_t cap) {
    if (!out || cap == 0) return;
    time_t t = (time_t)unix_sec;
    struct tm *tm = localtime(&t);
    if (!tm) {
        out[0] = '\0';
        return;
    }
    strftime(out, cap, "%Y-%m-%d %H:%M", tm);
}

void wada_ui_status_err(wada_app *app, char *buf, size_t cap, const wada_err *err) {
    const char *msg = wada_err_msg(err);
    if (buf && cap > 0) snprintf(buf, cap, "%s", msg);
    if (app) wada_app_set_status(app, msg);
}

void wada_ui_confirm_init(wada_ui_confirm *c) {
    wada_ui_confirm_reset(c);
}

void wada_ui_confirm_reset(wada_ui_confirm *c) {
    if (!c) return;
    c->target = -1;
    c->frames = 0;
}

bool wada_ui_confirm_active(const wada_ui_confirm *c, int index) {
    return c && index >= 0 && c->target == index;
}

bool wada_ui_confirm_step(wada_ui_confirm *c, int index, int timeout_frames) {
    if (!c || index < 0) return false;
    if (c->target == index) {
        wada_ui_confirm_reset(c);
        return true;
    }
    c->target = index;
    c->frames = timeout_frames > 0 ? timeout_frames : 300;
    return false;
}

void wada_ui_confirm_tick(wada_ui_confirm *c, cui_ctx *ui, int current_index) {
    if (!c || c->target < 0) return;
    if ((ui && cui_key_pressed(ui, CUI_KEY_ESCAPE)) || c->target != current_index ||
        c->frames <= 0) {
        wada_ui_confirm_reset(c);
        return;
    }
    c->frames--;
}
