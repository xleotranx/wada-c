#include "wada_settings_ui.h"

#include <cui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#include "wada_i18n.h"

struct wada_settings_ui {
    wada_app *app;
    wada_settings *settings;
    wada_host *host;
    wada_scope *scope;
    char data_dir[512];
    bool open;
    /* A switch applies at the next frame start: the frame that changed it is
     * drawn in one theme, not half in each. */
    bool theme_pending;
    bool theme_value;
    int lang_pending; /* -1: none */
};

static void cmd_open(void *user) {
    wada_settings_ui *s = (wada_settings_ui *)user;
    s->open = true;
}

static void apply_pending(void *user) {
    wada_settings_ui *s = (wada_settings_ui *)user;
    if (s->theme_pending) {
        s->theme_pending = false;
        wada_app_set_light(s->app, s->theme_value);
        if (s->settings) {
            wada_settings_set_int(s->settings, "ui/theme", s->theme_value ? 1 : 0, NULL);
        }
    }
    if (s->lang_pending >= 0) {
        int lang = s->lang_pending;
        s->lang_pending = -1;
        wada_i18n_set_lang(lang);
        if (s->settings) wada_settings_set(s->settings, "ui/lang", wada_i18n_code(lang), NULL);
        wada_app_set_title(s->app, wada_host_title(s->host));
        wada_app_set_status(s->app, wada_i18n_name(lang));
    }
}

static void schedule(wada_settings_ui *s) {
    if (wada_app_defer(s->app, apply_pending, s) != WADA_OK) apply_pending(s);
}

static void db_path(const wada_settings_ui *s, char *out, size_t cap) {
    snprintf(out, cap, "%s" WADA_PATH_SEP "wada.db", s->data_dir);
}

static void format_size(long long bytes, char *out, size_t cap) {
    if (bytes < 0) {
        snprintf(out, cap, "?");
    } else if (bytes < 1024) {
        snprintf(out, cap, "%lld B", bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    }
}

/* Read through the C library, so the panel has no platform file API. */
static long long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = (long long)ftell(f);
    fclose(f);
    return size;
}

static void reveal_folder(const char *path) {
#ifdef _WIN32
    ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    if (fork() == 0) {
        execlp("open", "open", path, (char *)NULL);
        _exit(1);
    }
#else
    if (fork() == 0) {
        execlp("xdg-open", "xdg-open", path, (char *)NULL);
        _exit(1);
    }
#endif
}

static void section(cui_ctx *ui, const char *label) {
    cui_text(ui, wada_tr(label), .size = 12, .color = CUI_COLOR_TEXT_MUTED);
}

