/* Learning product: cards, a due queue and a small review session.
 *
 * Second product on the same core. It uses the store service and the product
 * scope; switching to Notes disposes this screen's commands and store handle
 * without a line of teardown code.
 */
#include <cui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cards.h"
#include "wada_app.h"
#include "wada_log.h"
#include "wada_product.h"

#define MAX_QUEUE 512

enum {
    MODE_BROWSE = 0,
    MODE_STUDY = 1,
    MODE_ADD = 2
};

/* Static: registered by the host for the life of the app, not copied. */
static const wada_i18n_pair learn_strings[] = {
    {"Learn", "Học tập"},
    {"Cards are waiting.", "Có thẻ đang chờ."},
    {"Nothing due right now.", "Hiện không có thẻ đến hạn."},
    {"due now: %d", "đến hạn: %d"},
    {"total: %d", "tổng: %d"},
    {"Study", "Học"},
    {"Browse", "Duyệt"},
    {"Add card", "Thêm thẻ"},
    {"Cards", "Thẻ"},
    {"No cards yet. Add one to start.", "Chưa có thẻ nào. Thêm một thẻ để bắt đầu."},
    {"due now", "đến hạn"},
    {"Nothing due", "Không có thẻ đến hạn"},
    {"Add cards, then study them here.", "Thêm thẻ rồi học tại đây."},
    {"Next card is due %s.", "Thẻ tiếp theo đến hạn %s."},
    {"Check again", "Kiểm tra lại"},
    {"card %d of %d", "thẻ %d / %d"},
    {"Show answer", "Xem đáp án"},
    {"Again", "Lại"},
    {"Hard", "Khó"},
    {"Good", "Được"},
    {"Easy", "Dễ"},
    {"Add a card", "Thêm thẻ"},
    {"Front is the question, back is the answer.", "Mặt trước là câu hỏi, mặt sau là đáp án."},
    {"Front", "Mặt trước"},
    {"What do you want to remember?", "Bạn muốn ghi nhớ điều gì?"},
    {"Back", "Mặt sau"},
    {"The answer", "Đáp án"},
    {"Cancel", "Hủy"},
    {"a card needs a front", "thẻ cần có mặt trước"},
    {"card added", "đã thêm thẻ"},
    {"session done: %d reviewed", "xong phiên: đã ôn %d thẻ"},
    {"reviewed %d", "đã ôn %d"},
    {"Study cards", "Học thẻ"},
    {"Browse cards", "Duyệt thẻ"},
    {"What does the core store behind wada_store_ops?",
     "Lõi lưu gì phía sau wada_store_ops?"},
    {"SQLite today: open, transactions, get/put/delete, prefix iteration, migrate.",
     "Hiện là SQLite: mở, giao dịch, get/put/delete, duyệt theo tiền tố, migrate."},
};

typedef struct learn_state {
    wada_app *app;
    cards repo;
    int mode;
    int selected;
    int queue[MAX_QUEUE];
    int queue_count;
    int pos;
    bool revealed;
    int reviewed;
    char front[CARD_TEXT_CAP];
    char back[CARD_TEXT_CAP];
    char status[96];
} learn_state;

static void format_when(long long when, char *out, size_t cap) {
    time_t t = (time_t)when;
    struct tm *tm = localtime(&t);
    if (!tm) {
        snprintf(out, cap, "");
        return;
    }
    strftime(out, cap, "%Y-%m-%d %H:%M", tm);
}

static void format_due(long long due, char *out, size_t cap) {
    if (due <= (long long)time(NULL)) {
        snprintf(out, cap, "%s", wada_tr("due now"));
        return;
    }
    format_when(due, out, cap);
}

static void start_study(learn_state *st) {
    st->queue_count = cards_due(&st->repo, (long long)time(NULL), st->queue, MAX_QUEUE);
    st->pos = 0;
    st->revealed = false;
    st->reviewed = 0;
    st->mode = MODE_STUDY;
}

static void grade(learn_state *st, int value) {
    if (st->pos >= st->queue_count) return;
    wada_err err;
    int index = st->queue[st->pos];
    if (cards_review(&st->repo, index, value, &err) != WADA_OK) {
        snprintf(st->status, sizeof st->status, "%s", wada_err_msg(&err));
        wada_app_set_status(st->app, st->status);
        return;
    }
    st->pos++;
    st->revealed = false;
    st->reviewed++;
    if (st->pos >= st->queue_count) {
        wada_trf(st->status, sizeof st->status, "session done: %d reviewed", st->reviewed);
        wada_app_set_status(st->app, st->status);
    } else {
        wada_trf(st->status, sizeof st->status, "reviewed %d", st->reviewed);
        wada_app_set_status(st->app, st->status);
    }
}

static void cmd_browse(void *user) {
    learn_state *st = (learn_state *)user;
    st->mode = MODE_BROWSE;
}

static void cmd_study(void *user) {
    start_study((learn_state *)user);
}

static void cmd_add(void *user) {
    learn_state *st = (learn_state *)user;
    st->mode = MODE_ADD;
    st->front[0] = '\0';
    st->back[0] = '\0';
}

