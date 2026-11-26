#include "wada_store_sqlite.h"

#include <sqlite3.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <wchar.h>
static bool utf8_to_wide_path(const char *path, WCHAR *out, int cap) {
    if (!path || !out || cap <= 0) return false;
    return MultiByteToWideChar(CP_UTF8, 0, path, -1, out, cap) > 0;
}
#endif
/* ABI 2: kv stays the source of truth; kv_fts mirrors text values for search;
 * kv_fts_map remembers the FTS rowid for each key so an update can replace the
 * entry without scanning the index. The schema is versioned and migrated at
 * open (v1 databases gain the index and are marked v2). */

#define WADA_SQLITE_SCHEMA 2u

struct wada_store {
    sqlite3 *db;
    uint32_t schema;
    char backup_path[800]; /* "" when this store has no backup */
    sqlite3_stmt *stmt_begin_read;
    sqlite3_stmt *stmt_begin_write;
    sqlite3_stmt *stmt_commit;
    sqlite3_stmt *stmt_rollback;
    sqlite3_stmt *stmt_get;
    sqlite3_stmt *stmt_put;
    sqlite3_stmt *stmt_del;
    sqlite3_stmt *stmt_fts_find;
    sqlite3_stmt *stmt_fts_del_fts;
    sqlite3_stmt *stmt_fts_del_map;
    sqlite3_stmt *stmt_fts_ins_fts;
    sqlite3_stmt *stmt_fts_ins_map;
};

struct wada_txn {
    struct wada_store *store;
    sqlite3_stmt *last; /* borrowed cached stmt; reset in txn_drop_last */
};

struct wada_iter {
    sqlite3_stmt *stmt;
    bool done;
};

static wada_status err_from_db(sqlite3 *db, int rc, wada_err *err) {
    const char *msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    switch (rc) {
    case SQLITE_CONSTRAINT:
        return wada_err_set(err, WADA_ERR_EXISTS, "%s", msg);
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return wada_err_set(err, WADA_ERR_STATE, "%s", msg);
    case SQLITE_NOTADB:
    case SQLITE_CORRUPT:
    case SQLITE_IOERR:
    case SQLITE_CANTOPEN:
        return wada_err_set(err, WADA_ERR_IO, "%s", msg);
    default:
        return wada_err_set(err, WADA_ERR, "%s", msg);
    }
}

static wada_status exec_sql(sqlite3 *db, const char *sql, wada_err *err) {
    char *msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &msg);
    if (rc != SQLITE_OK) {
        wada_err_set(err, WADA_ERR_IO, "%s", msg ? msg : sqlite3_errmsg(db));
        sqlite3_free(msg);
        return WADA_ERR_IO;
    }
    sqlite3_free(msg);
    return WADA_OK;
}

static wada_status prepare(sqlite3 *db, const char *sql, sqlite3_stmt **out, wada_err *err) {
    int rc = sqlite3_prepare_v2(db, sql, -1, out, NULL);
    if (rc != SQLITE_OK) return err_from_db(db, rc, err);
    return WADA_OK;
}

static wada_status cached_stmt(sqlite3 *db, sqlite3_stmt **slot, const char *sql, wada_err *err) {
    if (!*slot) {
        int rc = sqlite3_prepare_v2(db, sql, -1, slot, NULL);
        if (rc != SQLITE_OK) return err_from_db(db, rc, err);
    } else {
        sqlite3_reset(*slot);
    }
    return WADA_OK;
}

static void finalize_cached_stmts(struct wada_store *s) {
    sqlite3_stmt **slots[] = {
        &s->stmt_begin_read, &s->stmt_begin_write, &s->stmt_commit, &s->stmt_rollback,
        &s->stmt_get, &s->stmt_put, &s->stmt_del,
        &s->stmt_fts_find, &s->stmt_fts_del_fts, &s->stmt_fts_del_map,
        &s->stmt_fts_ins_fts, &s->stmt_fts_ins_map
    };
    for (size_t i = 0; i < sizeof slots / sizeof slots[0]; i++) {
        if (*slots[i]) {
            sqlite3_finalize(*slots[i]);
            *slots[i] = NULL;
        }
    }
}

