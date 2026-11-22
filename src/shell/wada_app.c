#include "wada_app.h"

#include <cui.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wada_i18n.h"

typedef struct wada_cmd {
    wada_app *app;
    char *id;
    char *title;
    char *shortcut;
    int mods;
    cui_key key;
    bool has_shortcut;
    wada_command_fn fn;
    void *user;
    bool alive;
} wada_cmd;

typedef struct wada_deferred {
    wada_defer_fn fn;
    void *user;
} wada_deferred;

struct wada_app {
    wada_app_desc desc;
    wada_scope *root;
    wada_bus *bus;
    wada_registry *registry;
    wada_cmd **cmds;
    int cmd_count;
    int cmd_cap;
    wada_deferred *deferred;
    int deferred_count;
    int deferred_cap;
    cui_app *ui;
    cui_ctx *ui_ctx;
    bool light;
    bool palette_open;
    wada_surface_fn surface;
    void *surface_user;
    const bool *surface_open;
    char title[128];
    char status[160];
};

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static bool grow_cmds(wada_app *app) {
    int cap = app->cmd_cap ? app->cmd_cap * 2 : 16;
    wada_cmd **v = (wada_cmd **)realloc(app->cmds, (size_t)cap * sizeof *v);
    if (!v) return false;
    app->cmds = v;
    app->cmd_cap = cap;
    return true;
}

static bool parse_key_name(const char *name, cui_key *out) {
    if (strlen(name) == 1) {
        char c = (char)toupper((unsigned char)name[0]);
        if (c >= 'A' && c <= 'Z') {
            *out = (cui_key)(CUI_KEY_A + (c - 'A'));
            return true;
        }
        if (c >= '0' && c <= '9') {
            *out = (cui_key)(CUI_KEY_0 + (c - '0'));
            return true;
        }
    }
    if (strcmp(name, "SPACE") == 0) { *out = CUI_KEY_SPACE; return true; }
    if (strcmp(name, "ENTER") == 0) { *out = CUI_KEY_ENTER; return true; }
    if (strcmp(name, "ESC") == 0) { *out = CUI_KEY_ESCAPE; return true; }
    if (strcmp(name, ",") == 0) { *out = CUI_KEY_COMMA; return true; }
    if (strcmp(name, ".") == 0) { *out = CUI_KEY_PERIOD; return true; }
    if (strcmp(name, "/") == 0) { *out = CUI_KEY_SLASH; return true; }
    if (strcmp(name, "-") == 0) { *out = CUI_KEY_MINUS; return true; }
    if (strcmp(name, "=") == 0) { *out = CUI_KEY_EQUAL; return true; }
    return false;
}

/* Accepts "Ctrl N", "Ctrl+N", "Shift+Ctrl+S". Modifiers are required: a plain
 * letter must not fire a command while the user types. */
static bool parse_shortcut(const char *text, int *mods, cui_key *key) {
    *mods = 0;
    if (!text || !text[0] || !key) return false;

    char buf[64];
    snprintf(buf, sizeof buf, "%s", text);
    bool have_key = false;
    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '+') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != ' ' && *p != '+') {
            *p = (char)toupper((unsigned char)*p);
            p++;
        }
        if (*p) { *p = '\0'; p++; }
        if (strcmp(start, "CTRL") == 0 || strcmp(start, "CMD") == 0 ||
            strcmp(start, "SUPER") == 0 || strcmp(start, "PRIMARY") == 0) {
            *mods |= CUI_MOD_PRIMARY;
        } else if (strcmp(start, "SHIFT") == 0) {
            *mods |= CUI_MOD_SHIFT;
        } else if (strcmp(start, "ALT") == 0 || strcmp(start, "OPTION") == 0) {
            *mods |= CUI_MOD_ALT;
        } else if (!have_key) {
            have_key = parse_key_name(start, key);
        }
    }
    return have_key && *mods != 0;
}

static void cmd_disable(void *user) {
    wada_cmd *cmd = (wada_cmd *)user;
    if (cmd) cmd->alive = false;
}

static wada_cmd *cmd_find(wada_app *app, const char *id) {
    if (!app || !id) return NULL;
    for (int i = 0; i < app->cmd_count; i++) {
        wada_cmd *cmd = app->cmds[i];
        if (cmd->alive && strcmp(cmd->id, id) == 0) return cmd;
    }
    return NULL;
}

static void command_quit(void *user) {
    wada_app_quit((wada_app *)user);
}

static void command_theme(void *user) {
    wada_app_toggle_theme((wada_app *)user);
}

