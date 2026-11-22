#include "wada_i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wada_log.h"

#define MISS_CAP 64
#define MISS_LEN 96
#define LANG_CAP 4

typedef struct i18n_table {
    const wada_i18n_pair *pairs;
    int count;
} i18n_table;

static const char *const g_codes[LANG_CAP] = {"en", "vi", NULL, NULL};
static const char *const g_names[LANG_CAP] = {"English", "Tiếng Việt", NULL, NULL};

static i18n_table **g_tables;
static int g_table_count;
static int g_table_cap;
static int g_lang;
static bool g_inited;

static char g_misses[MISS_CAP][MISS_LEN];
static int g_miss_count;

/* The shell's own strings. A product registers its table with its scope. */
static const wada_i18n_pair g_shell[] = {
    {"Quit", "Thoát"},
    {"Toggle theme", "Đổi giao diện"},
    {"Settings", "Cài đặt"},
    {"Settings…", "Cài đặt…"},
    {"Commands", "Lệnh"},
    {"ready", "sẵn sàng"},
    {"light", "sáng"},
    {"dark", "tối"},
    {"light theme", "giao diện sáng"},
    {"dark theme", "giao diện tối"},
    {"Appearance", "Hiển thị"},
    {"Light theme", "Giao diện sáng"},
    {"Language", "Ngôn ngữ"},
    {"Workspace", "Không gian làm việc"},
    {"Product", "Sản phẩm"},
    {"Data", "Dữ liệu"},
    {"Open data folder", "Mở thư mục dữ liệu"},
    {"Done", "Xong"},
    {"Appearance, language, workspace and data.",
     "Hiển thị, ngôn ngữ, không gian làm việc và dữ liệu."},
    {"Wada %s · core ABI %u", "Wada %s · lõi ABI %u"},
    {"database %s", "cơ sở dữ liệu %s"},
    {"Copied", "Đã sao chép"},
    {"Store provider", "Nhà cung cấp lưu trữ"},
    {"No translation yet", "Chưa có bản dịch"},
};

static void table_dispose(void *user) {
    i18n_table *t = (i18n_table *)user;
    if (!t) return;
    for (int i = 0; i < g_table_count; i++) {
        if (g_tables[i] == t) {
            g_tables[i] = g_tables[g_table_count - 1];
            g_table_count--;
            break;
        }
    }
    free(t);
}

static wada_status add_table(wada_scope *owner, const wada_i18n_pair *pairs, int count) {
    if (!pairs || count <= 0) return WADA_ERR_INVALID;
    if (g_table_count == g_table_cap) {
        int cap = g_table_cap ? g_table_cap * 2 : 8;
        i18n_table **v = (i18n_table **)realloc(g_tables, (size_t)cap * sizeof *v);
        if (!v) return WADA_ERR_NOMEM;
        g_tables = v;
        g_table_cap = cap;
    }
    i18n_table *t = (i18n_table *)calloc(1, sizeof *t);
    if (!t) return WADA_ERR_NOMEM;
    t->pairs = pairs;
    t->count = count;
    if (owner && wada_scope_own(owner, table_dispose, t) == 0) {
        free(t);
        return WADA_ERR_STATE;
    }
    g_tables[g_table_count++] = t;
    return WADA_OK;
}

void wada_i18n_init(void) {
    if (g_inited) return;
    g_inited = true;
    if (add_table(NULL, g_shell, (int)(sizeof g_shell / sizeof g_shell[0])) != WADA_OK) {
        WADA_LOGW("i18n", "shell table registration failed");
    }
}

wada_status wada_i18n_register(wada_scope *owner, const wada_i18n_pair *pairs, int count) {
    return add_table(owner, pairs, count);
}

int wada_i18n_count(void) {
    int n = 0;
    while (n < LANG_CAP && g_codes[n]) n++;
    return n;
}

const char *wada_i18n_code(int index) {
    if (index < 0 || index >= wada_i18n_count()) return "";
    return g_codes[index];
}

const char *wada_i18n_name(int index) {
    if (index < 0 || index >= wada_i18n_count()) return "";
    return g_names[index];
}

int wada_i18n_lang(void) {
    return g_lang;
}

void wada_i18n_set_lang(int index) {
    int count = wada_i18n_count();
    if (index < 0 || index >= count || index == g_lang) return;
    g_lang = index;
    g_miss_count = 0;
}

bool wada_i18n_set_lang_code(const char *code) {
    if (!code || !code[0]) return false;
    int count = wada_i18n_count();
    for (int i = 0; i < count; i++) {
        if (strcmp(g_codes[i], code) == 0) {
            wada_i18n_set_lang(i);
            return true;
        }
    }
    return false;
}

/* Small guard against recording things that are not UI text (versions, ids). */
static bool looks_like_text(const char *s, size_t n) {
    if (n == 0 || n >= MISS_LEN) return false;
    bool letter = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalpha(c)) {
            letter = true;
            break;
        }
    }
    return letter;
}

static void record_miss(const char *en) {
    if (!en) return;
    size_t n = strlen(en);
    if (!looks_like_text(en, n)) return;
    for (int i = 0; i < g_miss_count; i++) {
        if (strcmp(g_misses[i], en) == 0) return;
    }
    if (g_miss_count >= MISS_CAP) return;
    snprintf(g_misses[g_miss_count++], MISS_LEN, "%s", en);
}

const char *wada_tr(const char *en) {
    if (!en || !en[0] || g_lang == 0) return en ? en : "";
    if (g_lang == 1) {
        /* Newest table first: a product may override a shell string. */
        for (int t = g_table_count - 1; t >= 0; t--) {
            const i18n_table *tab = g_tables[t];
            for (int i = 0; i < tab->count; i++) {
                if (strcmp(tab->pairs[i].en, en) == 0) {
                    const char *vi = tab->pairs[i].vi;
                    if (vi && vi[0]) return vi;
                    record_miss(en);
                    return en;
                }
            }
        }
    }
    record_miss(en);
    return en;
}

void wada_trf(char *out, size_t cap, const char *en_fmt, ...) {
    if (!out || cap == 0) return;
    const char *fmt = wada_tr(en_fmt);
    va_list ap;
    va_start(ap, en_fmt);
    vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}

int wada_i18n_miss_count(void) {
    return g_miss_count;
}

const char *wada_i18n_miss_at(int index) {
    if (index < 0 || index >= g_miss_count) return "";
    return g_misses[index];
}

void wada_i18n_misses_clear(void) {
    g_miss_count = 0;
}
