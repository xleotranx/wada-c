/* i18n: a string table keyed by the English source text.
 *
 * Keys are the English strings themselves (the gettext model), so there is no
 * second identifier to keep in sync and a missing translation degrades to
 * readable English instead of "notes.new". The cost is that changing the
 * English wording orphans its translation — the tests and --doctor make that
 * visible instead of silent:
 *
 *   - wada_tr() records every string that had no translation for the current
 *     language (bounded, deduplicated);
 *   - --doctor prints them, so an untranslated screen is a number, not a
 *     surprise.
 *
 * A product registers its table when it enters and passes its scope, so the
 * table disappears with the product — the same disposal rule as commands and
 * services. Tables must be static data: the table is borrowed, not copied.
 */
#ifndef WADA_I18N_H
#define WADA_I18N_H

#include "wada.h"

typedef struct wada_i18n_pair {
    const char *en; /* source text and key */
    const char *vi; /* Tiếng Việt, may be NULL until translated */
} wada_i18n_pair;

/* Registers the built-in shell strings. Idempotent. */
WADA_API void wada_i18n_init(void);

WADA_API int wada_i18n_count(void);
WADA_API const char *wada_i18n_code(int index); /* "en", "vi" */
WADA_API const char *wada_i18n_name(int index); /* "English", "Tiếng Việt" */

WADA_API int wada_i18n_lang(void);
WADA_API void wada_i18n_set_lang(int index);
WADA_API bool wada_i18n_set_lang_code(const char *code);

/* Borrowed static table; owned by the scope when owner is not NULL. */
WADA_API wada_status wada_i18n_register(wada_scope *owner, const wada_i18n_pair *pairs, int count);

/* The translation for the current language, or the English text. Never NULL. */
WADA_API const char *wada_tr(const char *en);
/* Translates the format string, then formats it. */
WADA_API void wada_trf(char *out, size_t cap, const char *en_fmt, ...) WADA_PRINTF(3, 4);

WADA_API int wada_i18n_miss_count(void);
WADA_API const char *wada_i18n_miss_at(int index);
WADA_API void wada_i18n_misses_clear(void);

#endif /* WADA_I18N_H */