static void add_card(learn_state *st) {
    if (!st->front[0]) {
        wada_app_set_status(st->app, wada_tr("a card needs a front"));
        return;
    }
    wada_err err;
    int index = cards_create(&st->repo, st->front, st->back, &err);
    if (index < 0) {
        wada_app_set_status(st->app, wada_err_msg(&err));
        return;
    }
    st->selected = index;
    st->front[0] = '\0';
    st->back[0] = '\0';
    st->mode = MODE_BROWSE;
    snprintf(st->status, sizeof st->status, "%s", wada_tr("card added"));
    wada_app_set_status(st->app, st->status);
}

static void sidebar(learn_state *st) {
    cui_ctx *ui = wada_app_ui(st->app);
    long long now = (long long)time(NULL);
    int due = cards_due_count(&st->repo, now);

    cui_column(ui, .width = CUI_PX(280), .height = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED,
               .pad_left = 10, .pad_right = 10, .pad_top = 14, .gap = 8) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
            cui_text(ui, wada_tr("Learn"), .font = CUI_FONT_DISPLAY, .size = 18);
            cui_textf(ui, "%d", cards_count(&st->repo));
        }
        cui_text(ui, wada_tr(due > 0 ? "Cards are waiting." : "Nothing due right now."), .size = 12,
                 .color = due > 0 ? CUI_COLOR_ACCENT : CUI_COLOR_TEXT_MUTED);
        { char _l[64]; wada_trf(_l, sizeof _l, "due now: %d", due); cui_text(ui, _l); }
        { char _l[64]; wada_trf(_l, sizeof _l, "total: %d", cards_count(&st->repo)); cui_text(ui, _l); }

        cui_column(ui, .width = CUI_FILL, .gap = 6) {
            if (cui_button(ui, wada_tr("Study"), .icon = CUI_ICON_PLAY, .width = CUI_FILL,
                           .variant = st->mode == MODE_STUDY ? CUI_BUTTON_PRIMARY : CUI_BUTTON_OUTLINE)) {
                start_study(st);
            }
            if (cui_button(ui, wada_tr("Browse"), .icon = CUI_ICON_LIST, .width = CUI_FILL,
                           .variant = st->mode == MODE_BROWSE ? CUI_BUTTON_PRIMARY : CUI_BUTTON_OUTLINE)) {
                st->mode = MODE_BROWSE;
            }
            if (cui_button(ui, wada_tr("Add card"), .icon = CUI_ICON_PLUS, .width = CUI_FILL,
                           .variant = st->mode == MODE_ADD ? CUI_BUTTON_PRIMARY : CUI_BUTTON_OUTLINE)) {
                cmd_add(st);
            }
        }
        cui_text(ui, st->status, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
    }
}

static void browse(learn_state *st) {
    cui_ctx *ui = wada_app_ui(st->app);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad_left = 28, .pad_right = 28,
               .pad_top = 20, .gap = 10) {
        cui_text(ui, wada_tr("Cards"), .font = CUI_FONT_DISPLAY, .size = 24);
        if (cards_count(&st->repo) == 0) {
            cui_text(ui, wada_tr("No cards yet. Add one to start."), .size = 13, .color = CUI_COLOR_TEXT_MUTED);
            if (cui_button(ui, wada_tr("Add card"), .variant = CUI_BUTTON_PRIMARY)) cmd_add(st);
        } else {
            int count = cards_count(&st->repo);
            int first = 0;
            int last = 0;
            cui_virtual_list_begin(ui, "cards-list", count, 56.0f, CUI_FILL, &first, &last);
            for (int row = first; row < last && row < count; row++) {
                int i = count - 1 - row; /* newest first */
                const card *c = cards_at(&st->repo, i);
                if (!c) continue;
                char due[32];
                format_due(c->due, due, sizeof due);
                if (cui_list_item(ui, c->front, .selected = i == st->selected, .subtitle = due)) {
                    st->selected = i;
                }
            }
            cui_virtual_list_end(ui);
        }
    }
}

