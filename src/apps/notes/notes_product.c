/* Notes product: list, search, editor, autosave, delete, export/import.
 *
 * The list holds metadata only; a body is loaded when a note is selected and
 * grows on demand, so a pasted long note is not silently truncated at a fixed
 * limit. Delete asks for a second click. Export writes a length-prefixed
 * backup that survives newlines and round-trips through Import.
 */
#include <cui.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "notes.h"
#include "wada_app.h"
#include "wada_log.h"
#include "wada_product.h"

#define MAX_FILTERED 1024
#define AUTOSAVE_FRAMES 20
#define ROW_HEIGHT 56.0f
#define BODY_MIN_CAP 16384u
#define BODY_MAX_CAP (4u * 1024u * 1024u)
#define CONFIRM_FRAMES 300

typedef struct notes_state {
    wada_app *app;
    notes repo;
    int selected;
    char search[64];
    char last_query[64];
    int data_version;
    int last_version;
    char title[NOTE_TITLE_CAP];
    char *body;
    size_t body_cap;
    char *base_body;
    size_t base_cap;
    int filtered[MAX_FILTERED];
    int filtered_count;
    bool dirty;
    int idle;
    int confirm_delete; /* index awaiting a second click, or -1 */
    int confirm_frames;
    char status[128];
} notes_state;

static bool grow_buffer(char **buf, size_t *cap, size_t need) {
    if (*cap >= need) return true;
    size_t next = *cap ? *cap : BODY_MIN_CAP;
    while (next < need) next *= 2;
    if (next > BODY_MAX_CAP) next = BODY_MAX_CAP;
    if (next < need) return false;
    char *p = (char *)realloc(*buf, next);
    if (!p) return false;
    *buf = p;
    *cap = next;
    return true;
}

static void free_state(notes_state *st) {
    free(st->body);
    free(st->base_body);
    free(st);
}

