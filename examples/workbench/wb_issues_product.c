/* Workbench Issues product: projects, FTS5 search, issue editor, multi-subscriber bus.
 *
 * Uses wada_records (via wb_store), wada_ui, scoped "wb/*" subscription for
 * cache invalidation, and the app-lifetime "wb.activity" service from registry.
 */
#include <cui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wb_store.h"
#include "wada_app.h"
#include "wada_log.h"
#include "wada_product.h"
#include "wada_ui.h"

#define WB_MAX_FILTERED 1024
#define WB_AUTOSAVE_FRAMES 25
#define WB_CONFIRM_FRAMES 300

typedef struct issues_state {
    wada_app *app;
    wb_store repo;
    const wb_activity_ops *activity_svc;
    int selected;
    char project_filter[WB_ID_CAP]; /* empty = all projects */
    int status_filter;              /* -1 = all statuses */
    char search[64];
    char last_query[64];
    char last_project[WB_ID_CAP];
    int last_status;
    bool dirty_filter;
    int filtered[WB_MAX_FILTERED];
    int filtered_count;

    char edit_title[WB_TITLE_CAP];
    char edit_notes[WB_NOTES_CAP];
    int edit_status;
    int edit_priority;
    bool dirty_edit;
    int idle_frames;

    bool show_new_proj;
    char new_proj_name[WB_NAME_CAP];
    wada_ui_confirm confirm;
    char status[128];
} issues_state;

static void on_wb_changed(void *user, const char *topic, const void *data, size_t len) {
    (void)topic;
    (void)data;
    (void)len;
    issues_state *st = (issues_state *)user;
    st->dirty_filter = true;
}

static void emit_wb(issues_state *st, const char *topic, const char *summary) {
    if (!st || !st->app || !topic) return;
    const char *msg = summary ? summary : "";
    wada_app_emit(st->app, topic, msg, strlen(msg) + 1);
}

static const char *project_name_by_id(const wb_store *s, const char *id) {
    int idx = wb_project_find(s, id);
    return idx >= 0 ? s->projects[idx].name : "Unassigned";
}

static void select_issue(issues_state *st, int index) {
    if (index < 0 || index >= st->repo.issue_count) {
        st->selected = -1;
        st->edit_title[0] = '\0';
        st->edit_notes[0] = '\0';
        st->dirty_edit = false;
        st->idle_frames = 0;
        wada_ui_confirm_reset(&st->confirm);
        return;
    }
    const wb_issue *it = &st->repo.issues[index];
    st->selected = index;
    st->show_new_proj = false;
    snprintf(st->edit_title, sizeof st->edit_title, "%s", it->title);
    snprintf(st->edit_notes, sizeof st->edit_notes, "%s", it->notes);
    st->edit_status = it->status;
    st->edit_priority = it->priority;
    st->dirty_edit = false;
    st->idle_frames = 0;
    wada_ui_confirm_reset(&st->confirm);
}

static void save_current(issues_state *st, bool announce) {
    if (st->selected < 0 || st->selected >= st->repo.issue_count) return;
    const wb_issue *cur = &st->repo.issues[st->selected];
    bool changed = strcmp(st->edit_title, cur->title) != 0 ||
                   strcmp(st->edit_notes, cur->notes) != 0 ||
                   st->edit_status != cur->status ||
                   st->edit_priority != cur->priority;
    if (!changed) {
        st->dirty_edit = false;
        st->idle_frames = 0;
        return;
    }

    wada_err err;
    if (wb_issue_save(&st->repo, st->selected, st->edit_title, st->edit_notes,
                      st->edit_status, st->edit_priority, &err) == WADA_OK) {
        st->dirty_edit = false;
        st->idle_frames = 0;
        emit_wb(st, "wb/issue.updated", st->repo.issues[st->selected].title);
        if (announce) {
            snprintf(st->status, sizeof st->status, "issue saved");
            wada_app_set_status(st->app, st->status);
        }
    } else {
        wada_ui_status_err(st->app, st->status, sizeof st->status, &err);
    }
}

