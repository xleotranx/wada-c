/* One process per data directory.
 *
 * Two instances of a Wada app share one SQLite file: writes are serialized by
 * the database, but each process caches lists and product state, so the second
 * window shows stale data and the first never learns about the change. The
 * cheap, honest answer is a lock file: the first process owns
 * "<data-dir>/wada.lock" by holding it open with no sharing, the second exits
 * with an explanation instead of quietly forking the user's data.
 *
 * The lock is advisory and per data directory, not per executable: running the
 * shipped app and an example against the same folder is refused too, because
 * they would share the same records. `--doctor` does not take the lock: it is a
 * short, bounded probe and deliberately must run while the app is open.
 *
 * The lock is released when the process exits, including a crash, because the
 * OS closes the handle. Nothing has to clean up a stale file.
 */
#ifndef WADA_INSTANCE_H
#define WADA_INSTANCE_H

#include "wada.h"

typedef struct wada_instance wada_instance;

/* Returns NULL when the directory is already claimed (WADA_ERR_STATE) or the
 * lock file cannot be created (WADA_ERR_IO). */
WADA_API wada_instance *wada_instance_acquire(const char *data_dir, wada_err *err);
WADA_API void wada_instance_release(wada_instance *lock);
WADA_API long wada_instance_pid(const wada_instance *lock);

#endif /* WADA_INSTANCE_H */