static void draw(wada_app *app, void *user) {
    wada_settings_ui *s = (wada_settings_ui *)user;
    if (!s->open) return;
    cui_ctx *ui = wada_app_ui(app);
    if (!ui) return;

    bool open = s->open;
    bool drawn = cui_dialog_begin(ui, wada_tr("Settings"), &open,
                                  &(cui_dialog_opts){.width = 520,
                                                     .description = wada_tr(
                                                         "Appearance, language, workspace and data.")});
    if (drawn) {
        section(ui, "Appearance");
        bool light = wada_app_is_light(app);
        if (cui_switch(ui, wada_tr("Light theme"), &light)) {
            wada_settings_ui_set_theme(s, light);
        }

        cui_separator(ui);

        section(ui, "Language");
        const char *names[8];
        int lang_count = wada_i18n_count();
        if (lang_count > 8) lang_count = 8;
        for (int i = 0; i < lang_count; i++) names[i] = wada_i18n_name(i);
        int lang = wada_i18n_lang();
        if (cui_select(ui, "settings-lang", names, lang_count, &lang, .width = CUI_FILL) &&
            lang >= 0 && lang < lang_count) {
            wada_settings_ui_set_language(s, wada_i18n_code(lang));
        }

        cui_separator(ui);

        section(ui, "Workspace");
        int count = wada_host_count(s->host);
        const char *titles[16];
        if (count > 16) count = 16;
        int current = -1;
        for (int i = 0; i < count; i++) {
            titles[i] = wada_tr(wada_host_title_at(s->host, i));
            if (strcmp(wada_host_id_at(s->host, i), wada_host_current(s->host)) == 0) current = i;
        }
        int pick = current;
        if (cui_select(ui, "settings-product", titles, count, &pick,
                       .label = wada_tr("Product"), .width = CUI_FILL) &&
            pick >= 0 && pick < count && pick != current) {
            wada_settings_ui_set_product(s, wada_host_id_at(s->host, pick));
        }

        cui_separator(ui);

        section(ui, "Data");
        cui_text(ui, s->data_dir, .size = 11, .color = CUI_COLOR_TEXT_MUTED, .width = CUI_FILL,
                 .nowrap = true);
        char path[640];
        db_path(s, path, sizeof path);
        char size[32];
        format_size(file_size(path), size, sizeof size);
        char line[96];
        wada_trf(line, sizeof line, "database %s", size);
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 10) {
            cui_text(ui, line, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            cui_spacer(ui);
            if (cui_button(ui, wada_tr("Open data folder"), .variant = CUI_BUTTON_OUTLINE)) {
                reveal_folder(s->data_dir);
            }
        }

        char about[96];
        wada_trf(about, sizeof about, "Wada %s · core ABI %u", WADA_VERSION_STRING, WADA_ABI);
        cui_text(ui, about, .size = 11, .color = CUI_COLOR_TEXT_MUTED);

        cui_dialog_footer(ui) {
            if (cui_button(ui, wada_tr("Done"), .variant = CUI_BUTTON_PRIMARY)) open = false;
        }
        cui_dialog_end(ui);
    }
    s->open = open;
}

wada_settings_ui *wada_settings_ui_create(wada_app *app, wada_settings *settings, wada_host *host,
                                          const char *data_dir) {
    if (!app) return NULL;
    wada_settings_ui *s = (wada_settings_ui *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->app = app;
    s->settings = settings;
    s->host = host;
    snprintf(s->data_dir, sizeof s->data_dir, "%s", data_dir ? data_dir : "");
    s->open = false;
    s->lang_pending = -1;

    s->scope = wada_scope_create(wada_app_scope(app), "settings-ui");
    if (!s->scope ||
        wada_app_command(app, s->scope, "app.settings", "Settings…", "Ctrl ,", cmd_open, s) != WADA_OK) {
        if (s->scope) wada_scope_destroy(s->scope);
        free(s);
        return NULL;
    }
    wada_app_set_surface(app, draw, s, &s->open);
    return s;
}

void wada_settings_ui_destroy(wada_settings_ui *s) {
    if (!s) return;
    /* A change queued for the next frame must not be lost if the app closes
     * first: apply it now, then detach. */
    if (s->theme_pending || s->lang_pending >= 0) apply_pending(s);
    wada_app_set_surface(s->app, NULL, NULL, NULL);
    if (s->scope) wada_scope_destroy(s->scope);
    free(s);
}

void wada_settings_ui_open(wada_settings_ui *s) {
    if (s) s->open = true;
}

void wada_settings_ui_close(wada_settings_ui *s) {
    if (s) s->open = false;
}

bool wada_settings_ui_is_open(wada_settings_ui *s) {
    return s && s->open;
}

void wada_settings_ui_set_theme(wada_settings_ui *s, bool light) {
    if (!s) return;
    s->theme_pending = true;
    s->theme_value = light;
    schedule(s);
}

bool wada_settings_ui_set_language(wada_settings_ui *s, const char *code) {
    if (!s || !code) return false;
    int index = -1;
    for (int i = 0; i < wada_i18n_count(); i++) {
        if (strcmp(wada_i18n_code(i), code) == 0) {
            index = i;
            break;
        }
    }
    if (index < 0 || index == wada_i18n_lang()) return index >= 0;
    s->lang_pending = index;
    schedule(s);
    return true;
}

bool wada_settings_ui_set_product(wada_settings_ui *s, const char *id) {
    if (!s || !id) return false;
    return wada_host_switch(s->host, id);
}