static void txn_drop_last(struct wada_txn *txn) {
    if (txn->last) {
        sqlite3_reset(txn->last);
        txn->last = NULL;
    }
}

static wada_status index_remove(struct wada_store *s, wada_slice key, wada_err *err) {
    if (cached_stmt(s->db, &s->stmt_fts_find, "SELECT rid FROM kv_fts_map WHERE k = ?1", err) != WADA_OK) {
        return WADA_ERR;
    }
    sqlite3_bind_blob(s->stmt_fts_find, 1, key.p, (int)key.n, SQLITE_STATIC);

    bool found = false;
    int64_t rid = 0;
    if (sqlite3_step(s->stmt_fts_find) == SQLITE_ROW) {
        rid = sqlite3_column_int64(s->stmt_fts_find, 0);
        found = true;
    }
    sqlite3_reset(s->stmt_fts_find);
    if (!found) return WADA_OK;

    if (cached_stmt(s->db, &s->stmt_fts_del_fts, "DELETE FROM kv_fts WHERE rowid = ?1", err) == WADA_OK) {
        sqlite3_bind_int64(s->stmt_fts_del_fts, 1, rid);
        sqlite3_step(s->stmt_fts_del_fts);
        sqlite3_reset(s->stmt_fts_del_fts);
    }
    if (cached_stmt(s->db, &s->stmt_fts_del_map, "DELETE FROM kv_fts_map WHERE k = ?1", err) == WADA_OK) {
        sqlite3_bind_blob(s->stmt_fts_del_map, 1, key.p, (int)key.n, SQLITE_STATIC);
        sqlite3_step(s->stmt_fts_del_map);
        sqlite3_reset(s->stmt_fts_del_map);
    }
    return WADA_OK;
}

static wada_status index_put(struct wada_store *s, wada_slice key, wada_slice value, wada_err *err) {
    if (memchr(value.p, 0, value.n) != NULL) return WADA_OK; /* binary: not indexed */
    if (index_remove(s, key, err) != WADA_OK) return WADA_ERR;

    if (cached_stmt(s->db, &s->stmt_fts_ins_fts, "INSERT INTO kv_fts (k, text) VALUES (?1, ?2)", err) != WADA_OK) {
        return WADA_ERR;
    }
    sqlite3_bind_blob(s->stmt_fts_ins_fts, 1, key.p, (int)key.n, SQLITE_STATIC);
    sqlite3_bind_blob(s->stmt_fts_ins_fts, 2, value.p, (int)value.n, SQLITE_STATIC);
    int rc = sqlite3_step(s->stmt_fts_ins_fts);
    sqlite3_reset(s->stmt_fts_ins_fts);
    if (rc != SQLITE_DONE) return err_from_db(s->db, rc, err);

    int64_t rid = sqlite3_last_insert_rowid(s->db);
    if (cached_stmt(s->db, &s->stmt_fts_ins_map, "INSERT INTO kv_fts_map (k, rid) VALUES (?1, ?2)", err) != WADA_OK) {
        return WADA_ERR;
    }
    sqlite3_bind_blob(s->stmt_fts_ins_map, 1, key.p, (int)key.n, SQLITE_STATIC);
    sqlite3_bind_int64(s->stmt_fts_ins_map, 2, rid);
    rc = sqlite3_step(s->stmt_fts_ins_map);
    sqlite3_reset(s->stmt_fts_ins_map);
    return rc == SQLITE_DONE ? WADA_OK : err_from_db(s->db, rc, err);
}