wada_app *wada_app_create(const wada_app_desc *desc, wada_err *err) {
    wada_app *app = (wada_app *)calloc(1, sizeof *app);
    if (!app) {
        wada_err_set(err, WADA_ERR_NOMEM, "wada_app_create");
        return NULL;
    }
    if (desc) app->desc = *desc;
    app->light = app->desc.light_theme;
    snprintf(app->title, sizeof app->title, "%s", app->desc.title ? app->desc.title : "Wada");
    app->root = wada_scope_create(NULL, "app");
    app->bus = wada_bus_create();
    app->registry = wada_registry_create();
    if (!app->root || !app->bus || !app->registry) {
        wada_app_destroy(app);
        wada_err_set(err, WADA_ERR_NOMEM, "wada_app_create: core objects");
        return NULL;
    }
    snprintf(app->status, sizeof app->status, "%s", wada_tr("ready"));

    wada_app_command(app, app->root, "app.quit", "Quit", "Ctrl Q", command_quit, app);
    wada_app_command(app, app->root, "app.theme", "Toggle theme", "Ctrl T", command_theme, app);
    return app;
}

void wada_app_destroy(wada_app *app) {
    if (!app) return;
    if (app->ui) {
        cui_app_destroy(app->ui);
        app->ui = NULL;
    }
    /* Disposes commands (marks them dead), subscriptions and services. */
    wada_scope_destroy(app->root);
    app->root = NULL;

    for (int i = 0; i < app->cmd_count; i++) {
        free(app->cmds[i]->id);
        free(app->cmds[i]->title);
        free(app->cmds[i]->shortcut);
        free(app->cmds[i]);
    }
    free(app->cmds);
    free(app->deferred);
    wada_bus_destroy(app->bus);
    wada_registry_destroy(app->registry);
    free(app);
}

wada_scope *wada_app_scope(wada_app *app) {
    return app ? app->root : NULL;
}

wada_bus *wada_app_bus(wada_app *app) {
    return app ? app->bus : NULL;
}

wada_registry *wada_app_registry(wada_app *app) {
    return app ? app->registry : NULL;
}

cui_ctx *wada_app_ui(wada_app *app) {
    return app ? app->ui_ctx : NULL;
}

wada_status wada_app_command(wada_app *app, wada_scope *owner, const char *id, const char *title,
                             const char *shortcut, wada_command_fn fn, void *user) {
    if (!app || !id || !id[0] || !fn) return WADA_ERR_INVALID;
    if (cmd_find(app, id)) return WADA_ERR_EXISTS;
    if (app->cmd_count == app->cmd_cap && !grow_cmds(app)) return WADA_ERR_NOMEM;

    wada_cmd *cmd = (wada_cmd *)calloc(1, sizeof *cmd);
    if (!cmd) return WADA_ERR_NOMEM;
    cmd->app = app;
    cmd->id = dup_str(id);
    cmd->title = dup_str(title && title[0] ? title : id);
    cmd->shortcut = dup_str(shortcut);
    cmd->has_shortcut = parse_shortcut(cmd->shortcut, &cmd->mods, &cmd->key);
    cmd->fn = fn;
    cmd->user = user;
    cmd->alive = true;
    if (!cmd->id || !cmd->title) {
        free(cmd->id);
        free(cmd->title);
        free(cmd->shortcut);
        free(cmd);
        return WADA_ERR_NOMEM;
    }
    if (owner && wada_scope_own(owner, cmd_disable, cmd) == 0) {
        free(cmd->id);
        free(cmd->title);
        free(cmd->shortcut);
        free(cmd);
        return WADA_ERR_STATE;
    }
    app->cmds[app->cmd_count++] = cmd;
    return WADA_OK;
}

bool wada_app_invoke(wada_app *app, const char *id) {
    wada_cmd *cmd = cmd_find(app, id);
    if (!cmd) return false;
    cmd->fn(cmd->user);
    return true;
}

int wada_app_command_count(wada_app *app) {
    if (!app) return 0;
    int n = 0;
    for (int i = 0; i < app->cmd_count; i++) {
        if (app->cmds[i]->alive) n++;
    }
    return n;
}

bool wada_app_command_at(wada_app *app, int index, wada_command_info *out) {
    if (!app || !out || index < 0) return false;
    int n = 0;
    for (int i = 0; i < app->cmd_count; i++) {
        wada_cmd *cmd = app->cmds[i];
        if (!cmd->alive) continue;
        if (n++ != index) continue;
        out->id = cmd->id;
        out->title = cmd->title;
        out->shortcut = cmd->shortcut;
        return true;
    }
    return false;
}