static bool contains_ci(const char *haystack, const char *needle) {
    if (!needle || !needle[0]) return true;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

static void refresh_filter(notes_state *st) {
    bool search_changed = strcmp(st->search, st->last_query) != 0;
    if (!search_changed && st->data_version == st->last_version) return;

    snprintf(st->last_query, sizeof st->last_query, "%s", st->search);
    st->last_version = st->data_version;
    st->filtered_count = 0;

    int count = notes_count(&st->repo);
    if (!st->search[0]) {
        for (int i = count - 1; i >= 0 && st->filtered_count < MAX_FILTERED; i--) {
            st->filtered[st->filtered_count++] = i;
        }
        return;
    }

    int found = notes_search(&st->repo, st->search, st->filtered, MAX_FILTERED);
    if (found >= 0) {
        st->filtered_count = found;
        return;
    }

    for (int i = count - 1; i >= 0 && st->filtered_count < MAX_FILTERED; i--) {
        const note *n = notes_at(&st->repo, i);
        if (n && contains_ci(n->title, st->search)) st->filtered[st->filtered_count++] = i;
    }
}

static void select_note(notes_state *st, int index) {
    const note *n = notes_at(&st->repo, index);
    if (!n) return;
    st->selected = index;
    st->confirm_delete = -1;
    st->confirm_frames = 0;

    size_t len = 0;
    wada_err err;
    if (notes_body_size(&st->repo, index, &len, &err) != WADA_OK) {
        snprintf(st->status, sizeof st->status, "%s", wada_err_msg(&err));
        len = 0;
    }
    size_t need = len + 1 + 4096;
    if (need < BODY_MIN_CAP) need = BODY_MIN_CAP;
    if (!grow_buffer(&st->body, &st->body_cap, need) ||
        !grow_buffer(&st->base_body, &st->base_cap, st->body_cap)) {
        wada_app_set_status(st->app, wada_tr("out of memory"));
        return;
    }

    snprintf(st->title, sizeof st->title, "%s", n->title);
    if (notes_body(&st->repo, index, st->body, st->body_cap, &err) != WADA_OK) {
        snprintf(st->status, sizeof st->status, "%s", wada_err_msg(&err));
        st->body[0] = '\0';
    }
    snprintf(st->base_body, st->base_cap, "%s", st->body);
    st->dirty = false;
    st->idle = 0;
}

static void format_when(long long updated, char *out, size_t cap) {
    time_t t = (time_t)updated;
    struct tm *tm = localtime(&t);
    if (!tm) {
        snprintf(out, cap, "");
        return;
    }
    strftime(out, cap, "%Y-%m-%d %H:%M", tm);
}

static void save_now(notes_state *st, bool announce) {
    if (st->selected < 0 || st->selected >= notes_count(&st->repo)) return;
    wada_err err;
    if (notes_save(&st->repo, st->selected, st->title, st->body, &err) == WADA_OK) {
        if (grow_buffer(&st->base_body, &st->base_cap, st->body_cap)) {
            snprintf(st->base_body, st->base_cap, "%s", st->body);
        }
        st->dirty = false;
        st->idle = 0;
        st->data_version++;
        if (announce) wada_app_set_status(st->app, wada_tr("saved"));
    } else {
        WADA_LOGW("notes", "save failed: %s", wada_err_msg(&err));
        wada_app_set_status(st->app, wada_err_msg(&err));
    }
}

static void tick_autosave(notes_state *st) {
    if (st->selected < 0 || st->selected >= notes_count(&st->repo)) return;
    const note *n = notes_at(&st->repo, st->selected);
    if (!n) return;

    bool changed = strcmp(st->title, n->title) != 0 || strcmp(st->body, st->base_body) != 0;
    if (changed) {
        st->dirty = true;
        st->idle = 0;
        return;
    }
    if (!st->dirty) return;
    if (++st->idle < AUTOSAVE_FRAMES) return;
    save_now(st, false);
}

/* Grow the editor buffer before CUI's textarea hits its cap. */
static void tick_growth(notes_state *st) {
    if (!st->body || st->body_cap >= BODY_MAX_CAP) return;
    if (strlen(st->body) + 1 >= st->body_cap) {
        if (!grow_buffer(&st->body, &st->body_cap, st->body_cap * 2)) {
            wada_app_set_status(st->app, wada_tr("body limit reached"));
        }
    }
}

static void cmd_new(void *user) {
    notes_state *st = (notes_state *)user;
    wada_err err;
    int index = notes_create(&st->repo, &err);
    if (index < 0) {
        wada_app_set_status(st->app, wada_err_msg(&err));
        return;
    }
    st->data_version++;
    select_note(st, index);
    snprintf(st->search, sizeof st->search, "%s", "");
    wada_app_set_status(st->app, wada_tr("new note"));
}

static void delete_selected(notes_state *st) {
    if (st->selected < 0) return;
    int old = st->selected;
    wada_err err;
    if (notes_remove(&st->repo, old, &err) != WADA_OK) {
        wada_app_set_status(st->app, wada_err_msg(&err));
        return;
    }
    st->data_version++;
    st->selected = -1;
    st->confirm_delete = -1;
    int count = notes_count(&st->repo);
    if (count > 0) select_note(st, old < count ? old : count - 1);
    wada_app_set_status(st->app, wada_tr("note deleted"));
}

static void cmd_delete(void *user) {
    notes_state *st = (notes_state *)user;
    if (st->selected < 0) return;
    if (st->confirm_delete != st->selected) {
        st->confirm_delete = st->selected;
        st->confirm_frames = CONFIRM_FRAMES;
        wada_app_set_status(st->app, wada_tr("click Delete again to confirm"));
        return;
    }
    delete_selected(st);
}

static void cmd_save(void *user) {
    save_now((notes_state *)user, true);
}

static void cmd_export(void *user) {
    notes_state *st = (notes_state *)user;
    char path[1024];
    cui_file_dialog_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.kind = CUI_FILE_SAVE;
    opts.title = wada_tr("Export notes");
    opts.filter_name = wada_tr("Wada notes");
    opts.filter = "wnotes";
    opts.default_name = "notes.wnotes";
    if (!cui_app_file_dialog(NULL, &opts, path, sizeof path)) return;

    wada_err err;
    if (notes_export(&st->repo, path, &err) == WADA_OK) {
        wada_trf(st->status, sizeof st->status, "exported %d notes", notes_count(&st->repo));
        wada_app_set_status(st->app, st->status);
    } else {
        wada_app_set_status(st->app, wada_err_msg(&err));
    }
}

static void cmd_import(void *user) {
    notes_state *st = (notes_state *)user;
    char path[1024];
    cui_file_dialog_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.kind = CUI_FILE_OPEN;
    opts.title = wada_tr("Import notes");
    opts.filter_name = wada_tr("Wada notes");
    opts.filter = "wnotes";
    if (!cui_app_file_dialog(NULL, &opts, path, sizeof path)) return;

    wada_err err;
    int added = 0;
    if (notes_import(&st->repo, path, &added, &err) == WADA_OK) {
        st->data_version++;
        wada_trf(st->status, sizeof st->status, "imported %d notes", added);
        wada_app_set_status(st->app, st->status);
        if (notes_count(&st->repo) > 0 && st->selected < 0) select_note(st, 0);
    } else {
        wada_app_set_status(st->app, wada_err_msg(&err));
    }
}

static void sidebar(notes_state *st) {
    cui_ctx *ui = wada_app_ui(st->app);

    cui_column(ui, .width = CUI_PX(280), .height = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED,
               .pad_left = 10, .pad_right = 10, .pad_top = 14, .gap = 6) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
            cui_text(ui, wada_tr("Notes"), .font = CUI_FONT_DISPLAY, .size = 18);
            cui_textf(ui, "%d", notes_count(&st->repo));
        }
        cui_search(ui, "notes-search", st->search, sizeof st->search,
                   .placeholder = wada_tr("Search"));

        if (st->filtered_count == 0) {
            cui_text(ui, wada_tr(st->search[0] ? "No matches" : "No notes yet"),
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        } else {
            int first = 0;
            int last = 0;
            cui_virtual_list_begin(ui, "notes-list", st->filtered_count, ROW_HEIGHT, CUI_FILL,
                                   &first, &last);
            for (int i = first; i < last && i < st->filtered_count; i++) {
                int index = st->filtered[i];
                const note *n = notes_at(&st->repo, index);
                if (!n) continue;
                char when[32];
                format_when(n->updated, when, sizeof when);
                if (cui_list_item(ui, n->title[0] ? n->title : wada_tr("Untitled"),
                                  .selected = index == st->selected, .subtitle = when)) {
                    select_note(st, index);
                }
            }
            cui_virtual_list_end(ui);
        }

        if (cui_button(ui, wada_tr("New note"), .icon = CUI_ICON_PLUS,
                       .variant = CUI_BUTTON_PRIMARY, .width = CUI_FILL)) {
            cmd_new(st);
        }
    }
}