/* ---- backups -----------------------------------------------------------
 *
 * Two moments are worth a copy:
 *
 *   - before a schema migration, because a migration bug can rewrite every
 *     record (the copy is kept, one per old version);
 *   - once a day on open, because most data loss is a user mistake, and a
 *     daily snapshot is the cheapest undo. The daily file is replaced, so the
 *     backup directory stays bounded: one daily copy plus one per migration.
 *
 * The copy is SQLite's own `VACUUM INTO`, which writes a consistent database
 * even with WAL content pending. A backup failure before a migration aborts
 * the open: migrating without a safety copy is the one thing this step must
 * not do silently.
 */

static bool make_dir(const char *path) {
#ifdef _WIN32
    WCHAR wpath[1024] = {0};
    if (!utf8_to_wide_path(path, wpath, 1024)) return false;
    return _wmkdir(wpath) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

static void dir_of(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) {
        snprintf(out, cap, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0) n = 1; /* "/wada.db" -> "/" */
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static bool is_memory_url(const char *url) {
    return url && (url[0] == ':' || strstr(url, "mode=memory") != NULL);
}

static bool backup_from_today(const char *path) {
#ifdef _WIN32
    WCHAR wpath[1024] = {0};
    if (!utf8_to_wide_path(path, wpath, 1024)) return false;
    struct _stat64 st;
    if (_wstat64(wpath, &st) != 0) return false;
    time_t mtime = (time_t)st.st_mtime;
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    time_t mtime = st.st_mtime;
#endif
    time_t now = time(NULL);
    struct tm saved = *localtime(&mtime);
    struct tm today = *localtime(&now);
    return saved.tm_year == today.tm_year && saved.tm_mon == today.tm_mon &&
           saved.tm_mday == today.tm_mday;
}

static wada_status vacuum_into(struct wada_store *s, const char *path, wada_err *err) {
    /* VACUUM INTO refuses an existing file: the backup replaces the old copy. */
#ifdef _WIN32
    WCHAR wpath[1024] = {0};
    if (utf8_to_wide_path(path, wpath, 1024)) _wremove(wpath);
#else
    remove(path);
#endif
    char *sql = sqlite3_mprintf("VACUUM INTO %Q", path);
    if (!sql) return wada_err_set(err, WADA_ERR_NOMEM, "backup: out of memory");
    wada_status st = exec_sql(s->db, sql, err);
    sqlite3_free(sql);
    return st;
}

static void db_stem(const char *path, char *out, size_t cap) {
    const char *base = path;
    const char *slash = strrchr(path, '/');
    if (slash) base = slash + 1;
    slash = strrchr(base, '\\');
    if (slash) base = slash + 1;
    size_t n = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot != base) n = (size_t)(dot - base);
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
    if (!out[0]) snprintf(out, cap, "wada");
}

static bool backup_dir(const char *url, char *dir, size_t dir_cap, char *backup_dir,
                       size_t backup_cap, char *stem, size_t stem_cap) {
    dir_of(url, dir, dir_cap);
    db_stem(url, stem, stem_cap);
    int n = snprintf(backup_dir, backup_cap, "%s" WADA_PATH_SEP "backups", dir);
    return n > 0 && (size_t)n < backup_cap;
}

static void set_backup_path(struct wada_store *s, const char *path) {
    snprintf(s->backup_path, sizeof s->backup_path, "%s", path);
}

static wada_status backup_before_migration(struct wada_store *s, const char *url, uint32_t from,
                                           wada_err *err) {
    char dir[700];
    char bdir[720];
    char stem[128];
    if (!backup_dir(url, dir, sizeof dir, bdir, sizeof bdir, stem, sizeof stem)) return WADA_OK;
    if (!make_dir(bdir)) {
        return wada_err_set(err, WADA_ERR_IO, "backup: cannot create %s", bdir);
    }
    char stamp[32] = "unknown";
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tm);
    char path[820];
    snprintf(path, sizeof path, "%s" WADA_PATH_SEP "%s-pre-v%u-%s.db", bdir, stem, from, stamp);
    wada_status st = vacuum_into(s, path, err);
    if (st != WADA_OK) {
        char detail[WADA_ERR_MSG_CAP];
        snprintf(detail, sizeof detail, "%s", wada_err_msg(err));
        return wada_err_set(err, st, "backup before migration failed: %s", detail);
    }
    set_backup_path(s, path);
    return WADA_OK;
}

