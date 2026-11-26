#include "wada_boot.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "wada_app.h"
#include "wada_i18n.h"
#include "wada_instance.h"
#include "wada_log.h"
#include "wada_module.h"
#include "wada_settings.h"
#include "wada_settings_ui.h"

static int g_width;
static int g_height;

/* A window-subsystem build has no console; log lines go to a file as well. */
static void log_file_sink(void *user, wada_log_level level, const char *line) {
    (void)level;
    FILE *f = (FILE *)user;
    time_t now = time(NULL);
    char stamp[32];
    struct tm *tm = localtime(&now);
    if (tm) strftime(stamp, sizeof stamp, "%H:%M:%S", tm);
    else stamp[0] = '\0';
    fprintf(f, "%s %s\n", stamp, line);
    fflush(f);
}

/* The window app has no console; --doctor and early errors still need one. */
static void attach_console(void) {
#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif
}

static bool ensure_dir(const char *path) {
#ifdef _WIN32
    if (_mkdir(path) == 0 || errno == EEXIST) return true;
#else
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return true;
#endif
    return false;
}

static void resolve_data_dir(const wada_boot_desc *desc, char *out, size_t cap) {
    const char *env = getenv("WADA_DATA_DIR");
    if (env && env[0]) {
        snprintf(out, cap, "%s", env);
        return;
    }
    const char *name = desc && desc->data_subdir && desc->data_subdir[0] ? desc->data_subdir : "Wada";
    const char *local = getenv("LOCALAPPDATA");
    if (local && local[0]) {
        snprintf(out, cap, "%s" WADA_PATH_SEP "%s", local, name);
        return;
    }
    if (desc && desc->data_subdir && desc->data_subdir[0]) {
        snprintf(out, cap, ".wada-%s", name);
    } else {
        snprintf(out, cap, ".wada");
    }
}

static void compose(wada_app *app, void *user) {
    wada_host_frame((wada_host *)user);
    wada_app_size(app, &g_width, &g_height);
}

static int count_prefix(const wada_store_ops *ops, wada_store *db, const char *prefix) {
    wada_err err;
    wada_err_clear(&err);
    wada_txn *txn = NULL;
    if (ops->begin(db, WADA_STORE_READ, &txn, &err) != WADA_OK) return -1;

    wada_iter *it = NULL;
    wada_slice p = {(const uint8_t *)prefix, strlen(prefix)};
    int n = 0;
    if (ops->list(txn, p, &it, &err) == WADA_OK) {
        wada_slice key, value;
        while (ops->next(it, &key, &value, &err) == WADA_OK) n++;
        ops->iter_close(it);
    }
    ops->commit(txn, &err);
    return n;
}

static void print_provider(FILE *out, const wada_module *module) {
    if (module) {
        const wada_plugin *p = wada_module_plugin(module);
        fprintf(out, "provider: module %s (id %s, plugin abi %u, store abi %u)\n",
                wada_module_path(module), wada_module_id(module), p->hdr.version, p->store_abi);
    } else {
        fprintf(out, "provider: static (linked into the executable)\n");
    }
}

