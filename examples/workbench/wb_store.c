#include "wb_store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wada_log.h"
#include "wada_ui.h"

static bool grow_array(void **ptr, int *cap, size_t elem_size) {
    int next = *cap ? (*cap) * 2 : 16;
    void *v = realloc(*ptr, (size_t)next * elem_size);
    if (!v) return false;
    *ptr = v;
    *cap = next;
    return true;
}

static bool contains_ci(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return true;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

typedef struct load_ctx {
    wb_store *s;
    bool oom;
} load_ctx;

static bool visit_project(const char *id, wada_slice value, void *user) {
    load_ctx *ctx = (load_ctx *)user;
    wb_project p;
    memset(&p, 0, sizeof p);
    snprintf(p.id, sizeof p.id, "%s", id);

    const char *cur = (const char *)value.p;
    const char *end = cur + value.n;
    char line[64];
    if (wada_records_next_line(&cur, end, line, sizeof line) && strcmp(line, "v1") == 0) {
        if (wada_records_next_line(&cur, end, line, sizeof line)) p.created = atoll(line);
        wada_records_next_line(&cur, end, p.name, sizeof p.name);
    }
    if (ctx->s->project_count == ctx->s->project_cap &&
        !grow_array((void **)&ctx->s->projects, &ctx->s->project_cap, sizeof(wb_project))) {
        ctx->oom = true;
        return false;
    }
    ctx->s->projects[ctx->s->project_count++] = p;
    return true;
}

static bool visit_issue(const char *id, wada_slice value, void *user) {
    load_ctx *ctx = (load_ctx *)user;
    wb_issue it;
    memset(&it, 0, sizeof it);
    snprintf(it.id, sizeof it.id, "%s", id);

    const char *cur = (const char *)value.p;
    const char *end = cur + value.n;
    char line[64];
    if (wada_records_next_line(&cur, end, line, sizeof line) && strcmp(line, "v1") == 0) {
        wada_records_next_line(&cur, end, it.project_id, sizeof it.project_id);
        if (wada_records_next_line(&cur, end, line, sizeof line)) it.status = atoi(line);
        if (wada_records_next_line(&cur, end, line, sizeof line)) it.priority = atoi(line);
        if (wada_records_next_line(&cur, end, line, sizeof line)) it.updated = atoll(line);
        wada_records_next_line(&cur, end, it.title, sizeof it.title);
        if (cur < end) {
            size_t len = (size_t)(end - cur);
            if (len >= sizeof it.notes) len = sizeof it.notes - 1;
            memcpy(it.notes, cur, len);
            it.notes[len] = '\0';
        }
    }
    if (it.status < WB_OPEN || it.status > WB_DONE) it.status = WB_OPEN;
    if (it.priority < WB_LOW || it.priority > WB_HIGH) it.priority = WB_MED;
    if (ctx->s->issue_count == ctx->s->issue_cap &&
        !grow_array((void **)&ctx->s->issues, &ctx->s->issue_cap, sizeof(wb_issue))) {
        ctx->oom = true;
        return false;
    }
    ctx->s->issues[ctx->s->issue_count++] = it;
    return true;
}

static bool visit_activity(const char *id, wada_slice value, void *user) {
    load_ctx *ctx = (load_ctx *)user;
    wb_activity a;
    memset(&a, 0, sizeof a);
    snprintf(a.id, sizeof a.id, "%s", id);

    const char *cur = (const char *)value.p;
    const char *end = cur + value.n;
    char line[64];
    if (wada_records_next_line(&cur, end, line, sizeof line) && strcmp(line, "v1") == 0) {
        if (wada_records_next_line(&cur, end, line, sizeof line)) a.when = atoll(line);
        wada_records_next_line(&cur, end, a.topic, sizeof a.topic);
        wada_records_next_line(&cur, end, a.summary, sizeof a.summary);
    }
    if (ctx->s->activity_count == ctx->s->activity_cap &&
        !grow_array((void **)&ctx->s->activities, &ctx->s->activity_cap, sizeof(wb_activity))) {
        ctx->oom = true;
        return false;
    }
    ctx->s->activities[ctx->s->activity_count++] = a;
    return true;
}

wada_status wb_store_open(wb_store *s, const wada_store_ops *ops, const char *url, wada_err *err) {
    if (!s || !ops || !url) return wada_err_set(err, WADA_ERR_INVALID, "wb_store_open");
    memset(s, 0, sizeof *s);

    wada_status st = wada_records_open(&s->projects_rec, ops, url, "proj/", err);
    if (st != WADA_OK) return st;
    st = wada_records_open(&s->issues_rec, ops, url, "issue/", err);
    if (st != WADA_OK) {
        wb_store_close(s);
        return st;
    }
    st = wada_records_open(&s->activity_rec, ops, url, "act/", err);
    if (st != WADA_OK) {
        wb_store_close(s);
        return st;
    }

    load_ctx ctx = {s, false};
    st = wada_records_each(&s->projects_rec, visit_project, &ctx, err);
    if (st == WADA_OK && !ctx.oom) st = wada_records_each(&s->issues_rec, visit_issue, &ctx, err);
    if (st == WADA_OK && !ctx.oom) st = wada_records_each(&s->activity_rec, visit_activity, &ctx, err);
    if (ctx.oom) st = wada_err_set(err, WADA_ERR_NOMEM, "wb_store_open");
    if (st != WADA_OK) wb_store_close(s);
    return st;
}

void wb_store_close(wb_store *s) {
    if (!s) return;
    wada_records_close(&s->projects_rec);
    wada_records_close(&s->issues_rec);
    wada_records_close(&s->activity_rec);
    free(s->projects);
    free(s->issues);
    free(s->activities);
    memset(s, 0, sizeof *s);
}

wada_status wb_store_reload_activity(wb_store *s, wada_err *err) {
    if (!s) return wada_err_set(err, WADA_ERR_INVALID, "wb_store_reload_activity");
    s->activity_count = 0;
    load_ctx ctx = {s, false};
    wada_status st = wada_records_each(&s->activity_rec, visit_activity, &ctx, err);
    if (ctx.oom) st = wada_err_set(err, WADA_ERR_NOMEM, "wb_store_reload_activity");
    return st;
}

int wb_project_create(wb_store *s, const char *name, wada_err *err) {
    if (!s || !name || !name[0]) {
        wada_err_set(err, WADA_ERR_INVALID, "project name required");
        return -1;
    }
    wb_project p;
    memset(&p, 0, sizeof p);
    wada_records_fresh_id(&s->projects_rec, p.id, sizeof p.id);
    p.created = (long long)time(NULL);
    wada_records_sanitize_line(name, p.name, sizeof p.name);

    char buf[WB_NAME_CAP + 64];
    int len = snprintf(buf, sizeof buf, "v1\n%lld\n%s", p.created, p.name);
    if (len < 0 || (size_t)len >= sizeof buf) {
        wada_err_set(err, WADA_ERR, "encode project");
        return -1;
    }
    if (wada_records_put(&s->projects_rec, p.id, buf, (size_t)len, err) != WADA_OK) return -1;
    if (s->project_count == s->project_cap &&
        !grow_array((void **)&s->projects, &s->project_cap, sizeof(wb_project))) {
        wada_err_set(err, WADA_ERR_NOMEM, "wb_project_create");
        return -1;
    }
    s->projects[s->project_count++] = p;
    return s->project_count - 1;
}

int wb_project_find(const wb_store *s, const char *id) {
    if (!s || !id || !id[0]) return -1;
    for (int i = 0; i < s->project_count; i++) {
        if (strcmp(s->projects[i].id, id) == 0) return i;
    }
    return -1;
}

static wada_status write_issue(wb_store *s, const wb_issue *it, wada_err *err) {
    size_t notes_len = strlen(it->notes);
    size_t need = 160 + strlen(it->title) + notes_len + 1;
    char *buf = (char *)malloc(need);
    if (!buf) return wada_err_set(err, WADA_ERR_NOMEM, "encode issue");

    int head = snprintf(buf, need, "v1\n%s\n%d\n%d\n%lld\n%s\n",
                        it->project_id, it->status, it->priority, it->updated, it->title);
    if (head < 0 || (size_t)head + notes_len >= need) {
        free(buf);
        return wada_err_set(err, WADA_ERR, "encode issue");
    }
    memcpy(buf + head, it->notes, notes_len);
    wada_status st = wada_records_put(&s->issues_rec, it->id, buf, (size_t)head + notes_len, err);
    free(buf);
    return st;
}

int wb_issue_create(wb_store *s, const char *project_id, const char *title, const char *notes,
                    int priority, wada_err *err) {
    if (!s) return -1;
    wb_issue it;
    memset(&it, 0, sizeof it);
    wada_records_fresh_id(&s->issues_rec, it.id, sizeof it.id);
    wada_records_sanitize_line(project_id ? project_id : "", it.project_id, sizeof it.project_id);
    wada_records_sanitize_line(title && title[0] ? title : "Untitled issue", it.title, sizeof it.title);
    snprintf(it.notes, sizeof it.notes, "%s", notes ? notes : "");
    it.status = WB_OPEN;
    it.priority = (priority >= WB_LOW && priority <= WB_HIGH) ? priority : WB_MED;
    it.updated = (long long)time(NULL);

    if (write_issue(s, &it, err) != WADA_OK) return -1;
    if (s->issue_count == s->issue_cap &&
        !grow_array((void **)&s->issues, &s->issue_cap, sizeof(wb_issue))) {
        wada_err_set(err, WADA_ERR_NOMEM, "wb_issue_create");
        return -1;
    }
    s->issues[s->issue_count++] = it;
    return s->issue_count - 1;
}

wada_status wb_issue_save(wb_store *s, int index, const char *title, const char *notes,
                          int status, int priority, wada_err *err) {
    if (!s || index < 0 || index >= s->issue_count) {
        return wada_err_set(err, WADA_ERR_NOT_FOUND, "issue %d", index);
    }
    wb_issue updated = s->issues[index];
    wada_records_sanitize_line(title && title[0] ? title : "Untitled issue",
                               updated.title, sizeof updated.title);
    snprintf(updated.notes, sizeof updated.notes, "%s", notes ? notes : "");
    updated.status = (status >= WB_OPEN && status <= WB_DONE) ? status : updated.status;
    updated.priority = (priority >= WB_LOW && priority <= WB_HIGH) ? priority : updated.priority;
    updated.updated = (long long)time(NULL);

    wada_status st = write_issue(s, &updated, err);
    if (st == WADA_OK) s->issues[index] = updated;
    return st;
}

wada_status wb_issue_remove(wb_store *s, int index, wada_err *err) {
    if (!s || index < 0 || index >= s->issue_count) {
        return wada_err_set(err, WADA_ERR_NOT_FOUND, "issue %d", index);
    }
    wada_status st = wada_records_del(&s->issues_rec, s->issues[index].id, err);
    if (st != WADA_OK) return st;
    for (int i = index; i + 1 < s->issue_count; i++) s->issues[i] = s->issues[i + 1];
    s->issue_count--;
    return WADA_OK;
}

int wb_issue_find(const wb_store *s, const char *id) {
    if (!s || !id) return -1;
    for (int i = 0; i < s->issue_count; i++) {
        if (strcmp(s->issues[i].id, id) == 0) return i;
    }
    return -1;
}

static bool issue_matches_filters(const wb_issue *it, const char *project_id, int status_filter) {
    if (project_id && project_id[0] && strcmp(it->project_id, project_id) != 0) return false;
    if (status_filter >= 0 && it->status != status_filter) return false;
    return true;
}

typedef struct search_ctx {
    const wb_store *s;
    const char *project_id;
    int status_filter;
    int *out;
    int cap;
    int count;
} search_ctx;

static bool search_visitor(const char *id, wada_slice value, void *user) {
    (void)value;
    search_ctx *ctx = (search_ctx *)user;
    int idx = wb_issue_find(ctx->s, id);
    if (idx < 0) return true;
    if (!issue_matches_filters(&ctx->s->issues[idx], ctx->project_id, ctx->status_filter)) {
        return true;
    }
    if (ctx->count < ctx->cap) ctx->out[ctx->count++] = idx;
    return ctx->count < ctx->cap;
}

int wb_issue_search(wb_store *s, const char *project_id, int status_filter,
                    const char *query, int *out, int cap) {
    if (!s || !out || cap <= 0) return 0;

    if (!query || !query[0]) {
        int n = 0;
        for (int i = s->issue_count - 1; i >= 0 && n < cap; i--) {
            if (issue_matches_filters(&s->issues[i], project_id, status_filter)) out[n++] = i;
        }
        return n;
    }

    search_ctx ctx = {s, project_id, status_filter, out, cap, 0};
    wada_err err;
    wada_err_clear(&err);
    wada_status st = wada_records_search(&s->issues_rec, query, (uint32_t)cap * 2u,
                                         search_visitor, &ctx, &err);
    if (st == WADA_OK) return ctx.count;
    if (st != WADA_ERR_UNSUPPORTED) return 0;

    int n = 0;
    for (int i = s->issue_count - 1; i >= 0 && n < cap; i--) {
        const wb_issue *it = &s->issues[i];
        if (!issue_matches_filters(it, project_id, status_filter)) continue;
        if (contains_ci(it->title, query) || contains_ci(it->notes, query)) out[n++] = i;
    }
    return n;
}

static wada_status write_activity_rec(wada_records *rec, const char *topic, const char *summary,
                                      wb_activity *out_rec, wada_err *err) {
    wb_activity a;
    memset(&a, 0, sizeof a);
    wada_records_fresh_id(rec, a.id, sizeof a.id);
    a.when = (long long)time(NULL);
    wada_records_sanitize_line(topic ? topic : "", a.topic, sizeof a.topic);
    wada_records_sanitize_line(summary ? summary : "", a.summary, sizeof a.summary);

    char buf[WB_TOPIC_CAP + WB_SUMMARY_CAP + 64];
    int len = snprintf(buf, sizeof buf, "v1\n%lld\n%s\n%s", a.when, a.topic, a.summary);
    if (len < 0 || (size_t)len >= sizeof buf) {
        return wada_err_set(err, WADA_ERR, "encode activity");
    }
    wada_status st = wada_records_put(rec, a.id, buf, (size_t)len, err);
    if (st == WADA_OK && out_rec) *out_rec = a;
    return st;
}

wada_status wb_activity_append(wb_store *s, const char *topic, const char *summary, wada_err *err) {
    if (!s) return wada_err_set(err, WADA_ERR_INVALID, "wb_activity_append");
    wb_activity a;
    wada_status st = write_activity_rec(&s->activity_rec, topic, summary, &a, err);
    if (st != WADA_OK) return st;
    if (s->activity_count == s->activity_cap &&
        !grow_array((void **)&s->activities, &s->activity_cap, sizeof(wb_activity))) {
        return wada_err_set(err, WADA_ERR_NOMEM, "wb_activity_append");
    }
    s->activities[s->activity_count++] = a;
    return WADA_OK;
}

wada_status wb_activity_clear(wb_store *s, wada_err *err) {
    if (!s) return wada_err_set(err, WADA_ERR_INVALID, "wb_activity_clear");
    for (int i = 0; i < s->activity_count; i++) {
        wada_status st = wada_records_del(&s->activity_rec, s->activities[i].id, err);
        if (st != WADA_OK) return st;
    }
    s->activity_count = 0;
    return WADA_OK;
}

wb_counts_info wb_counts(const wb_store *s, const char *project_id) {
    wb_counts_info c;
    memset(&c, 0, sizeof c);
    if (!s) return c;
    c.projects = s->project_count;
    c.activities = s->activity_count;
    for (int i = 0; i < s->issue_count; i++) {
        const wb_issue *it = &s->issues[i];
        if (project_id && project_id[0] && strcmp(it->project_id, project_id) != 0) continue;
        c.total_issues++;
        if (it->status == WB_OPEN) c.open++;
        else if (it->status == WB_DOING) c.doing++;
        else c.done++;
        if (it->priority == WB_HIGH) c.high++;
    }
    return c;
}

const char *wb_status_name(int status) {
    switch (status) {
        case WB_OPEN: return "Open";
        case WB_DOING: return "Doing";
        case WB_DONE: return "Done";
        default: return "Open";
    }
}

const char *wb_priority_name(int priority) {
    switch (priority) {
        case WB_LOW: return "Low";
        case WB_MED: return "Medium";
        case WB_HIGH: return "High";
        default: return "Medium";
    }
}

/* ---- App-lifetime activity service ------------------------------------- */

typedef struct wb_act_service {
    wada_records rec;
    int events;
    char last[WB_SUMMARY_CAP];
    wb_activity_ops ops;
} wb_act_service;

static int svc_event_count(void *ctx) {
    return ctx ? ((wb_act_service *)ctx)->events : 0;
}

static const char *svc_last_summary(void *ctx) {
    return ctx ? ((wb_act_service *)ctx)->last : "";
}

static void svc_dispose(void *user) {
    wb_act_service *svc = (wb_act_service *)user;
    if (!svc) return;
    wada_records_close(&svc->rec);
    free(svc);
}

static void on_app_event(void *user, const char *topic, const void *data, size_t len) {
    wb_act_service *svc = (wb_act_service *)user;
    if (!svc) return;
    char summary[WB_SUMMARY_CAP];
    if (data && len > 0) {
        size_t n = len < sizeof summary - 1 ? len : sizeof summary - 1;
        memcpy(summary, data, n);
        summary[n] = '\0';
    } else {
        snprintf(summary, sizeof summary, "%s", topic ? topic : "");
    }
    wada_err err;
    wada_err_clear(&err);
    if (write_activity_rec(&svc->rec, topic, summary, NULL, &err) != WADA_OK) {
        WADA_LOGW("wb", "audit write failed: %s", wada_err_msg(&err));
    }
    svc->events++;
    snprintf(svc->last, sizeof svc->last, "%s: %s", topic ? topic : "", summary);
}

wada_status wb_setup(wada_app *app, wada_scope *app_scope, const char *data_dir, wada_err *err) {
    if (!app || !app_scope) return wada_err_set(err, WADA_ERR_INVALID, "wb_setup");
    const wada_store_ops *store = wada_ui_store_ops(app, "wb_setup", err);
    if (!store) return err ? (wada_status)err->code : WADA_ERR_NOT_FOUND;

    wb_act_service *svc = (wb_act_service *)calloc(1, sizeof *svc);
    if (!svc) return wada_err_set(err, WADA_ERR_NOMEM, "wb_setup");

    char path[640];
    wada_ui_db_path(data_dir, path, sizeof path);
    wada_status st = wada_records_open(&svc->rec, store, path, "act/", err);
    if (st != WADA_OK) {
        free(svc);
        return st;
    }

    svc->ops.hdr.size = sizeof(wb_activity_ops);
    svc->ops.hdr.version = WADA_WB_ACTIVITY_ABI;
    svc->ops.event_count = svc_event_count;
    svc->ops.last_summary = svc_last_summary;
    svc->ops.ctx = svc;

    if (wada_scope_own(app_scope, svc_dispose, svc) == 0) {
        svc_dispose(svc);
        return wada_err_set(err, WADA_ERR_STATE, "wb_setup: scope dead");
    }

    st = wada_registry_add(wada_app_registry(app), app_scope, "wb.activity",
                           WADA_WB_ACTIVITY_ABI, &svc->ops);
    if (st != WADA_OK) {
        wada_err_set(err, st, "wb_setup: register wb.activity");
        return st;
    }

    if (!wada_bus_subscribe_scoped(wada_app_bus(app), app_scope, "wb/*", 0, on_app_event, svc) ||
        !wada_bus_subscribe_scoped(wada_app_bus(app), app_scope, "product/entered", 0, on_app_event, svc)) {
        return wada_err_set(err, WADA_ERR_NOMEM, "wb_setup: bus subscribe");
    }
    return WADA_OK;
}