static void editor(notes_state *st) {
    cui_ctx *ui = wada_app_ui(st->app);
    const note *n = notes_at(&st->repo, st->selected);
    bool confirming = n && st->confirm_delete == st->selected;

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad_left = 28, .pad_right = 28,
               .pad_top = 20, .gap = 10) {
        if (!n) {
            cui_text(ui, wada_tr("No note selected"), .font = CUI_FONT_DISPLAY, .size = 22);
            cui_text(ui, wada_tr("Create a note, or pick one on the left."), .size = 13,
                     .color = CUI_COLOR_TEXT_MUTED);
            if (cui_button(ui, wada_tr("New note"), .variant = CUI_BUTTON_PRIMARY)) cmd_new(st);
        } else {
            cui_input(ui, "note-title", st->title, sizeof st->title, .placeholder = wada_tr("Title"));
            if (strlen(st->title) + 1 >= sizeof st->title) {
                cui_text(ui, wada_tr("title limit reached"), .size = 11, .color = CUI_COLOR_ACCENT);
            }
            cui_textarea(ui, "note-body", st->body, st->body_cap, .placeholder = wada_tr("Write…"),
                         .height = -1);

            char when[32];
            char chars[48];
            format_when(n->updated, when, sizeof when);
            wada_trf(chars, sizeof chars, "%d chars", (int)strlen(st->body));
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 10) {
                cui_text(ui, wada_tr(st->dirty ? "Unsaved" : "Saved"), .size = 11,
                         .color = st->dirty ? CUI_COLOR_ACCENT : CUI_COLOR_TEXT_MUTED);
                cui_text(ui, when, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
                cui_text(ui, chars);
                cui_text(ui, st->status, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            }
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
                if (confirming) {
                    cui_text(ui, wada_tr("Delete this note?"), .size = 12, .color = CUI_COLOR_ACCENT);
                }
                if (cui_button(ui, wada_tr(confirming ? "Confirm delete" : "Delete"),
                               .variant = confirming ? CUI_BUTTON_PRIMARY : CUI_BUTTON_OUTLINE,
                               .icon = CUI_ICON_TRASH_2)) {
                    cmd_delete(st);
                }
                if (confirming && cui_button(ui, wada_tr("Keep"), .variant = CUI_BUTTON_GHOST)) {
                    st->confirm_delete = -1;
                    st->confirm_frames = 0;
                }
            }
        }
    }
}