static void tick_autosave(issues_state *st) {
    if (st->selected < 0 || st->selected >= st->repo.issue_count) return;
    const wb_issue *cur = &st->repo.issues[st->selected];
    bool changed = strcmp(st->edit_title, cur->title) != 0 ||
                   strcmp(st->edit_notes, cur->notes) != 0 ||
                   st->edit_status != cur->status ||
                   st->edit_priority != cur->priority;
    if (!changed) {
        st->dirty_edit = false;
        st->idle_frames = 0;
        return;
    }
    if (!st->dirty_edit) {
        st->dirty_edit = true;
        st->idle_frames = 0;
        return;
    }
    if (++st->idle_frames >= WB_AUTOSAVE_FRAMES) {
        save_current(st, false);
    }
}

static void refresh_filter(issues_state *st) {
    bool query_changed = strcmp(st->search, st->last_query) != 0;
    bool proj_changed = strcmp(st->project_filter, st->last_project) != 0;
    bool status_changed = st->status_filter != st->last_status;
    if (!st->dirty_filter && !query_changed && !proj_changed && !status_changed) return;

    snprintf(st->last_query, sizeof st->last_query, "%s", st->search);
    snprintf(st->last_project, sizeof st->last_project, "%s", st->project_filter);
    st->last_status = st->status_filter;
    st->dirty_filter = false;
    st->filtered_count = wb_issue_search(&st->repo, st->project_filter, st->status_filter,
                                         st->search, st->filtered, WB_MAX_FILTERED);
}

static void cmd_new_issue(void *user) {
    issues_state *st = (issues_state *)user;
    save_current(st, false);
    const char *pid = st->project_filter[0] ? st->project_filter
                      : (st->repo.project_count > 0 ? st->repo.projects[0].id : "");
    wada_err err;
    int idx = wb_issue_create(&st->repo, pid, "New issue", "", WB_MED, &err);
    if (idx < 0) {
        wada_ui_status_err(st->app, st->status, sizeof st->status, &err);
        return;
    }
    st->search[0] = '\0';
    st->status_filter = -1;
    select_issue(st, idx);
    emit_wb(st, "wb/issue.created", st->repo.issues[idx].title);
    snprintf(st->status, sizeof st->status, "created issue");
    wada_app_set_status(st->app, st->status);
}

static void cmd_save_issue(void *user) {
    save_current((issues_state *)user, true);
}

static void cmd_cycle_status(void *user) {
    issues_state *st = (issues_state *)user;
    if (st->selected < 0 || st->selected >= st->repo.issue_count) return;
    st->edit_status = (st->edit_status + 1) % 3;
    save_current(st, true);
}

static void cmd_delete_issue(void *user) {
    issues_state *st = (issues_state *)user;
    if (st->selected < 0 || st->selected >= st->repo.issue_count) return;
    st->show_new_proj = false;
    if (!wada_ui_confirm_step(&st->confirm, st->selected, WB_CONFIRM_FRAMES)) {
        wada_app_set_status(st->app, "click Delete again to confirm");
        return;
    }
    int old = st->selected;
    char removed_title[WB_TITLE_CAP];
    snprintf(removed_title, sizeof removed_title, "%s", st->repo.issues[old].title);

    wada_err err;
    if (wb_issue_remove(&st->repo, old, &err) != WADA_OK) {
        wada_ui_status_err(st->app, st->status, sizeof st->status, &err);
        return;
    }
    int next = st->repo.issue_count > 0 ? (old < st->repo.issue_count ? old : st->repo.issue_count - 1) : -1;
    select_issue(st, next);
    emit_wb(st, "wb/issue.deleted", removed_title);
    snprintf(st->status, sizeof st->status, "issue deleted");
    wada_app_set_status(st->app, st->status);
}

static void cmd_new_project(void *user) {
    issues_state *st = (issues_state *)user;
    st->show_new_proj = true;
    st->new_proj_name[0] = '\0';
    wada_ui_confirm_reset(&st->confirm);
}