/* The daily snapshot can be turned off for automation (tests, CI). The
 * pre-migration snapshot cannot: that one protects data, not convenience. */
static bool daily_disabled(void) {
    const char *value = getenv("WADA_NO_DAILY_BACKUP");
    return value && value[0] == '1';
}

static void backup_daily(struct wada_store *s, const char *url) {
    if (daily_disabled()) return;
    /* A pre-migration copy is the more important one to report this open. */
    if (s->backup_path[0]) return;
    char dir[700];
    char bdir[720];
    char stem[128];
    if (!backup_dir(url, dir, sizeof dir, bdir, sizeof bdir, stem, sizeof stem)) return;
    char path[820];
    snprintf(path, sizeof path, "%s" WADA_PATH_SEP "%s-daily.db", bdir, stem);
    if (backup_from_today(path)) {
        set_backup_path(s, path);
        return;
    }
    if (!make_dir(bdir)) return;
    wada_err ignored;
    wada_err_clear(&ignored);
    if (vacuum_into(s, path, &ignored) == WADA_OK) set_backup_path(s, path);
}

static wada_status read_user_version(sqlite3 *db, uint32_t *out, wada_err *err) {
    sqlite3_stmt *stmt = NULL;
    if (prepare(db, "PRAGMA user_version", &stmt, err) != WADA_OK) return WADA_ERR;
    wada_status st = WADA_ERR;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = (uint32_t)sqlite3_column_int(stmt, 0);
        st = WADA_OK;
    }
    sqlite3_finalize(stmt);
    return st;
}

static wada_status rebuild_index(struct wada_store *s, wada_err *err) {
    wada_status st = exec_sql(s->db, "SAVEPOINT wada_rebuild", err);
    if (st != WADA_OK) return st;
    st = exec_sql(s->db, "DELETE FROM kv_fts", err);
    if (st != WADA_OK) goto fail;
    st = exec_sql(s->db, "DELETE FROM kv_fts_map", err);
    if (st != WADA_OK) goto fail;

    {
        sqlite3_stmt *stmt = NULL;
        if (prepare(s->db, "SELECT k, v FROM kv", &stmt, err) != WADA_OK) {
            st = WADA_ERR;
            goto fail;
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const void *k = sqlite3_column_blob(stmt, 0);
            const void *v = sqlite3_column_blob(stmt, 1);
            wada_slice key = {(const uint8_t *)k, (size_t)sqlite3_column_bytes(stmt, 0)};
            wada_slice value = {(const uint8_t *)v, (size_t)sqlite3_column_bytes(stmt, 1)};
            if (index_put(s, key, value, err) != WADA_OK) {
                sqlite3_finalize(stmt);
                st = WADA_ERR;
                goto fail;
            }
        }
        sqlite3_finalize(stmt);
    }
    return exec_sql(s->db, "RELEASE wada_rebuild", err);

fail:
    exec_sql(s->db, "ROLLBACK TO wada_rebuild", NULL);
    exec_sql(s->db, "RELEASE wada_rebuild", NULL);
    return st;
}