static int doctor(FILE *out, const wada_boot_desc *desc, const char *db_path, const char *log_path,
                  const wada_module *module, const wada_store_ops *ops, wada_settings *settings) {
    fprintf(out, "wada %s (core ABI %u)\n", WADA_VERSION_STRING, WADA_ABI);
    fprintf(out, "database: %s\n", db_path);
    fprintf(out, "log: %s\n", log_path);
    print_provider(out, module);
    fprintf(out, "store: abi %u, ops size %u, caps 0x%x (search: %s)\n", ops->hdr.version,
            ops->hdr.size, ops->caps(NULL),
            (ops->caps(NULL) & WADA_STORE_CAP_SEARCH) ? "yes" : "no");
    fprintf(out, "language: %s (%s)\n", wada_i18n_code(wada_i18n_lang()),
            wada_i18n_name(wada_i18n_lang()));

    char value[64];
    wada_status st = wada_settings_get(settings, "app/product", value, sizeof value, NULL);
    fprintf(out, "settings: app/product=%s\n", st == WADA_OK ? value : "(unset)");
    st = wada_settings_get(settings, "ui/lang", value, sizeof value, NULL);
    fprintf(out, "settings: ui/lang=%s\n", st == WADA_OK ? value : "(unset)");

    wada_err err;
    wada_err_clear(&err);
    wada_store *db = NULL;
    st = ops->open(db_path, &db, &err);
    if (st != WADA_OK) {
        fprintf(out, "store open: FAIL (%s)\n", wada_err_msg(&err));
        return 1;
    }
    fprintf(out, "store open: ok, schema %u\n", ops->schema_version ? ops->schema_version(db) : 0);
    const char *backup = ops->last_backup ? ops->last_backup(db) : "";
    fprintf(out, "backup: %s\n", backup[0] ? backup : "none");

    wada_txn *txn = NULL;
    st = ops->begin(db, WADA_STORE_WRITE, &txn, &err);
    if (st == WADA_OK) {
        wada_slice key = {(const uint8_t *)"health/probe", 12};
        wada_slice val = {(const uint8_t *)"1", 1};
        st = ops->put(txn, key, val, &err);
        if (st == WADA_OK) st = ops->commit(txn, &err);
        else ops->rollback(txn);
    }
    if (st != WADA_OK) {
        fprintf(out, "store write: FAIL (%s)\n", wada_err_msg(&err));
        ops->close(db);
        return 1;
    }
    fprintf(out, "store write: ok\n");

    st = ops->begin(db, WADA_STORE_READ, &txn, &err);
    wada_slice read;
    wada_slice key = {(const uint8_t *)"health/probe", 12};
    st = (st == WADA_OK) ? ops->get(txn, key, &read, &err) : st;
    bool read_ok = st == WADA_OK && read.n == 1 && read.p[0] == '1';
    if (txn) ops->rollback(txn);
    fprintf(out, "store read: %s\n", read_ok ? "ok" : "FAIL");

    st = ops->begin(db, WADA_STORE_WRITE, &txn, &err);
    if (st == WADA_OK) {
        st = ops->del(txn, key, &err);
        if (st == WADA_OK) st = ops->commit(txn, &err);
        else ops->rollback(txn);
    }
    fprintf(out, "store delete: %s\n", st == WADA_OK ? "ok" : "FAIL");

    fprintf(out, "counts:");
    {
        const char *seen[64];
        int nseen = 0;
        seen[nseen++] = "settings/";
        for (int i = 0; desc && i < desc->product_count; i++) {
            const wada_product *p = desc->products[i];
            if (!p || !p->prefixes) continue;
            for (int j = 0; j < p->prefix_count && nseen < 64; j++) {
                const char *pfx = p->prefixes[j];
                if (!pfx || !pfx[0]) continue;
                bool dup = false;
                for (int k = 0; k < nseen; k++) {
                    if (strcmp(seen[k], pfx) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) seen[nseen++] = pfx;
            }
        }
        for (int i = 0; i < nseen; i++) {
            int n = count_prefix(ops, db, seen[i]);
            fprintf(out, " %s=%d", seen[i], n);
        }
        fprintf(out, "\n");
    }
    ops->close(db);
    return read_ok && st == WADA_OK ? 0 : 1;
}

typedef struct doctor_ui {
    FILE *out;
    wada_host *host;
    const wada_boot_desc *desc;
    const char *last;
    bool panel_round; /* second pass: draw the settings panel, do not switch */
    int walk_pos;
    int screens;
} doctor_ui;

static void doctor_compose(wada_app *app, void *user) {
    doctor_ui *d = (doctor_ui *)user;
    wada_host_frame(d->host);
    if (d->panel_round) return;

    const char *current = wada_host_current(d->host);
    if (!current[0]) {
        wada_app_quit(app);
        return;
    }

    int index = -1;
    for (int i = 0; i < d->desc->product_count; i++) {
        if (strcmp(d->desc->products[i]->id, current) == 0) {
            index = i;
            break;
        }
    }

    if (!d->last || strcmp(d->last, current) != 0) {
        int count = wada_app_command_count(app);
        fprintf(d->out, "commands[%s] (%d):", current, count);
        for (int i = 0; i < count; i++) {
            wada_command_info info;
            if (!wada_app_command_at(app, i, &info)) continue;
            fprintf(d->out, " %s", info.id);
            if (info.shortcut && info.shortcut[0]) fprintf(d->out, "(%s)", info.shortcut);
        }
        fprintf(d->out, "\n");
        d->last = current;
        d->walk_pos = 0;
    }

    /* Each product names the commands that reveal the screens its first frame
     * does not draw. One command per frame, so each state gets rendered. */
    if (index >= 0) {
        const wada_product *product = d->desc->products[index];
        if (product->walk && d->walk_pos < product->walk_count) {
            if (wada_app_invoke(app, product->walk[d->walk_pos])) {
                d->walk_pos++;
                d->screens++;
                return;
            }
            d->walk_pos = product->walk_count; /* unknown id: stop this product's walk */
        }
    }

    if (index >= 0 && index + 1 < d->desc->product_count) {
        wada_host_switch(d->host, d->desc->products[index + 1]->id);
    } else {
        wada_app_quit(app);
    }
}

/* Boots the products hidden to list their commands and to see which strings a
 * translated run actually draws. Data goes to a throwaway directory so
 * --doctor never touches the real database. */
static int doctor_commands(FILE *out, const wada_boot_desc *desc, const wada_store_ops *store,
                           wada_settings *settings, const char *base_dir) {
    char dir[700];
    snprintf(dir, sizeof dir, "%s" WADA_PATH_SEP "doctor-walk", base_dir);
    ensure_dir(dir);

    wada_err err;
    wada_err_clear(&err);
    wada_app_desc app_desc;
    memset(&app_desc, 0, sizeof app_desc);
    app_desc.title = desc->title;
    app_desc.width = 400;
    app_desc.height = 300;
    app_desc.hidden = true;

    wada_i18n_misses_clear();

    wada_app *app = wada_app_create(&app_desc, &err);
    if (!app) {
        fprintf(out, "commands: FAIL (%s)\n", wada_err_msg(&err));
        return 1;
    }
    if (wada_registry_add(wada_app_registry(app), wada_app_scope(app), "store", WADA_STORE_ABI,
                          store) != WADA_OK) {
        fprintf(out, "commands: FAIL (store service)\n");
        wada_app_destroy(app);
        return 1;
    }
    if (desc->setup && desc->setup(app, wada_app_scope(app), dir, &err) != WADA_OK) {
        fprintf(out, "setup: FAIL (%s)\n", wada_err_msg(&err));
        wada_app_destroy(app);
        return 1;
    }
    wada_host *host = wada_host_create(app, NULL, dir, desc->products, desc->product_count, &err);
    if (!host) {
        fprintf(out, "commands: FAIL (%s)\n", wada_err_msg(&err));
        wada_app_destroy(app);
        return 1;
    }

    wada_host_switch(host, desc->products[0]->id);
    wada_settings_ui *panel = wada_settings_ui_create(app, settings, host, dir);
    doctor_ui state;
    state.out = out;
    state.host = host;
    state.desc = desc;
    state.last = NULL;
    state.panel_round = false;
    state.walk_pos = 0;
    state.screens = desc->product_count;
    int budget = 4 + desc->product_count * 2;
    for (int i = 0; i < desc->product_count; i++) budget += desc->products[i]->walk_count;
    wada_app_run_frames(app, doctor_compose, &state, budget);

    /* Second pass: the settings panel is a shell surface, so it is not part of
     * any product's screen. Draw it once so its strings count as translated
     * (or not) too. */
    if (panel) {
        wada_settings_ui_open(panel);
        state.panel_round = true;
        wada_app_run_frames(app, doctor_compose, &state, 2);
        state.screens++;
    }

    int misses = wada_i18n_miss_count();
    fprintf(out, "untranslated: %d (screens drawn: %d)", misses, state.screens);
    for (int i = 0; i < misses && i < 8; i++) {
        fprintf(out, " \"%s\"", wada_i18n_miss_at(i));
    }
    fprintf(out, "\n");

    wada_settings_ui_destroy(panel);
    wada_host_destroy(host);
    wada_app_destroy(app);

    static const char *suffixes[] = {"wada.db", "wada.db-wal", "wada.db-shm",
                                     "backups" WADA_PATH_SEP "wada-daily.db"};
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        char path[800];
        snprintf(path, sizeof path, "%s" WADA_PATH_SEP "%s", dir, suffixes[i]);
        remove(path);
    }
    char backup_dir[820];
    snprintf(backup_dir, sizeof backup_dir, "%s" WADA_PATH_SEP "backups", dir);
#ifdef _WIN32
    _rmdir(backup_dir);
#else
    rmdir(backup_dir);
#endif
    return 0;
}

