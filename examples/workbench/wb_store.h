/* Workbench store and app-lifetime activity service.
 *
 * Manages three prefix-scoped collections in one SQLite store ("proj/",
 * "issue/", "act/") via wada_records, plus a versioned "wb.activity" service
 * installed through wada_boot_desc.setup that listens to "wb/*" and
 * "product/entered" across product switches.
 */
#ifndef WB_STORE_H
#define WB_STORE_H

#include "wada.h"
#include "wada_app.h"
#include "wada_iface.h"
#include "wada_records.h"

#define WB_ID_CAP 48
#define WB_NAME_CAP 64
#define WB_TITLE_CAP 128
#define WB_NOTES_CAP 512
#define WB_TOPIC_CAP 48
#define WB_SUMMARY_CAP 160

enum {
    WB_OPEN = 0,
    WB_DOING = 1,
    WB_DONE = 2
};

enum {
    WB_LOW = 0,
    WB_MED = 1,
    WB_HIGH = 2
};

typedef struct wb_project {
    char id[WB_ID_CAP];
    long long created;
    char name[WB_NAME_CAP];
} wb_project;

typedef struct wb_issue {
    char id[WB_ID_CAP];
    char project_id[WB_ID_CAP];
    int status;
    int priority;
    long long updated;
    char title[WB_TITLE_CAP];
    char notes[WB_NOTES_CAP];
} wb_issue;

typedef struct wb_activity {
    char id[WB_ID_CAP];
    long long when;
    char topic[WB_TOPIC_CAP];
    char summary[WB_SUMMARY_CAP];
} wb_activity;

typedef struct wb_counts_info {
    int projects;
    int total_issues;
    int open;
    int doing;
    int done;
    int high;
    int activities;
} wb_counts_info;

typedef struct wb_store {
    wada_records projects_rec;
    wada_records issues_rec;
    wada_records activity_rec;
    wb_project *projects;
    int project_count;
    int project_cap;
    wb_issue *issues;
    int issue_count;
    int issue_cap;
    wb_activity *activities;
    int activity_count;
    int activity_cap;
} wb_store;

#define WADA_WB_ACTIVITY_ABI 1u

typedef struct wb_activity_ops {
    wada_ops_header hdr; /* sizeof(wb_activity_ops), WADA_WB_ACTIVITY_ABI */
    int (*event_count)(void *ctx);
    const char *(*last_summary)(void *ctx);
    void *ctx;
} wb_activity_ops;

wada_status wb_store_open(wb_store *s, const wada_store_ops *ops, const char *url, wada_err *err);
void wb_store_close(wb_store *s);
wada_status wb_store_reload_activity(wb_store *s, wada_err *err);

int wb_project_create(wb_store *s, const char *name, wada_err *err);
int wb_project_find(const wb_store *s, const char *id);

int wb_issue_create(wb_store *s, const char *project_id, const char *title, const char *notes,
                    int priority, wada_err *err);
wada_status wb_issue_save(wb_store *s, int index, const char *title, const char *notes,
                          int status, int priority, wada_err *err);
wada_status wb_issue_remove(wb_store *s, int index, wada_err *err);
int wb_issue_find(const wb_store *s, const char *id);

/* FTS5 search via wada_records_search, filtered by optional project_id (NULL/""
 * for all) and status_filter (< 0 for all). Falls back to scan on
 * WADA_ERR_UNSUPPORTED. */
int wb_issue_search(wb_store *s, const char *project_id, int status_filter,
                    const char *query, int *out, int cap);

wada_status wb_activity_append(wb_store *s, const char *topic, const char *summary, wada_err *err);
wada_status wb_activity_clear(wb_store *s, wada_err *err);

wb_counts_info wb_counts(const wb_store *s, const char *project_id);
const char *wb_status_name(int status);
const char *wb_priority_name(int priority);

/* App-lifetime setup hook compatible with wada_boot_desc.setup. */
wada_status wb_setup(wada_app *app, wada_scope *app_scope, const char *data_dir, wada_err *err);

#endif /* WB_STORE_H */