static wada_status ensure_schema(struct wada_store *s, wada_err *err) {
    wada_status st = exec_sql(s->db,
                              "CREATE TABLE IF NOT EXISTS kv ("
                              " k TEXT PRIMARY KEY NOT NULL,"
                              " v BLOB NOT NULL"
                              ") WITHOUT ROWID",
                              err);
    if (st != WADA_OK) return st;

    st = exec_sql(s->db,
                  "CREATE VIRTUAL TABLE IF NOT EXISTS kv_fts USING fts5("
                  " k UNINDEXED, text, tokenize='unicode61 remove_diacritics 2')",
                  err);
    if (st != WADA_OK) return st;

    st = exec_sql(s->db,
                  "CREATE TABLE IF NOT EXISTS kv_fts_map ("
                  " k TEXT PRIMARY KEY NOT NULL, rid INTEGER NOT NULL)",
                  err);
    if (st != WADA_OK) return st;

    uint32_t version = 0;
    if (read_user_version(s->db, &version, err) != WADA_OK) return WADA_ERR;
    if (version < WADA_SQLITE_SCHEMA) {
        st = rebuild_index(s, err);
        if (st != WADA_OK) return st;
        char sql[64];
        snprintf(sql, sizeof sql, "PRAGMA user_version = %u", WADA_SQLITE_SCHEMA);
        st = exec_sql(s->db, sql, err);
        if (st != WADA_OK) return st;
        version = WADA_SQLITE_SCHEMA;
    }
    s->schema = version;
    return WADA_OK;
}

static wada_status sql_open(const char *url, wada_store **out, wada_err *err) {
    if (!url || !out) return wada_err_set(err, WADA_ERR_INVALID, "store open: url/out");
    struct wada_store *s = (struct wada_store *)calloc(1, sizeof *s);
    if (!s) return wada_err_set(err, WADA_ERR_NOMEM, "store open");

    int rc = sqlite3_open_v2(url, &s->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) {
        wada_status st = err_from_db(s->db, rc, err);
        sqlite3_close_v2(s->db);
        free(s);
        return st;
    }

    sqlite3_busy_timeout(s->db, 5000);
    exec_sql(s->db, "PRAGMA journal_mode=WAL", NULL);
    exec_sql(s->db, "PRAGMA synchronous=NORMAL", NULL);
    exec_sql(s->db, "PRAGMA foreign_keys=ON", NULL);

    /* Read the schema before it is touched so an upgrade can be protected. */
    uint32_t version = 0;
    if (read_user_version(s->db, &version, err) != WADA_OK) {
        char detail[WADA_ERR_MSG_CAP];
        snprintf(detail, sizeof detail, "%s", wada_err_msg(err));
        sqlite3_close_v2(s->db);
        free(s);
        return wada_err_set(err, WADA_ERR, "store open: %s",
                            detail[0] ? detail : "cannot read the schema version");
    }
    bool memory = is_memory_url(url);
    if (!memory && version > 0 && version < WADA_SQLITE_SCHEMA) {
        wada_status st = backup_before_migration(s, url, version, err);
        if (st != WADA_OK) {
            sqlite3_close_v2(s->db);
            free(s);
            return st;
        }
    }

    wada_status st = ensure_schema(s, err);
    if (st != WADA_OK) {
        sqlite3_close_v2(s->db);
        free(s);
        return st;
    }
    if (!memory && version > 0) backup_daily(s, url);
    *out = (wada_store *)s;
    return WADA_OK;
}

static void sql_close(struct wada_store *s) {
    if (!s) return;
    finalize_cached_stmts(s);
    sqlite3_close_v2(s->db);
    free(s);
}

static wada_status sql_begin(struct wada_store *s, wada_store_mode mode, struct wada_txn **out, wada_err *err) {
    if (!s || !out) return wada_err_set(err, WADA_ERR_INVALID, "begin");
    sqlite3_stmt **slot = (mode == WADA_STORE_WRITE) ? &s->stmt_begin_write : &s->stmt_begin_read;
    const char *sql = (mode == WADA_STORE_WRITE) ? "BEGIN IMMEDIATE" : "BEGIN";
    if (cached_stmt(s->db, slot, sql, err) != WADA_OK) return WADA_ERR;
    int rc = sqlite3_step(*slot);
    sqlite3_reset(*slot);
    if (rc != SQLITE_DONE) return err_from_db(s->db, rc, err);

    struct wada_txn *txn = (struct wada_txn *)calloc(1, sizeof *txn);
    if (!txn) {
        exec_sql(s->db, "ROLLBACK", NULL);
        return wada_err_set(err, WADA_ERR_NOMEM, "begin");
    }
    txn->store = s;
    *out = (wada_txn *)txn;
    return WADA_OK;
}