/* --provider beats WADA_STORE_MODULE beats the statically linked provider. */
static const wada_store_ops *resolve_store(const wada_boot_desc *desc, const char *module_path,
                                           wada_module **out_module, wada_err *err) {
    *out_module = NULL;
    if (module_path && module_path[0]) {
        wada_module *m = wada_module_load(module_path, err);
        if (!m) return NULL;
        *out_module = m;
        return wada_module_store(m);
    }
    if (desc->store) return desc->store;
    wada_err_set(err, WADA_ERR_NOT_FOUND, "no store provider: link one or pass --provider <path>");
    return NULL;
}

static void apply_language(const wada_boot_desc *desc, wada_settings *settings, const char *arg) {
    (void)desc;
    const char *code = arg && arg[0] ? arg : NULL;
    if (!code) {
        const char *env = getenv("WADA_LANG");
        if (env && env[0]) code = env;
    }
    char saved[16];
    if (!code) {
        if (wada_settings_get(settings, "ui/lang", saved, sizeof saved, NULL) == WADA_OK && saved[0]) {
            code = saved;
        }
    }
    if (code && !wada_i18n_set_lang_code(code)) {
        WADA_LOGW("i18n", "unknown language '%s', keeping English", code);
    }
}

int wada_boot_run(const wada_boot_desc *desc, int argc, char **argv) {
    if (!desc || !desc->products || desc->product_count <= 0) return 1;

    const char *requested = NULL;
    const char *provider_path = NULL;
    const char *lang_arg = NULL;
    bool run_doctor = false;
    bool open_settings = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--doctor") == 0) {
            run_doctor = true;
        } else if (strcmp(argv[i], "--settings") == 0) {
            open_settings = true;
        } else if (strcmp(argv[i], "--app") == 0 && i + 1 < argc) {
            requested = argv[++i];
        } else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            provider_path = argv[++i];
        } else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            lang_arg = argv[++i];
        }
    }
    if (!provider_path) {
        const char *env = getenv("WADA_STORE_MODULE");
        if (env && env[0]) provider_path = env;
    }

    wada_log_set_level(WADA_LOG_DEBUG);
    attach_console();
    wada_i18n_init();

    char data_dir[512];
    resolve_data_dir(desc, data_dir, sizeof data_dir);
    if (!ensure_dir(data_dir)) {
        fprintf(stderr, "wada: cannot create data directory: %s\n", data_dir);
        return 1;
    }
    char db_path[640];
    snprintf(db_path, sizeof db_path, "%s" WADA_PATH_SEP "wada.db", data_dir);
    char log_path[700];
    snprintf(log_path, sizeof log_path, "%s" WADA_PATH_SEP "log.txt", data_dir);
    FILE *log_file = fopen(log_path, "a");
    if (log_file) wada_log_set_sink(log_file_sink, log_file);
    WADA_LOGI("app", "data: %s", data_dir);
    WADA_LOGI("app", "log: %s", log_path);

    wada_err err;
    wada_err_clear(&err);
    wada_module *module = NULL;
    const wada_store_ops *store = resolve_store(desc, provider_path, &module, &err);
    if (!store) {
        /* The window build has no console: the log file carries the reason. */
        WADA_LOGE("app", "provider: %s", wada_err_msg(&err));
        fprintf(stderr, "wada: %s\n", wada_err_msg(&err));
        return 1;
    }

    wada_settings *settings = wada_settings_open(store, db_path, &err);
    if (!settings) {
        WADA_LOGE("app", "settings: %s", wada_err_msg(&err));
        fprintf(stderr, "wada: cannot open settings: %s\n", wada_err_msg(&err));
        wada_module_unload(module);
        return 1;
    }
    apply_language(desc, settings, lang_arg);

    if (run_doctor) {
        /* A window-subsystem build has no usable stdout; write a report file. */
        char report[700];
        snprintf(report, sizeof report, "%s" WADA_PATH_SEP "doctor.txt", data_dir);
        FILE *f = fopen(report, "w");
        int rc = f ? doctor(f, desc, db_path, log_path, module, store, settings) : 1;
        if (rc == 0 && f) rc = doctor_commands(f, desc, store, settings, data_dir);
        if (f) fclose(f);
        fprintf(stdout, "doctor report: %s\n", report);
        wada_settings_close(settings);
        wada_module_unload(module);
        return rc;
    }

    /* One process per data directory: a second window would show stale lists
     * and interleave settings writes. --doctor is exempt: it is short-lived
     * and must be able to probe while the app runs. */
    wada_instance *instance = wada_instance_acquire(data_dir, &err);
    if (!instance) {
        WADA_LOGE("app", "instance: %s", wada_err_msg(&err));
        fprintf(stderr, "wada: %s\n", wada_err_msg(&err));
        wada_settings_close(settings);
        wada_module_unload(module);
        return 2;
    }

    wada_app_desc app_desc;
    app_desc.title = desc->title;
    app_desc.width = wada_settings_int(settings, "ui/width", desc->width > 0 ? desc->width : 1180);
    app_desc.height = wada_settings_int(settings, "ui/height", desc->height > 0 ? desc->height : 780);
    app_desc.light_theme =
        wada_settings_int(settings, "ui/theme", desc->light_theme ? 1 : 0) != 0;

    wada_app *app = wada_app_create(&app_desc, &err);
    if (!app) {
        fprintf(stderr, "wada: %s\n", wada_err_msg(&err));
        wada_instance_release(instance);
        wada_settings_close(settings);
        wada_module_unload(module);
        return 1;
    }

    if (wada_registry_add(wada_app_registry(app), wada_app_scope(app), "store", WADA_STORE_ABI,
                          store) != WADA_OK) {
        fprintf(stderr, "wada: could not publish the store service\n");
        wada_app_destroy(app);
        wada_instance_release(instance);
        wada_settings_close(settings);
        wada_module_unload(module);
        return 1;
    }
    if (desc->setup && desc->setup(app, wada_app_scope(app), data_dir, &err) != WADA_OK) {
        WADA_LOGE("app", "setup: %s", wada_err_msg(&err));
        fprintf(stderr, "wada: %s\n", wada_err_msg(&err));
        wada_app_destroy(app);
        wada_instance_release(instance);
        wada_settings_close(settings);
        wada_module_unload(module);
        return 1;
    }

    wada_host *host = wada_host_create(app, settings, data_dir, desc->products,
                                       desc->product_count, &err);
    if (!host) {
        fprintf(stderr, "wada: %s\n", wada_err_msg(&err));
        wada_app_destroy(app);
        wada_instance_release(instance);
        wada_settings_close(settings);
        wada_module_unload(module);
        return 1;
    }

    wada_settings_ui *settings_ui = wada_settings_ui_create(app, settings, host, data_dir);
    if (!settings_ui) WADA_LOGW("app", "settings panel unavailable");

    char saved[32];
    saved[0] = '\0';
    if (wada_settings_get(settings, "app/product", saved, sizeof saved, NULL) != WADA_OK) {
        saved[0] = '\0';
    }
    const char *fallback = desc->default_product ? desc->default_product : desc->products[0]->id;
    const char *initial = requested ? requested : (saved[0] ? saved : fallback);
    if (!wada_host_switch(host, initial)) wada_host_switch(host, fallback);
    if (open_settings) wada_settings_ui_open(settings_ui);

    int rc = wada_app_run(app, compose, host);

    if (g_width > 0 && g_height > 0) {
        wada_settings_set_int(settings, "ui/width", g_width, NULL);
        wada_settings_set_int(settings, "ui/height", g_height, NULL);
    }
    wada_settings_set_int(settings, "ui/theme", wada_app_is_light(app) ? 1 : 0, NULL);
    wada_settings_set(settings, "ui/lang", wada_i18n_code(wada_i18n_lang()), NULL);
    const char *current = wada_host_current(host);
    if (current[0]) wada_settings_set(settings, "app/product", current, NULL);

    wada_settings_ui_destroy(settings_ui);
    wada_host_destroy(host);
    wada_app_destroy(app);
    wada_instance_release(instance);
    wada_settings_close(settings);
    wada_module_unload(module);
    return rc;
}