wada_status wada_app_defer(wada_app *app, wada_defer_fn fn, void *user) {
    if (!app || !fn) return WADA_ERR_INVALID;
    if (app->deferred_count == app->deferred_cap) {
        int cap = app->deferred_cap ? app->deferred_cap * 2 : 8;
        wada_deferred *v = (wada_deferred *)realloc(app->deferred, (size_t)cap * sizeof *v);
        if (!v) return WADA_ERR_NOMEM;
        app->deferred = v;
        app->deferred_cap = cap;
    }
    app->deferred[app->deferred_count].fn = fn;
    app->deferred[app->deferred_count].user = user;
    app->deferred_count++;
    return WADA_OK;
}

static void drain_deferred(wada_app *app) {
    int i = 0;
    while (i < app->deferred_count) {
        wada_deferred d = app->deferred[i++];
        if (d.fn) d.fn(d.user);
    }
    app->deferred_count = 0;
}

void wada_app_emit(wada_app *app, const char *topic, const void *data, size_t len) {
    if (!app) return;
    wada_bus_publish(app->bus, topic, data, len);
}

void wada_app_set_status(wada_app *app, const char *text) {
    if (!app) return;
    snprintf(app->status, sizeof app->status, "%s", text ? text : "");
    wada_app_emit(app, "app/status", app->status, strlen(app->status) + 1);
}

const char *wada_app_status_text(wada_app *app) {
    return app ? app->status : "";
}

void wada_app_set_title(wada_app *app, const char *title) {
    if (!app) return;
    snprintf(app->title, sizeof app->title, "%s", title && title[0] ? title : "Wada");
    if (app->ui) cui_app_set_title(app->ui, app->title);
}

void wada_app_size(wada_app *app, int *width, int *height) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (!app || !app->ui) return;
    cui_vec2 size = cui_app_size(app->ui);
    if (width) *width = (int)size.x;
    if (height) *height = (int)size.y;
}

bool wada_app_is_light(wada_app *app) {
    return app && app->light;
}

static void apply_theme(wada_app *app) {
    if (app->ui_ctx) cui_set_theme(app->ui_ctx, app->light ? CUI_THEME_LIGHT : CUI_THEME_DARK);
}

void wada_app_set_light(wada_app *app, bool light) {
    if (!app || app->light == light) return;
    app->light = light;
    apply_theme(app);
    wada_app_set_status(app, wada_tr(light ? "light theme" : "dark theme"));
}

void wada_app_toggle_theme(wada_app *app) {
    if (!app) return;
    wada_app_set_light(app, !app->light);
}

void wada_app_set_surface(wada_app *app, wada_surface_fn fn, void *user, const bool *open) {
    if (!app) return;
    app->surface = fn;
    app->surface_user = user;
    app->surface_open = open;
}

bool wada_app_surface_active(wada_app *app) {
    return app && app->surface && app->surface_open && *app->surface_open;
}

void wada_app_quit(wada_app *app) {
    if (!app || !app->ui) return;
    cui_app_quit(app->ui);
}

static void draw_palette(wada_app *app) {
    if (!app->palette_open || !app->ui_ctx) return;

    cui_command items[64];
    wada_cmd *map[64];
    int n = 0;
    for (int i = 0; i < app->cmd_count && n < 64; i++) {
        wada_cmd *cmd = app->cmds[i];
        if (!cmd->alive) continue;
        items[n].title = wada_tr(cmd->title); /* titles stay English; lookup happens at draw */
        items[n].icon = NULL;
        items[n].group = wada_tr("Commands");
        items[n].shortcut = cmd->shortcut;
        map[n] = cmd;
        n++;
    }
    int chosen = cui_command_palette(app->ui_ctx, &app->palette_open, items, n);
    if (chosen >= 0 && chosen < n) wada_app_invoke(app, map[chosen]->id);
}

static void dispatch_shortcuts(wada_app *app, cui_ctx *ui) {
    for (int i = 0; i < app->cmd_count; i++) {
        wada_cmd *cmd = app->cmds[i];
        if (!cmd->alive || !cmd->has_shortcut) continue;
        if (cui_shortcut(ui, cmd->mods, cmd->key)) {
            wada_app_invoke(app, cmd->id);
            return;
        }
    }
}