static void cancel_confirm(notes_state *st, cui_ctx *ui) {
    if (st->confirm_delete < 0) return;
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE) || st->confirm_delete != st->selected ||
        st->confirm_frames <= 0) {
        st->confirm_delete = -1;
        st->confirm_frames = 0;
        return;
    }
    st->confirm_frames--;
}

static void notes_frame(void *state, wada_app *app) {
    notes_state *st = (notes_state *)state;
    st->app = app;
    tick_growth(st);
    refresh_filter(st);
    tick_autosave(st);
    cancel_confirm(st, wada_app_ui(app));

    cui_row(wada_app_ui(app), .width = CUI_FILL, .height = CUI_FILL) {
        sidebar(st);
        editor(st);
    }
}

/* Static: registered by the host for the life of the app, not copied. */
static const wada_i18n_pair notes_strings[] = {
    {"Notes", "Ghi chú"},
    {"Search", "Tìm kiếm"},
    {"No matches", "Không có kết quả"},
    {"No notes yet", "Chưa có ghi chú"},
    {"Untitled", "Chưa đặt tên"},
    {"New note", "Ghi chú mới"},
    {"No note selected", "Chưa chọn ghi chú"},
    {"Create a note, or pick one on the left.", "Tạo ghi chú mới, hoặc chọn một ghi chú bên trái."},
    {"Title", "Tiêu đề"},
    {"title limit reached", "tiêu đề đã đạt giới hạn"},
    {"Write…", "Viết…"},
    {"Unsaved", "Chưa lưu"},
    {"Saved", "Đã lưu"},
    {"%d chars", "%d ký tự"},
    {"Delete this note?", "Xóa ghi chú này?"},
    {"Confirm delete", "Xác nhận xóa"},
    {"Delete", "Xóa"},
    {"Keep", "Giữ"},
    {"Delete note", "Xóa ghi chú"},
    {"Save now", "Lưu ngay"},
    {"Export notes…", "Xuất ghi chú…"},
    {"Import notes…", "Nhập ghi chú…"},
    {"Export notes", "Xuất ghi chú"},
    {"Import notes", "Nhập ghi chú"},
    {"Wada notes", "Ghi chú Wada"},
    {"new note", "ghi chú mới"},
    {"note deleted", "đã xóa ghi chú"},
    {"click Delete again to confirm", "bấm Xóa lần nữa để xác nhận"},
    {"saved", "đã lưu"},
    {"out of memory", "hết bộ nhớ"},
    {"body limit reached", "nội dung đã đạt giới hạn"},
    {"exported %d notes", "đã xuất %d ghi chú"},
    {"imported %d notes", "đã nhập %d ghi chú"},
    {"Welcome to Wada", "Chào mừng đến Wada"},
    {"This note is a metadata record plus a body record in SQLite.\n\n"
     "- The list holds titles only; a body loads when you select it.\n"
     "- Search goes through the store's FTS index, not a per-frame scan.\n"
     "- Delete asks for a second click; Export/Import back up every note.",
     "Ghi chú này gồm một bản ghi siêu dữ liệu và một bản ghi nội dung trong SQLite.\n\n"
     "- Danh sách chỉ giữ tiêu đề; nội dung được nạp khi bạn chọn ghi chú.\n"
     "- Tìm kiếm dùng chỉ mục FTS của kho, không quét lại mỗi khung hình.\n"
     "- Xóa cần bấm lần thứ hai; Xuất/Nhập sao lưu toàn bộ ghi chú."},
};