static wada_status sql_commit(struct wada_txn *t, wada_err *err) {
    if (!t) return wada_err_set(err, WADA_ERR_INVALID, "commit");
    struct wada_store *s = t->store;
    txn_drop_last(t);
    wada_status st = WADA_OK;
    if (cached_stmt(s->db, &s->stmt_commit, "COMMIT", err) != WADA_OK) {
        st = WADA_ERR_IO;
    } else {
        int rc = sqlite3_step(s->stmt_commit);
        sqlite3_reset(s->stmt_commit);
        if (rc != SQLITE_DONE) st = err_from_db(s->db, rc, err);
    }
    if (st != WADA_OK && !sqlite3_get_autocommit(s->db)) {
        exec_sql(s->db, "ROLLBACK", NULL);
    }
    free(t);
    return st;
}

static void sql_rollback(struct wada_txn *t) {
    if (!t) return;
    struct wada_store *s = t->store;
    txn_drop_last(t);
    if (cached_stmt(s->db, &s->stmt_rollback, "ROLLBACK", NULL) == WADA_OK) {
        sqlite3_step(s->stmt_rollback);
        sqlite3_reset(s->stmt_rollback);
    } else {
        exec_sql(s->db, "ROLLBACK", NULL);
    }
    free(t);
}

static wada_status sql_get(struct wada_txn *t, wada_slice key, wada_slice *value, wada_err *err) {
    if (!t || !value) return wada_err_set(err, WADA_ERR_INVALID, "get");
    struct wada_store *s = t->store;
    txn_drop_last(t);

    if (cached_stmt(s->db, &s->stmt_get, "SELECT v FROM kv WHERE k = ?1", err) != WADA_OK) return WADA_ERR;
    sqlite3_bind_blob(s->stmt_get, 1, key.p, (int)key.n, SQLITE_STATIC);

    int rc = sqlite3_step(s->stmt_get);
    if (rc == SQLITE_ROW) {
        value->p = (const uint8_t *)sqlite3_column_blob(s->stmt_get, 0);
        value->n = (size_t)sqlite3_column_bytes(s->stmt_get, 0);
        t->last = s->stmt_get;
        return WADA_OK;
    }
    sqlite3_reset(s->stmt_get);
    if (rc == SQLITE_DONE) return wada_err_set(err, WADA_ERR_NOT_FOUND, "key not found");
    return err_from_db(s->db, rc, err);
}

static wada_status sql_put(struct wada_txn *t, wada_slice key, wada_slice value, wada_err *err) {
    if (!t) return wada_err_set(err, WADA_ERR_INVALID, "put");
    struct wada_store *s = t->store;
    txn_drop_last(t);

    if (cached_stmt(s->db, &s->stmt_put,
                    "INSERT INTO kv (k, v) VALUES (?1, ?2) "
                    "ON CONFLICT(k) DO UPDATE SET v = excluded.v",
                    err) != WADA_OK) {
        return WADA_ERR;
    }
    sqlite3_bind_blob(s->stmt_put, 1, key.p, (int)key.n, SQLITE_STATIC);
    sqlite3_bind_blob(s->stmt_put, 2, value.p, (int)value.n, SQLITE_STATIC);
    int rc = sqlite3_step(s->stmt_put);
    sqlite3_reset(s->stmt_put);
    if (rc != SQLITE_DONE) return err_from_db(s->db, rc, err);
    return index_put(s, key, value, err);
}

static wada_status sql_del(struct wada_txn *t, wada_slice key, wada_err *err) {
    if (!t) return wada_err_set(err, WADA_ERR_INVALID, "del");
    struct wada_store *s = t->store;
    txn_drop_last(t);
    if (index_remove(s, key, err) != WADA_OK) return WADA_ERR;

    if (cached_stmt(s->db, &s->stmt_del, "DELETE FROM kv WHERE k = ?1", err) != WADA_OK) return WADA_ERR;
    sqlite3_bind_blob(s->stmt_del, 1, key.p, (int)key.n, SQLITE_STATIC);
    int rc = sqlite3_step(s->stmt_del);
    sqlite3_reset(s->stmt_del);
    if (rc != SQLITE_DONE) return err_from_db(s->db, rc, err);
    return WADA_OK;
}

