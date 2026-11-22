#include "wada_id.h"

#include <stdio.h>
#include <time.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

static unsigned long g_serial;
static uint32_t g_seed;

static uint32_t make_seed(void) {
    uint32_t seed = (uint32_t)time(NULL);
    seed ^= (uint32_t)(uintptr_t)&seed; /* address-space entropy */
    seed ^= (uint32_t)clock() * 2654435761u;
#if defined(_WIN32)
    seed ^= (uint32_t)_getpid() * 2246822519u;
#else
    seed ^= (uint32_t)getpid() * 2246822519u;
#endif
    if (seed == 0) seed = 0x9E3779B9u;
    return seed;
}

void wada_id_make(char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (g_seed == 0) g_seed = make_seed();
    unsigned long serial = ++g_serial;
    snprintf(out, cap, "%lld-%lu-%08x", (long long)time(NULL), serial, (unsigned)g_seed);
}