/* States a single frame does not show: the delete confirmation. */
static const char *const notes_walk[] = {"notes.delete"};

static void *notes_enter(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err) {
    notes_state *st = (notes_state *)calloc(1, sizeof *st);
    if (!st) {
        wada_err_set(err, WADA_ERR_NOMEM, "notes");
        return NULL;
    }
    st->app = app;
    st->selected = -1;
    st->last_version = -1;
    st->confirm_delete = -1;

    if (!grow_buffer(&st->body, &st->body_cap, BODY_MIN_CAP) ||
        !grow_buffer(&st->base_body, &st->base_cap, BODY_MIN_CAP)) {
        free_state(st);
        wada_err_set(err, WADA_ERR_NOMEM, "notes");
        return NULL;
    }

    const wada_store_ops *ops = wada_registry_get(wada_app_registry(app), "store", WADA_STORE_ABI, NULL);
    if (!ops) {
        wada_err_set(err, WADA_ERR_NOT_FOUND, "notes: no store service");
        free_state(st);
        return NULL;
    }

    char path[640];
    snprintf(path, sizeof path, "%s" WADA_PATH_SEP "wada.db", data_dir);
    if (notes_open(&st->repo, ops, path, err) != WADA_OK) {
        free_state(st);
        return NULL;
    }

    if (notes_count(&st->repo) == 0) {
        int idx = notes_create(&st->repo, err);
        if (idx >= 0) {
            notes_save(&st->repo, idx, wada_tr("Welcome to Wada"),
                       wada_tr("This note is a metadata record plus a body record in SQLite.\n\n"
                               "- The list holds titles only; a body loads when you select it.\n"
                               "- Search goes through the store's FTS index, not a per-frame scan.\n"
                               "- Delete asks for a second click; Export/Import back up every note."),
                       err);
        }
    }
    if (notes_count(&st->repo) > 0) select_note(st, 0);

    wada_app_command(app, scope, "notes.new", "New note", "Ctrl N", cmd_new, st);
    wada_app_command(app, scope, "notes.delete", "Delete note", NULL, cmd_delete, st);
    wada_app_command(app, scope, "notes.save", "Save now", "Ctrl S", cmd_save, st);
    wada_app_command(app, scope, "notes.export", "Export notes…", NULL, cmd_export, st);
    wada_app_command(app, scope, "notes.import", "Import notes…", NULL, cmd_import, st);
    return st;
}

static void notes_leave(void *state) {
    notes_state *st = (notes_state *)state;
    if (!st) return;
    save_now(st, false);
    notes_close(&st->repo);
    free_state(st);
}

const wada_product notes_product = {
    "notes",
    "Notes",
    notes_enter,
    notes_leave,
    notes_frame,
    notes_strings,
    (int)(sizeof notes_strings / sizeof notes_strings[0]),
    notes_walk,
    (int)(sizeof notes_walk / sizeof notes_walk[0]),
};