/* The bundled fonts cover Latin; a Vietnamese (or Cyrillic, Greek…) screen
 * needs a fallback. CUI looks a missing glyph up in the fallback font, so the
 * app keeps its typography and the user gets a complete alphabet. The file is
 * an OS resource: nothing is vendored and nothing is downloaded, and a system
 * without it still runs (with the original coverage). */
static void install_system_fallback(cui_ctx *ui) {
#if defined(_WIN32)
    static const char *const paths[] = {"C:\\Windows\\Fonts\\segoeui.ttf",
                                        "C:\\Windows\\Fonts\\arial.ttf"};
#elif defined(__APPLE__)
    static const char *const paths[] = {"/System/Library/Fonts/Supplemental/Arial.ttf",
                                        "/Library/Fonts/Arial Unicode.ttf"};
#else
    static const char *const paths[] = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                        "/usr/share/fonts/TTF/DejaVuSans.ttf"};
#endif
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        cui_font font = cui_font_load_file(ui, paths[i]);
        if (font < 0) continue;
        for (int role = CUI_FONT_BODY; role <= CUI_FONT_MONO; role++) {
            cui_font_add_fallback(ui, role, font);
        }
        WADA_LOGD("ui", "fallback font: %s", paths[i]);
        return;
    }
    WADA_LOGD("ui", "no system fallback font; text coverage is the bundled set");
}

static void draw_frame(wada_app *app, wada_compose_fn compose, void *user) {
    cui_ctx *ui = app->ui_ctx;
    if (!ui) return;

    bool surface = wada_app_surface_active(app);
    if (!surface) {
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_K)) app->palette_open = !app->palette_open;
        if (!app->palette_open) dispatch_shortcuts(app, ui);
    }

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
        cui_row(ui, .width = CUI_FILL, .height = CUI_FILL) {
            if (compose) compose(app, user);
        }
        cui_status_bar(ui, "wada-status") {
            cui_status_bar_text(ui, app->status[0] ? app->status : wada_tr("ready"));
            cui_status_bar_spacer(ui);
            cui_status_bar_text(ui, wada_tr(app->light ? "light" : "dark"));
            cui_status_bar_text(ui, WADA_VERSION_STRING);
        }
    }

    if (surface && app->surface) app->surface(app, app->surface_user);
    draw_palette(app);
}

static int run_loop(wada_app *app, wada_compose_fn compose, void *user, int max_frames,
                    wada_step_fn step, void *step_user) {
    cui_app_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.title = app->title;
    desc.width = app->desc.width > 0 ? app->desc.width : 1200;
    desc.height = app->desc.height > 0 ? app->desc.height : 800;
    desc.theme = app->light ? CUI_THEME_LIGHT : CUI_THEME_DARK;
    desc.hidden = app->desc.hidden;
    /* A hidden window has no events to wake it: without continuous rendering
     * the frame call waits forever once nothing needs a redraw. Tests and
     * --doctor step frames, so render every step. */
    desc.continuous = app->desc.hidden;

    app->ui = cui_app_create(&desc);
    if (!app->ui) {
        WADA_LOGE("app", "window creation failed");
        return 1;
    }
    install_system_fallback(cui_app_ui(app->ui));

    WADA_LOGI("app", "wada %s started", WADA_VERSION_STRING);
    int frame = 0;
    while (cui_app_frame(app->ui)) {
        app->ui_ctx = cui_app_ui(app->ui);
        drain_deferred(app);
        /* Input fed here is read by this frame's draw: CUI clears per-frame
         * key and mouse state at the end of the previous frame. */
        if (step) step(app, frame, step_user);
        wada_app_emit(app, "app/frame", NULL, 0);
        draw_frame(app, compose, user);
        frame++;
        if (max_frames > 0 && --max_frames == 0) break;
    }
    app->ui_ctx = NULL;
    cui_app_destroy(app->ui);
    app->ui = NULL;
    WADA_LOGI("app", "closed");
    return 0;
}

int wada_app_run(wada_app *app, wada_compose_fn compose, void *user) {
    if (!app) return 1;
    return run_loop(app, compose, user, 0, NULL, NULL);
}

int wada_app_run_frames(wada_app *app, wada_compose_fn compose, void *user, int frames) {
    if (!app || frames <= 0) return 1;
    return run_loop(app, compose, user, frames, NULL, NULL);
}

int wada_app_run_steps(wada_app *app, wada_compose_fn compose, void *user, int frames,
                       wada_step_fn step, void *step_user) {
    if (!app || frames <= 0) return 1;
    return run_loop(app, compose, user, frames, step, step_user);
}
