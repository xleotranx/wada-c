/* Workbench tests: multi-prefix store CRUD, FTS5 search, multi-subscriber
 * wildcard bus, scoped disposal, and app-lifetime activity service. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "wb_store.h"
#include "wada.h"
#include "wada_app.h"
#include "wada_log.h"
#include "wada_store_sqlite.h"
#include "test_env.h"

static int g_checks = 0;

#define CHECK(expr)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(expr)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);             \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

static void clean_db(const char *path) {
    char buf[256];
    const char *suffix[3] = {"", "-wal", "-shm"};
    for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof buf, "%s%s", path, suffix[i]);
        remove(buf);
    }
}

static void make_dir(const char *dir) {
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

static int test_workbench_store(void) {
    const char *db = "test_workbench.db";
    clean_db(db);
    wada_err err;
    wada_err_clear(&err);
    const wada_store_ops *sqlite = wada_store_sqlite();

    wb_store s;
    CHECK(wb_store_open(&s, sqlite, db, &err) == WADA_OK);
    CHECK(s.project_count == 0 && s.issue_count == 0);

    int p_core = wb_project_create(&s, "Core", &err);
    int p_docs = wb_project_create(&s, "Docs", &err);
    CHECK(p_core == 0 && p_docs == 1);
    char core_id[WB_ID_CAP], docs_id[WB_ID_CAP];
    snprintf(core_id, sizeof core_id, "%s", s.projects[p_core].id);
    snprintf(docs_id, sizeof docs_id, "%s", s.projects[p_docs].id);
    CHECK(wb_project_find(&s, core_id) == 0);

    int i1 = wb_issue_create(&s, core_id, "Kernel bus routing", "Wildcard matching", WB_HIGH, &err);
    int i2 = wb_issue_create(&s, core_id, "Store snapshot", "Backup before upgrade", WB_MED, &err);
    int i3 = wb_issue_create(&s, docs_id, "Update architecture guide", "Mention kernel bus", WB_LOW, &err);
    CHECK(i1 == 0 && i2 == 1 && i3 == 2);

    CHECK(wb_issue_save(&s, i1, "Kernel wildcard bus", "Supports wb/* patterns", WB_DOING, WB_HIGH, &err) == WADA_OK);
    CHECK(wb_issue_save(&s, i3, "Architecture guide", "Cover records search", WB_DONE, WB_LOW, &err) == WADA_OK);

    wb_counts_info all_c = wb_counts(&s, NULL);
    CHECK(all_c.projects == 2 && all_c.total_issues == 3);
    CHECK(all_c.open == 1 && all_c.doing == 1 && all_c.done == 1 && all_c.high == 1);

    wb_counts_info core_c = wb_counts(&s, core_id);
    CHECK(core_c.total_issues == 2 && core_c.doing == 1 && core_c.open == 1);

    /* FTS5 search across title, notes, prefix, project, and status filters. */
    int out[8];
    CHECK(wb_issue_search(&s, NULL, -1, "wildcard", out, 8) == 1 && out[0] == i1);
    CHECK(wb_issue_search(&s, NULL, -1, "kern", out, 8) == 1 && out[0] == i1);
    CHECK(wb_issue_search(&s, NULL, -1, "records", out, 8) == 1 && out[0] == i3);
    CHECK(wb_issue_search(&s, docs_id, -1, "wildcard", out, 8) == 0);
    CHECK(wb_issue_search(&s, core_id, WB_OPEN, "snapshot", out, 8) == 1 && out[0] == i2);
    CHECK(wb_issue_search(&s, core_id, WB_DONE, "snapshot", out, 8) == 0);

    /* Delete updates both memory and FTS5 index. */
    CHECK(wb_issue_remove(&s, i2, &err) == WADA_OK);
    CHECK(s.issue_count == 2);
    CHECK(wb_issue_search(&s, NULL, -1, "snapshot", out, 8) == 0);

    CHECK(wb_activity_append(&s, "wb/manual", "test entry", &err) == WADA_OK);
    CHECK(s.activity_count == 1);
    wb_store_close(&s);

    /* Reopen verifies relational persistence and updated FTS5 index. */
    CHECK(wb_store_open(&s, sqlite, db, &err) == WADA_OK);
    CHECK(s.project_count == 2 && s.issue_count == 2 && s.activity_count == 1);
    CHECK(wb_issue_search(&s, NULL, -1, "wildcard", out, 8) == 1);
    CHECK(strcmp(s.issues[out[0]].title, "Kernel wildcard bus") == 0);
    CHECK(s.issues[out[0]].status == WB_DOING && s.issues[out[0]].priority == WB_HIGH);
    CHECK(wb_activity_clear(&s, &err) == WADA_OK);
    CHECK(s.activity_count == 0);
    wb_store_close(&s);

    /* Fallback scan path when provider lacks FTS5 search. */
    wada_store_ops no_search = *sqlite;
    no_search.search = NULL;
    CHECK(wb_store_open(&s, &no_search, db, &err) == WADA_OK);
    CHECK(wb_issue_search(&s, NULL, -1, "wildcard", out, 8) == 1);
    wb_store_close(&s);

    clean_db(db);
    return 0;
}

