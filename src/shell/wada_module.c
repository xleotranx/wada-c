#include "wada_module.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wada_log.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

struct wada_module {
    void *lib;
    wada_plugin plugin;
    char path[720];
};

static bool is_absolute(const char *p) {
    if (!p || !p[0]) return false;
#ifdef _WIN32
    if (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return true;
#endif
    return p[0] == '/' || p[0] == '\\';
}

/* Directory of the running executable, without the trailing separator. */
static bool exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    char buf[720];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof buf);
    if (n == 0 || n >= sizeof buf) return false;
    char *slash = strrchr(buf, '\\');
    if (!slash) slash = strrchr(buf, '/');
    if (!slash) return false;
    *slash = '\0';
    if (strlen(buf) >= cap) return false;
    snprintf(out, cap, "%s", buf);
    return true;
#else
    char buf[720];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return false;
    buf[n] = '\0';
    char *slash = strrchr(buf, '/');
    if (!slash) return false;
    *slash = '\0';
    if (strlen(buf) >= cap) return false;
    snprintf(out, cap, "%s", buf);
    return true;
#endif
}

static void *lib_open(const char *path, char *why, size_t cap) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        char msg[256];
        DWORD code = GetLastError();
        DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                                 0, msg, (DWORD)sizeof msg, NULL);
        while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == ' ')) msg[--n] = '\0';
        snprintf(why, cap, "%s (error %lu)", n ? msg : "load failed", (unsigned long)code);
    }
    return (void *)h;
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) snprintf(why, cap, "%s", dlerror() ? dlerror() : "load failed");
    return h;
#endif
}

static void *lib_symbol(void *lib, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

static void lib_close(void *lib) {
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
}

/* The module's own view of the contract, checked field by field. */
static wada_status validate(const wada_plugin *p, wada_err *err) {
    if (!p) return wada_err_set(err, WADA_ERR_ABI, "module: no plugin description");
    if (p->hdr.version != WADA_PLUGIN_ABI) {
        return wada_err_set(err, WADA_ERR_ABI, "module: plugin ABI %u, host %u", p->hdr.version,
                            WADA_PLUGIN_ABI);
    }
    if (p->hdr.size < sizeof(wada_plugin)) {
        return wada_err_set(err, WADA_ERR_ABI, "module: plugin struct %u bytes, host needs %u",
                            p->hdr.size, (unsigned)sizeof(wada_plugin));
    }
    if (!p->id || !p->id[0]) return wada_err_set(err, WADA_ERR_ABI, "module: no provider id");
    if (p->store_abi != WADA_STORE_ABI) {
        return wada_err_set(err, WADA_ERR_ABI, "module: store ABI %u, host %u", p->store_abi,
                            WADA_STORE_ABI);
    }
    if (!p->store) return wada_err_set(err, WADA_ERR_ABI, "module: no store vtable");
    if (!wada_ops_ok(&p->store->hdr, sizeof(wada_store_ops), WADA_STORE_ABI)) {
        return wada_err_set(err, WADA_ERR_ABI, "module: store vtable %u bytes, ABI %u", p->store->hdr.size,
                            p->store->hdr.version);
    }
    return WADA_OK;
}

/* Paths are tried in order until one loads; the last failure is reported. */
static bool open_any(const char *first, const char *second, char *used, size_t used_cap, void **out,
                     char *why, size_t why_cap) {
    void *lib = lib_open(first, why, why_cap);
    if (lib) {
        snprintf(used, used_cap, "%s", first);
        *out = lib;
        return true;
    }
    if (second && second[0]) {
        lib = lib_open(second, why, why_cap);
        if (lib) {
            snprintf(used, used_cap, "%s", second);
            *out = lib;
            return true;
        }
    }
    return false;
}

wada_module *wada_module_load(const char *path, wada_err *err) {
    if (!path || !path[0]) {
        wada_err_set(err, WADA_ERR_INVALID, "module: empty path");
        return NULL;
    }

    char dir[720];
    char joined[1500];
    bool have_dir = exe_dir(dir, sizeof dir);
    if (have_dir) {
        snprintf(joined, sizeof joined, "%s" WADA_PATH_SEP "%s", dir, path);
    }

    char why[256] = "";
    char used[720] = "";
    void *lib = NULL;
    bool ok;
    if (is_absolute(path)) {
        ok = open_any(path, NULL, used, sizeof used, &lib, why, sizeof why);
    } else if (have_dir) {
        ok = open_any(joined, path, used, sizeof used, &lib, why, sizeof why);
    } else {
        ok = open_any(path, NULL, used, sizeof used, &lib, why, sizeof why);
    }
    if (!ok) {
        wada_err_set(err, WADA_ERR_IO, "module: cannot load %s: %s", path, why);
        return NULL;
    }

    wada_plugin_entry_fn entry = (wada_plugin_entry_fn)lib_symbol(lib, WADA_PLUGIN_SYMBOL);
    if (!entry) {
        lib_close(lib);
        wada_err_set(err, WADA_ERR_NOT_FOUND, "module: %s has no %s", path, WADA_PLUGIN_SYMBOL);
        return NULL;
    }

    wada_plugin plugin;
    memset(&plugin, 0, sizeof plugin);
    plugin.hdr.size = (uint32_t)sizeof plugin;
    plugin.hdr.version = WADA_PLUGIN_ABI;

    wada_err inner;
    wada_err_clear(&inner);
    wada_status st = entry((uint32_t)sizeof plugin, WADA_PLUGIN_ABI, &plugin, &inner);
    if (st != WADA_OK) {
        lib_close(lib);
        wada_err_set(err, st, "module: %s refused the host: %s", path,
                     wada_err_msg(&inner)[0] ? wada_err_msg(&inner) : wada_status_name(st));
        return NULL;
    }
    st = validate(&plugin, err);
    if (st != WADA_OK) {
        char detail[WADA_ERR_MSG_CAP];
        snprintf(detail, sizeof detail, "%s", wada_err_msg(err));
        lib_close(lib);
        wada_err_set(err, st, "%s: %s", path, detail);
        return NULL;
    }

    wada_module *m = (wada_module *)calloc(1, sizeof *m);
    if (!m) {
        lib_close(lib);
        wada_err_set(err, WADA_ERR_NOMEM, "module");
        return NULL;
    }
    m->lib = lib;
    m->plugin = plugin;
    snprintf(m->path, sizeof m->path, "%s", used);
    WADA_LOGI("module", "loaded %s (id %s, plugin abi %u, store abi %u)", used, plugin.id,
              plugin.hdr.version, plugin.store_abi);
    return m;
}

void wada_module_unload(wada_module *m) {
    if (!m) return;
    /* Only at shutdown, after the store is closed and no callback can run. */
    if (m->lib) lib_close(m->lib);
    free(m);
}

const wada_plugin *wada_module_plugin(const wada_module *m) {
    return m ? &m->plugin : NULL;
}

const wada_store_ops *wada_module_store(const wada_module *m) {
    return m ? m->plugin.store : NULL;
}

const char *wada_module_id(const wada_module *m) {
    return m && m->plugin.id ? m->plugin.id : "";
}

const char *wada_module_path(const wada_module *m) {
    return m ? m->path : "";
}