static void submit_new_project(issues_state *st) {
    wada_err err;
    int idx = wb_project_create(&st->repo, st->new_proj_name, &err);
    if (idx < 0) {
        wada_ui_status_err(st->app, st->status, sizeof st->status, &err);
        return;
    }
    snprintf(st->project_filter, sizeof st->project_filter, "%s", st->repo.projects[idx].id);
    st->show_new_proj = false;
    st->new_proj_name[0] = '\0';
    emit_wb(st, "wb/project.created", st->repo.projects[idx].name);
    snprintf(st->status, sizeof st->status, "project added");
    wada_app_set_status(st->app, st->status);
}

static void sidebar(issues_state *st) {
    cui_ctx *ui = wada_app_ui(st->app);
    wb_counts_info counts = wb_counts(&st->repo, st->project_filter);

    cui_column(ui, .width = CUI_PX(310), .height = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED,
               .pad_left = 10, .pad_right = 10, .pad_top = 14, .gap = 6) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
            cui_text(ui, "Issues", .font = CUI_FONT_DISPLAY, .size = 18);
            cui_textf(ui, "%d open · %d doing · %d done", counts.open, counts.doing, counts.done);
        }

        cui_row(ui, .width = CUI_FILL, .gap = 4) {
            if (cui_button(ui, "All projects",
                           .variant = st->project_filter[0] ? CUI_BUTTON_GHOST : CUI_BUTTON_PRIMARY)) {
                st->project_filter[0] = '\0';
            }
            if (cui_button(ui, "+ Project", .variant = CUI_BUTTON_OUTLINE)) {
                cmd_new_project(st);
            }
        }
        if (st->repo.project_count > 0) {
            cui_row(ui, .width = CUI_FILL, .gap = 4) {
                for (int i = 0; i < st->repo.project_count && i < 4; i++) {
                    const wb_project *p = &st->repo.projects[i];
                    bool active = strcmp(st->project_filter, p->id) == 0;
                    if (cui_button(ui, p->name,
                                   .variant = active ? CUI_BUTTON_PRIMARY : CUI_BUTTON_OUTLINE)) {
                        snprintf(st->project_filter, sizeof st->project_filter, "%s", p->id);
                    }
                }
            }
        }

        cui_row(ui, .width = CUI_FILL, .gap = 4) {
            if (cui_button(ui, "All", .variant = st->status_filter == -1 ? CUI_BUTTON_PRIMARY : CUI_BUTTON_GHOST))
                st->status_filter = -1;
            if (cui_button(ui, "Open", .variant = st->status_filter == WB_OPEN ? CUI_BUTTON_PRIMARY : CUI_BUTTON_GHOST))
                st->status_filter = WB_OPEN;
            if (cui_button(ui, "Doing", .variant = st->status_filter == WB_DOING ? CUI_BUTTON_PRIMARY : CUI_BUTTON_GHOST))
                st->status_filter = WB_DOING;
            if (cui_button(ui, "Done", .variant = st->status_filter == WB_DONE ? CUI_BUTTON_PRIMARY : CUI_BUTTON_GHOST))
                st->status_filter = WB_DONE;
        }

        cui_search(ui, "wb-search", st->search, sizeof st->search, .placeholder = "Search issues (FTS5)");

        if (st->filtered_count == 0) {
            cui_text(ui, st->search[0] ? "No matching issues" : "No issues in view",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        } else {
            int first = 0;
            int last = 0;
            cui_virtual_list_begin(ui, "wb-issues-list", st->filtered_count, 58.0f, CUI_FILL,
                                   &first, &last);
            for (int row = first; row < last && row < st->filtered_count; row++) {
                int idx = st->filtered[row];
                const wb_issue *it = &st->repo.issues[idx];
                char sub[128];
                snprintf(sub, sizeof sub, "%s · %s · %s",
                         wb_status_name(it->status), wb_priority_name(it->priority),
                         project_name_by_id(&st->repo, it->project_id));
                if (cui_list_item(ui, it->title, .selected = !st->show_new_proj && idx == st->selected,
                                  .subtitle = sub)) {
                    save_current(st, false);
                    select_issue(st, idx);
                }
            }
            cui_virtual_list_end(ui);
        }

        if (cui_button(ui, "New issue", .icon = CUI_ICON_PLUS,
                       .variant = CUI_BUTTON_PRIMARY, .width = CUI_FILL)) {
            cmd_new_issue(st);
        }
    }
}

