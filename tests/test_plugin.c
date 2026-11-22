/* Provider module tests.
 *
 * Loads the real wada_store_plugin (built as a shared library), checks the
 * handshake, and uses the vtable it hands back to write, read, search and
 * close a database. Then it loads a module with a wrong ABI and a path that
 * does not exist, and asserts both are refused with an error, not a crash.
 *
 * Paths may be passed as arguments (CTest) or found next to the executable
 * (build.ps1 runs the tests with no arguments).
 */
#include <stdio.h>
#include <string.h>

#include "wada.h"
#include "wada_iface.h"
#include "wada_log.h"
#include "wada_module.h"
#include "test_env.h"

#if defined(_WIN32)
#define PLUGIN_NAME "wada_store_plugin.dll"
#define BAD_NAME "wada_bad_plugin.dll"
#else
#define PLUGIN_NAME "wada_store_plugin.so"
#define BAD_NAME "wada_bad_plugin.so"
#endif

static int g_checks = 0;

#define CHECK(expr)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(expr)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);             \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

int main(int argc, char **argv) {
    test_daily_backup(false); /* this test is not about backups */
    wada_log_set_level(WADA_LOG_WARN);
    const char *good = argc > 1 ? argv[1] : PLUGIN_NAME;
    const char *bad = argc > 2 ? argv[2] : BAD_NAME;

    wada_err err;
    wada_err_clear(&err);
    wada_module *module = wada_module_load(good, &err);
    if (!module) {
        fprintf(stderr, "plugin tests: cannot load %s: %s\n", good, wada_err_msg(&err));
        return 1;
    }

    const wada_plugin *plugin = wada_module_plugin(module);
    CHECK(plugin != NULL);
    CHECK(plugin->hdr.version == WADA_PLUGIN_ABI);
    CHECK(plugin->hdr.size >= sizeof(wada_plugin));
    CHECK(plugin->store_abi == WADA_STORE_ABI);
    CHECK(strcmp(wada_module_id(module), "sqlite") == 0);
    CHECK(wada_module_store(module) == plugin->store);

    const wada_store_ops *ops = wada_module_store(module);
    CHECK(wada_ops_ok(&ops->hdr, sizeof(wada_store_ops), WADA_STORE_ABI));

    /* The vtable from inside the module behaves like the linked one. */
    wada_store *db = NULL;
    CHECK(ops->open("test_plugin.db", &db, &err) == WADA_OK);
    CHECK(ops->schema_version(db) == 2u);
    CHECK((ops->caps(db) & WADA_STORE_CAP_SEARCH) != 0);

    wada_txn *txn = NULL;
    CHECK(ops->begin(db, WADA_STORE_WRITE, &txn, &err) == WADA_OK);
    wada_slice key = {(const uint8_t *)"note/1", 6};
    wada_slice value = {(const uint8_t *)"hello from a module", 19};
    CHECK(ops->put(txn, key, value, &err) == WADA_OK);
    CHECK(ops->commit(txn, &err) == WADA_OK);

    CHECK(ops->begin(db, WADA_STORE_READ, &txn, &err) == WADA_OK);
    wada_slice read;
    CHECK(ops->get(txn, key, &read, &err) == WADA_OK);
    CHECK(read.n == value.n && memcmp(read.p, value.p, read.n) == 0);
    ops->rollback(txn);

    CHECK(ops->begin(db, WADA_STORE_READ, &txn, &err) == WADA_OK);
    wada_iter *it = NULL;
    wada_slice prefix = {(const uint8_t *)"note/", 5};
    CHECK(ops->search(txn, prefix, "module", 10, &it, &err) == WADA_OK);
    wada_slice skey, sval;
    CHECK(ops->next(it, &skey, &sval, &err) == WADA_OK);
    CHECK(skey.n == key.n && memcmp(skey.p, key.p, key.n) == 0);
    ops->iter_close(it);
    ops->rollback(txn);
    ops->close(db);

    remove("test_plugin.db");
    remove("test_plugin.db-wal");
    remove("test_plugin.db-shm");

    /* A module that reports another ABI is refused, and nothing is used. */
    wada_err bad_err;
    wada_err_clear(&bad_err);
    wada_module *refused = wada_module_load(bad, &bad_err);
    CHECK(refused == NULL);
    CHECK(bad_err.code == WADA_ERR_ABI);
    CHECK(strstr(wada_err_msg(&bad_err), "ABI") != NULL);

    /* A missing file is an io error with the path in the message. */
    wada_err missing;
    wada_err_clear(&missing);
    CHECK(wada_module_load("wada_no_such_provider.dll", &missing) == NULL);
    CHECK(missing.code == WADA_ERR_IO);
    CHECK(strstr(wada_err_msg(&missing), "wada_no_such_provider.dll") != NULL);

    wada_module_unload(module);
    printf("plugin tests: %d checks passed\n", g_checks);
    return 0;
}