static int g_product_hits = 0;

static void on_product_wb(void *user, const char *topic, const void *data, size_t len) {
    (void)user;
    (void)topic;
    (void)data;
    (void)len;
    g_product_hits++;
}

static int test_workbench_setup_and_bus(void) {
    const char *dir = "test_wb_dir";
    const char *db = "test_wb_dir" WADA_PATH_SEP "wada.db";
    make_dir(dir);
    clean_db(db);

    wada_err err;
    wada_err_clear(&err);
    wada_app_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.title = "wb-test";
    desc.width = 400;
    desc.height = 300;
    desc.hidden = true;

    wada_app *app = wada_app_create(&desc, &err);
    CHECK(app != NULL);
    const wada_store_ops *sqlite = wada_store_sqlite();
    CHECK(wada_registry_add(wada_app_registry(app), wada_app_scope(app), "store",
                            WADA_STORE_ABI, sqlite) == WADA_OK);

    CHECK(wb_setup(app, wada_app_scope(app), dir, &err) == WADA_OK);
    const wb_activity_ops *svc = (const wb_activity_ops *)wada_registry_get(
        wada_app_registry(app), "wb.activity", WADA_WB_ACTIVITY_ABI, NULL);
    CHECK(svc != NULL && wada_ops_ok(&svc->hdr, sizeof *svc, WADA_WB_ACTIVITY_ABI));
    CHECK(svc->event_count(svc->ctx) == 0);

    /* Independent product-scoped subscriber on "wb/*" alongside wb_setup's app subscriber. */
    wada_scope *prod_scope = wada_scope_create(wada_app_scope(app), "wb.issues");
    CHECK(prod_scope != NULL);
    g_product_hits = 0;
    CHECK(wada_bus_subscribe_scoped(wada_app_bus(app), prod_scope, "wb/*", 0,
                                    on_product_wb, NULL) != NULL);

    wada_app_emit(app, "wb/issue.created", "New bug", 8);
    wada_app_emit(app, "product/entered", "wb.issues", 10);
    CHECK(g_product_hits == 1);
    CHECK(svc->event_count(svc->ctx) == 2);
    CHECK(strstr(svc->last_summary(svc->ctx), "product/entered") != NULL);

    /* Disposing product scope removes only the product subscriber; app service stays live. */
    wada_scope_destroy(prod_scope);
    wada_app_emit(app, "wb/issue.updated", "Updated bug", 12);
    CHECK(g_product_hits == 1);
    CHECK(svc->event_count(svc->ctx) == 3);

    wada_app_destroy(app);

    /* Reopen store directly to confirm all 3 audit entries persisted. */
    wb_store s;
    CHECK(wb_store_open(&s, sqlite, db, &err) == WADA_OK);
    CHECK(s.activity_count == 3);
    wb_store_close(&s);

    clean_db(db);
    test_remove_dir(dir);
    return 0;
}

int main(void) {
    wada_log_set_level(WADA_LOG_WARN);
    test_daily_backup(false);
    if (test_workbench_store() || test_workbench_setup_and_bus()) {
        fprintf(stderr, "workbench tests: FAILED after %d checks\n", g_checks);
        return 1;
    }
    printf("workbench tests: %d checks passed\n", g_checks);
    return 0;
}