static void detail(issues_state *st) {
    cui_ctx *ui = wada_app_ui(st->app);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad_left = 28, .pad_right = 28,
               .pad_top = 20, .gap = 10) {
        if (st->show_new_proj) {
            cui_text(ui, "Create project", .font = CUI_FONT_DISPLAY, .size = 24);
            cui_text(ui, "Projects scope issue filtering and emit wb/project.created on the bus.",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            cui_input(ui, "wb-proj-name", st->new_proj_name, sizeof st->new_proj_name,
                      .label = "Project name", .placeholder = "e.g. Core, Storage, Docs");
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                if (cui_button(ui, "Create project", .variant = CUI_BUTTON_PRIMARY)) {
                    submit_new_project(st);
                }
                if (cui_button(ui, "Cancel", .variant = CUI_BUTTON_OUTLINE)) {
                    st->show_new_proj = false;
                }
            }
        } else if (st->selected < 0 || st->selected >= st->repo.issue_count) {
            cui_text(ui, "No issue selected", .font = CUI_FONT_DISPLAY, .size = 22);
            cui_text(ui, "Create an issue or pick one from the list.",
                     .size = 13, .color = CUI_COLOR_TEXT_MUTED);
            if (cui_button(ui, "New issue", .variant = CUI_BUTTON_PRIMARY)) cmd_new_issue(st);
        } else {
            const wb_issue *it = &st->repo.issues[st->selected];
            bool confirming = wada_ui_confirm_active(&st->confirm, st->selected);
            char when[32];
            wada_ui_format_when(it->updated, when, sizeof when);

            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 12) {
                cui_textf(ui, "Project: %s", project_name_by_id(&st->repo, it->project_id));
                cui_textf(ui, "Updated: %s", when);
                if (st->activity_svc) {
                    cui_textf(ui, "Session events: %d",
                              st->activity_svc->event_count(st->activity_svc->ctx));
                }
            }

            cui_input(ui, "wb-issue-title", st->edit_title, sizeof st->edit_title,
                      .placeholder = "Issue title");
            cui_textarea(ui, "wb-issue-notes", st->edit_notes, sizeof st->edit_notes,
                         .placeholder = "Notes, acceptance criteria, links…", .height = -1);

            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
                char status_lbl[48], prio_lbl[48];
                snprintf(status_lbl, sizeof status_lbl, "Status: %s", wb_status_name(st->edit_status));
                snprintf(prio_lbl, sizeof prio_lbl, "Priority: %s", wb_priority_name(st->edit_priority));
                if (cui_button(ui, status_lbl, .variant = CUI_BUTTON_OUTLINE)) cmd_cycle_status(st);
                if (cui_button(ui, prio_lbl, .variant = CUI_BUTTON_OUTLINE)) {
                    st->edit_priority = (st->edit_priority + 1) % 3;
                    save_current(st, true);
                }
                if (cui_button(ui, st->dirty_edit ? "Save *" : "Save",
                               .variant = st->dirty_edit ? CUI_BUTTON_PRIMARY : CUI_BUTTON_GHOST)) {
                    save_current(st, true);
                }
            }

            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8) {
                if (confirming) {
                    cui_text(ui, "Delete this issue?", .size = 12, .color = CUI_COLOR_ACCENT);
                }
                if (cui_button(ui, confirming ? "Confirm delete" : "Delete",
                               .variant = confirming ? CUI_BUTTON_PRIMARY : CUI_BUTTON_OUTLINE,
                               .icon = CUI_ICON_TRASH_2)) {
                    cmd_delete_issue(st);
                }
                if (confirming && cui_button(ui, "Keep", .variant = CUI_BUTTON_GHOST)) {
                    wada_ui_confirm_reset(&st->confirm);
                }
            }
        }
        cui_text(ui, st->status, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
    }
}