/* Smallest byte string greater than every string starting with prefix. */
static bool prefix_upper(wada_slice prefix, unsigned char *buf, size_t cap, size_t *out_n) {
    if (prefix.n == 0 || prefix.n > cap) return false;
    memcpy(buf, prefix.p, prefix.n);
    size_t i = prefix.n;
    while (i > 0 && buf[i - 1] == 0xFF) i--;
    if (i == 0) return false;
    buf[i - 1] = (unsigned char)(buf[i - 1] + 1);
    *out_n = i;
    return true;
}

static wada_status sql_list(struct wada_txn *t, wada_slice prefix, struct wada_iter **out, wada_err *err) {
    if (!t || !out) return wada_err_set(err, WADA_ERR_INVALID, "list");
    txn_drop_last(t);

    unsigned char upper[256];
    size_t upper_n = 0;
    sqlite3_stmt *stmt = NULL;
    if (prefix.n == 0) {
        if (prepare(t->store->db, "SELECT k, v FROM kv ORDER BY k", &stmt, err) != WADA_OK) return WADA_ERR;
    } else if (prefix_upper(prefix, upper, sizeof upper, &upper_n)) {
        if (prepare(t->store->db, "SELECT k, v FROM kv WHERE k >= ?1 AND k < ?2 ORDER BY k", &stmt, err) != WADA_OK) {
            return WADA_ERR;
        }
        sqlite3_bind_blob(stmt, 1, prefix.p, (int)prefix.n, SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 2, upper, (int)upper_n, SQLITE_TRANSIENT);
    } else {
        if (prepare(t->store->db, "SELECT k, v FROM kv WHERE k >= ?1 ORDER BY k", &stmt, err) != WADA_OK) {
            return WADA_ERR;
        }
        sqlite3_bind_blob(stmt, 1, prefix.p, (int)prefix.n, SQLITE_TRANSIENT);
    }

    struct wada_iter *it = (struct wada_iter *)calloc(1, sizeof *it);
    if (!it) {
        sqlite3_finalize(stmt);
        return wada_err_set(err, WADA_ERR_NOMEM, "list");
    }
    it->stmt = stmt;
    *out = (wada_iter *)it;
    return WADA_OK;
}

static void build_match(const char *query, char *out, size_t cap) {
    size_t o = 0;
    int terms = 0;
    out[0] = '\0';
    const char *p = query;
    while (*p && terms < 8) {
        while (*p && !isalnum((unsigned char)*p) && (unsigned char)*p < 0x80) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80)) p++;
        size_t n = (size_t)(p - start);
        if (!n) {
            p++;
            continue;
        }
        if (o + n + 8 >= cap) break;
        if (terms) {
            memcpy(out + o, " AND ", 5);
            o += 5;
        }
        out[o++] = '"';
        memcpy(out + o, start, n);
        o += n;
        out[o++] = '"';
        out[o++] = '*';
        terms++;
    }
    out[o] = '\0';
}

