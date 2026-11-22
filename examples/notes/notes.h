/* Notes: metadata and body are stored separately.
 *
 *   note/<id>       v1\n<updated>\n<title>      (the list loads only this)
 *   note-body/<id>  the body, raw
 *
 * The split is what keeps 10,000 notes at ~2 MB instead of ~84 MB. A legacy
 * record (the pre-split layout with the body inline) is migrated on open.
 * Search asks the store first and falls back to a title scan.
 */
#ifndef NOTES_H
#define NOTES_H

#include "wada_iface.h"

#define NOTE_ID_CAP 48
#define NOTE_TITLE_CAP 128
#define NOTE_BODY_CAP 8192

typedef struct note {
    char id[NOTE_ID_CAP]; /* suffix, not the key: "1720000000-1" */
    char title[NOTE_TITLE_CAP];
    long long updated; /* unix seconds */
} note;

typedef struct notes {
    const wada_store_ops *ops;
    wada_store *store;
    note *items;
    int count;
    int cap;
} notes;

wada_status notes_open(notes *n, const wada_store_ops *ops, const char *url, wada_err *err);
void notes_close(notes *n);

int notes_count(const notes *n);
const note *notes_at(const notes *n, int index);
int notes_find(const notes *n, const char *id);

/* Loads the body of one note. Missing body reads as empty. */
wada_status notes_body(notes *n, int index, char *out, size_t cap, wada_err *err);

/* Stored body length, without copying it. */
wada_status notes_body_size(notes *n, int index, size_t *out_len, wada_err *err);

int notes_create(notes *n, wada_err *err);
wada_status notes_save(notes *n, int index, const char *title, const char *body, wada_err *err);
wada_status notes_remove(notes *n, int index, wada_err *err);

/* Fills out[] with note indices matching query, best first. Returns the count,
 * or -1 when the provider has no search support (caller falls back). */
int notes_search(notes *n, const char *query, int *out, int cap);

/* Length-prefixed backup: titles and bodies survive byte for byte, including
 * newlines. Import always creates new notes (new ids). */
wada_status notes_export(notes *n, const char *path, wada_err *err);
wada_status notes_import(notes *n, const char *path, int *added, wada_err *err);

#endif /* NOTES_H */