static void wb_issues_frame(void *state, wada_app *app) {
    issues_state *st = (issues_state *)state;
    st->app = app;
    refresh_filter(st);
    tick_autosave(st);
    wada_ui_confirm_tick(&st->confirm, wada_app_ui(app), st->selected);

    cui_row(wada_app_ui(app), .width = CUI_FILL, .height = CUI_FILL) {
        sidebar(st);
        detail(st);
    }
}

static void *wb_issues_enter(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err) {
    issues_state *st = (issues_state *)calloc(1, sizeof *st);
    if (!st) {
        wada_err_set(err, WADA_ERR_NOMEM, "wb.issues");
        return NULL;
    }
    st->app = app;
    st->selected = -1;
    st->status_filter = -1;
    st->last_status = -2;
    st->dirty_filter = true;
    wada_ui_confirm_init(&st->confirm);

    const wada_store_ops *ops = wada_ui_store_ops(app, "wb.issues", err);
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

    if (st->repo.project_count == 0) {
        int p1 = wb_project_create(&st->repo, "Core", err);
        int p2 = wb_project_create(&st->repo, "Examples", err);
        const char *core_id = p1 >= 0 ? st->repo.projects[p1].id : "";
        const char *ex_id = p2 >= 0 ? st->repo.projects[p2].id : "";
        if (st->repo.issue_count == 0) {
            wb_issue_create(&st->repo, core_id, "Wildcard topics on wada_bus",
                            "wb/* matches project and issue events with priority ordering.", WB_HIGH, err);
            wb_issue_create(&st->repo, core_id, "FTS5 search via wada_records_search",
                            "Search issues without leaving the prefix-scoped records abstraction.", WB_MED, err);
            wb_issue_create(&st->repo, ex_id, "Inspect cross-product audit trail",
                            "Switch to Activity via Ctrl+K or settings to verify wb.activity.", WB_LOW, err);
        }
    }
    if (st->repo.issue_count > 0) select_issue(st, st->repo.issue_count - 1);

    wada_app_command(app, scope, "wb.issue.new", "New issue", "Ctrl N", cmd_new_issue, st);
    wada_app_command(app, scope, "wb.issue.save", "Save issue", "Ctrl S", cmd_save_issue, st);
    wada_app_command(app, scope, "wb.issue.cycle_status", "Cycle issue status", "Ctrl D", cmd_cycle_status, st);
    wada_app_command(app, scope, "wb.issue.delete", "Delete issue", NULL, cmd_delete_issue, st);
    wada_app_command(app, scope, "wb.project.new", "New project", "Ctrl P", cmd_new_project, st);

    wada_bus_subscribe_scoped(wada_app_bus(app), scope, "wb/*", 0, on_wb_changed, st);
    return st;
}

static void wb_issues_leave(void *state) {
    issues_state *st = (issues_state *)state;
    if (!st) return;
    save_current(st, false);
    wb_store_close(&st->repo);
    free(st);
}

static const char *const issues_walk[] = {"wb.issue.new", "wb.issue.delete", "wb.project.new"};
static const char *const issues_prefixes[] = {"proj/", "issue/"};

const wada_product wb_issues_product = {
    "wb.issues",
    "Issues",
    wb_issues_enter,
    wb_issues_leave,
    wb_issues_frame,
    NULL,
    0,
    issues_walk,
    (int)(sizeof issues_walk / sizeof issues_walk[0]),
    issues_prefixes,
    (int)(sizeof issues_prefixes / sizeof issues_prefixes[0]),
};