static void study(learn_state *st) {
    cui_ctx *ui = wada_app_ui(st->app);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad_left = 28, .pad_right = 28,
               .pad_top = 20, .gap = 12) {
        if (st->pos >= st->queue_count) {
            cui_text(ui, wada_tr("Nothing due"), .font = CUI_FONT_DISPLAY, .size = 24);
            if (cards_count(&st->repo) == 0) {
                cui_text(ui, wada_tr("Add cards, then study them here."), .size = 13,
                         .color = CUI_COLOR_TEXT_MUTED);
            } else {
                char next[32];
                format_when(cards_next_due(&st->repo), next, sizeof next);
                { char _l[96]; wada_trf(_l, sizeof _l, "Next card is due %s.", next); cui_text(ui, _l); }
            }
            if (cui_button(ui, wada_tr("Check again"), .variant = CUI_BUTTON_OUTLINE)) start_study(st);
        } else {
            const card *c = cards_at(&st->repo, st->queue[st->pos]);
            if (!c) return;

            { char _l[64]; wada_trf(_l, sizeof _l, "card %d of %d", st->pos + 1, st->queue_count); cui_text(ui, _l); }
            cui_column(ui, .width = CUI_FILL, .bg = CUI_COLOR_SURFACE_RAISED, .pad = 20, .gap = 8) {
                cui_text(ui, c->front, .font = CUI_FONT_DISPLAY, .size = 22, .width = CUI_FILL,
                         .line_height = 1.35f);
                if (st->revealed) {
                    cui_text(ui, c->back, .size = 14, .width = CUI_FILL, .line_height = 1.45f);
                }
            }

            if (!st->revealed) {
                if (cui_button(ui, wada_tr("Show answer"), .variant = CUI_BUTTON_PRIMARY)) st->revealed = true;
            } else {
                cui_row(ui, .width = CUI_FILL, .gap = 8) {
                    if (cui_button(ui, wada_tr("Again"), .variant = CUI_BUTTON_OUTLINE, .width = CUI_FILL)) grade(st, 0);
                    if (cui_button(ui, wada_tr("Hard"), .variant = CUI_BUTTON_OUTLINE, .width = CUI_FILL)) grade(st, 1);
                    if (cui_button(ui, wada_tr("Good"), .variant = CUI_BUTTON_PRIMARY, .width = CUI_FILL)) grade(st, 2);
                    if (cui_button(ui, wada_tr("Easy"), .variant = CUI_BUTTON_OUTLINE, .width = CUI_FILL)) grade(st, 3);
                }
            }
        }
    }
}

static void add_form(learn_state *st) {
    cui_ctx *ui = wada_app_ui(st->app);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad_left = 28, .pad_right = 28,
               .pad_top = 20, .gap = 10) {
        cui_text(ui, wada_tr("Add a card"), .font = CUI_FONT_DISPLAY, .size = 24);
        cui_text(ui, wada_tr("Front is the question, back is the answer."), .size = 13,
                 .color = CUI_COLOR_TEXT_MUTED);
        cui_input(ui, "card-front", st->front, sizeof st->front, .label = wada_tr("Front"),
                  .placeholder = wada_tr("What do you want to remember?"));
        cui_input(ui, "card-back", st->back, sizeof st->back, .label = wada_tr("Back"),
                  .placeholder = wada_tr("The answer"));
        cui_row(ui, .width = CUI_FILL, .gap = 8) {
            if (cui_button(ui, wada_tr("Add card"), .variant = CUI_BUTTON_PRIMARY)) add_card(st);
            if (cui_button(ui, wada_tr("Cancel"), .variant = CUI_BUTTON_OUTLINE)) st->mode = MODE_BROWSE;
        }
    }
}

static void learn_frame(void *state, wada_app *app) {
    learn_state *st = (learn_state *)state;
    st->app = app;

    cui_row(wada_app_ui(app), .width = CUI_FILL, .height = CUI_FILL) {
        sidebar(st);
        if (st->mode == MODE_STUDY) study(st);
        else if (st->mode == MODE_ADD) add_form(st);
        else browse(st);
    }
}

/* States a single frame does not show: a review session and the add form. */
static const char *const learn_walk[] = {"learning.study", "learning.add"};

static void *learn_enter(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err) {
    learn_state *st = (learn_state *)calloc(1, sizeof *st);
    if (!st) {
        wada_err_set(err, WADA_ERR_NOMEM, "learning");
        return NULL;
    }
    st->app = app;
    st->selected = -1;

    const wada_store_ops *ops = wada_registry_get(wada_app_registry(app), "store", WADA_STORE_ABI, NULL);
    if (!ops) {
        wada_err_set(err, WADA_ERR_NOT_FOUND, "learning: no store service");
        free(st);
        return NULL;
    }

    char path[640];
    snprintf(path, sizeof path, "%s" WADA_PATH_SEP "wada.db", data_dir);
    if (cards_open(&st->repo, ops, path, err) != WADA_OK) {
        free(st);
        return NULL;
    }

    if (cards_count(&st->repo) == 0) {
        cards_create(&st->repo, wada_tr("What does the core store behind wada_store_ops?"),
                     wada_tr("SQLite today: open, transactions, get/put/delete, prefix iteration, "
                             "migrate."),
                     err);
    }
    if (cards_count(&st->repo) > 0) st->selected = 0;

    /* Capture/demo hook: start directly in a review session. */
    if (getenv("WADA_STUDY")) start_study(st);

    wada_app_command(app, scope, "learning.study", "Study cards", NULL, cmd_study, st);
    wada_app_command(app, scope, "learning.browse", "Browse cards", NULL, cmd_browse, st);
    wada_app_command(app, scope, "learning.add", "Add a card", NULL, cmd_add, st);
    return st;
}

static void learn_leave(void *state) {
    learn_state *st = (learn_state *)state;
    if (!st) return;
    cards_close(&st->repo);
    free(st);
}

const wada_product learning_product = {
    "learning",
    "Learn",
    learn_enter,
    learn_leave,
    learn_frame,
    learn_strings,
    (int)(sizeof learn_strings / sizeof learn_strings[0]),
    learn_walk,
    (int)(sizeof learn_walk / sizeof learn_walk[0]),
};
