/* Record ids: unique, sortable, and safe across restarts.
 *
 * Format: "<unix-seconds>-<serial>-<seed>", e.g. "1760012345-3-6f3a1c02".
 *
 * The seconds prefix makes a listing sort roughly by creation time; the serial
 * keeps ids distinct inside one process; the seed (time, pid, clock and address
 * entropy, mixed once per process) keeps them distinct across processes and
 * across restarts, which is where the older "<seconds>-<serial>" scheme broke:
 * a restart reset the serial to 0 while the clock still read the same second,
 * so the first record of the new run overwrote the first record of the old one.
 *
 * Uniqueness is not left to probability alone: record layers also check the
 * store and draw again on a hit, so a clock set backwards cannot collide.
 */
#ifndef WADA_ID_H
#define WADA_ID_H

#include "wada.h"

WADA_API void wada_id_make(char *out, size_t cap);

#endif /* WADA_ID_H */
