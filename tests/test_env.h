/* Small helper for tests that are not about backups.
 *
 * A test that opens a file database in the repository directory would make the
 * provider write a daily snapshot next to it. Turning the daily snapshot off
 * keeps the tree clean; the pre-migration snapshot is never disabled, and
 * tests/test_store.c exercises the daily path on purpose.
 */
#ifndef TEST_ENV_H
#define TEST_ENV_H

#include <stdlib.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

static void test_daily_backup(bool enabled) {
#if defined(_WIN32)
    _putenv(enabled ? (char *)"WADA_NO_DAILY_BACKUP=0" : (char *)"WADA_NO_DAILY_BACKUP=1");
#else
    setenv("WADA_NO_DAILY_BACKUP", enabled ? "0" : "1", 1);
#endif
}

static void test_remove_dir(const char *path) {
#if defined(_WIN32)
    _rmdir(path);
#else
    rmdir(path);
#endif
}

#endif /* TEST_ENV_H */
