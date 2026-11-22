/* Instance lock tests: one process per data directory. */
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#define REMOVE_DIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MAKE_DIR(p) mkdir((p), 0755)
#define REMOVE_DIR(p) rmdir(p)
#endif

#include "wada.h"
#include "wada_instance.h"

static int g_checks = 0;

#define CHECK(expr)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(expr)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);             \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

int main(void) {
    const char *dir = "test_instance_data";
    MAKE_DIR(dir);
    remove("test_instance_data/wada.lock");
    remove("test_instance_data\\wada.lock");

    wada_err err;
    wada_err_clear(&err);
    wada_instance *first = wada_instance_acquire(dir, &err);
    if (!first) {
        fprintf(stderr, "instance tests: cannot acquire: %s\n", wada_err_msg(&err));
        return 1;
    }
    CHECK(wada_instance_pid(first) > 0);

    /* The second acquisition in the same process fails the same way a second
     * process would: the handle is open without sharing. */
    wada_err second_err;
    wada_err_clear(&second_err);
    wada_instance *second = wada_instance_acquire(dir, &second_err);
    CHECK(second == NULL);
    CHECK(second_err.code == WADA_ERR_STATE);
    CHECK(strstr(wada_err_msg(&second_err), "already using") != NULL);

    /* Release, reacquire: a crashed process leaves no stale state behind. */
    wada_instance_release(first);
    wada_err_clear(&err);
    first = wada_instance_acquire(dir, &err);
    CHECK(first != NULL);
    wada_instance_release(first);

    /* A directory that does not exist is an IO error, not a lock conflict. */
    wada_err missing;
    wada_err_clear(&missing);
    CHECK(wada_instance_acquire("test_no_such_dir_xyz", &missing) == NULL);
    CHECK(missing.code == WADA_ERR_IO);

    remove("test_instance_data/wada.lock");
    remove("test_instance_data\\wada.lock");
    REMOVE_DIR(dir);

    printf("instance tests: %d checks passed\n", g_checks);
    return 0;
}