static wada_status sql_search(struct wada_txn *t, wada_slice prefix, const char *query, uint32_t limit,
                              struct wada_iter **out, wada_err *err) {
    if (!t || !out) return wada_err_set(err, WADA_ERR_INVALID, "search");
    char match[512];
    build_match(query ? query : "", match, sizeof match);
    if (!match[0]) return sql_list(t, prefix, out, err);

    txn_drop_last(t);
    sqlite3_stmt *stmt = NULL;
    if (prepare(t->store->db,
                "SELECT k FROM kv_fts WHERE kv_fts MATCH ?1 AND k LIKE ?2 "
                "ORDER BY rank LIMIT ?3",
                &stmt, err) != WADA_OK) {
        return WADA_ERR;
    }

    char like[300];
    size_t pn = prefix.n < sizeof like - 2 ? prefix.n : sizeof like - 2;
    memcpy(like, prefix.p, pn);
    like[pn] = '%';
    like[pn + 1] = '\0';

    sqlite3_bind_text(stmt, 1, match, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, (int)(limit ? limit : 200));

    struct wada_iter *it = (struct wada_iter *)calloc(1, sizeof *it);
    if (!it) {
        sqlite3_finalize(stmt);
        return wada_err_set(err, WADA_ERR_NOMEM, "search");
    }
    it->stmt = stmt;
    *out = (wada_iter *)it;
    return WADA_OK;
}

static wada_status sql_next(struct wada_iter *it, wada_slice *key, wada_slice *value, wada_err *err) {
    if (!it || !key || !value) return wada_err_set(err, WADA_ERR_INVALID, "next");
    if (it->done) return wada_err_set(err, WADA_ERR_NOT_FOUND, "end of iteration");

    int rc = sqlite3_step(it->stmt);
    if (rc == SQLITE_ROW) {
        const void *k = sqlite3_column_blob(it->stmt, 0);
        key->p = (const uint8_t *)k;
        key->n = (size_t)sqlite3_column_bytes(it->stmt, 0);
        if (sqlite3_column_count(it->stmt) > 1) {
            const void *v = sqlite3_column_blob(it->stmt, 1);
            value->p = (const uint8_t *)v;
            value->n = (size_t)sqlite3_column_bytes(it->stmt, 1);
        } else {
            value->p = NULL;
            value->n = 0;
        }
        return WADA_OK;
    }
    if (rc == SQLITE_DONE) {
        it->done = true;
        return wada_err_set(err, WADA_ERR_NOT_FOUND, "end of iteration");
    }
    return err_from_db(NULL, rc, err);
}

static void sql_iter_close(struct wada_iter *it) {
    if (!it) return;
    sqlite3_finalize(it->stmt);
    free(it);
}

static wada_status sql_migrate(struct wada_store *s, uint32_t target, wada_err *err) {
    if (!s) return wada_err_set(err, WADA_ERR_INVALID, "migrate");
    if (target == 0) return wada_err_set(err, WADA_ERR_INVALID, "migrate: target 0");
    if (target <= s->schema) return WADA_OK;
    if (target != WADA_SQLITE_SCHEMA) {
        return wada_err_set(err, WADA_ERR_UNSUPPORTED, "migrate: unknown schema %u", target);
    }
    wada_status st = rebuild_index(s, err);
    if (st != WADA_OK) return st;
    char sql[64];
    snprintf(sql, sizeof sql, "PRAGMA user_version = %u", target);
    st = exec_sql(s->db, sql, err);
    if (st == WADA_OK) s->schema = target;
    return st;
}

static uint32_t sql_caps(const struct wada_store *s) {
    (void)s;
    return WADA_STORE_CAP_TRANSACTIONS | WADA_STORE_CAP_MIGRATIONS | WADA_STORE_CAP_SEARCH;
}

static uint32_t sql_schema_version(const struct wada_store *s) {
    return s ? s->schema : 0;
}

static const char *sql_last_backup(const struct wada_store *s) {
    return s ? s->backup_path : "";
}

static const wada_store_ops g_sqlite_ops = {
    { (uint32_t)sizeof(wada_store_ops), WADA_STORE_ABI },
    sql_open,
    sql_close,
    sql_begin,
    sql_commit,
    sql_rollback,
    sql_get,
    sql_put,
    sql_del,
    sql_list,
    sql_next,
    sql_iter_close,
    sql_migrate,
    sql_caps,
    sql_schema_version,
    sql_search,
    sql_last_backup,
};

const wada_store_ops *wada_store_sqlite(void) {
    return &g_sqlite_ops;
}
