/* Cards: the learning product's store layer.
 *
 * Same shape as notes: a text record per card under "card/", reviewed with a
 * small SM-2 style scheduler. It knows only wada_store_ops.
 */
#ifndef CARDS_H
#define CARDS_H

#include "wada_iface.h"

#define CARD_ID_CAP 48
#define CARD_TEXT_CAP 512

typedef struct card {
    char id[CARD_ID_CAP];
    char front[CARD_TEXT_CAP];
    char back[CARD_TEXT_CAP];
    double ease;      /* SM-2 ease factor, 1.3 .. 3.0 */
    int reps;         /* successful repetitions in a row */
    int interval_min; /* current interval in minutes */
    long long due;    /* unix seconds */
} card;

typedef struct cards {
    const wada_store_ops *ops;
    wada_store *store;
    card *items;
    int count;
    int cap;
} cards;

wada_status cards_open(cards *c, const wada_store_ops *ops, const char *url, wada_err *err);
void cards_close(cards *c);

int cards_count(const cards *c);
const card *cards_at(const cards *c, int index);
int cards_find(const cards *c, const char *id);

/* Returns the new card's index, or -1. Due immediately. */
int cards_create(cards *c, const char *front, const char *back, wada_err *err);

/* grade: 0 again, 1 hard, 2 good, 3 easy. */
wada_status cards_review(cards *c, int index, int grade, wada_err *err);

/* Indices due at or before now, sorted by due time. Returns the count. */
int cards_due(const cards *c, long long now, int *out, int cap);
int cards_due_count(const cards *c, long long now);

/* Earliest due time among all cards, or 0 when there are none. */
long long cards_next_due(const cards *c);

#endif /* CARDS_H */
