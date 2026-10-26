#include <stdlib.h>

#include "history.h"

#define BUTTERFLY_HISTORY_START (68)
#define LOW_PLY_HISTORY_START (0)
#define CAPTURE_HISTORY_START (-700)
#define PAWN_HISTORY_START (-1270)
#define CONTINUATION_HISTORY_START (-470)

static void fill(i16 *vals, u64 count, i16 v) {
    for (u64 i = 0; i < count; i++)
        vals[i] = v;
}

Histories *histories_create(void) {
    Histories *h = malloc(sizeof(Histories));
    if (h)
        histories_clear(h);
    return h;
}

void histories_destroy(Histories *h) {
    free(h);
}

void histories_clear(Histories *h) {
    fill(&h->butterfly[0][0], sizeof(h->butterfly) / sizeof(i16), BUTTERFLY_HISTORY_START);
    fill(&h->low_ply[0][0], sizeof(h->low_ply) / sizeof(i16), LOW_PLY_HISTORY_START);
    fill(&h->capture[0][0][0], sizeof(h->capture) / sizeof(i16), CAPTURE_HISTORY_START);
    fill(&h->pawn[0][0][0], sizeof(h->pawn) / sizeof(i16), PAWN_HISTORY_START);
    fill(&h->continuation[0][0].tables[0][0].vals[0][0], sizeof(h->continuation) / sizeof(i16),
         CONTINUATION_HISTORY_START);
}

void histories_new_search(Histories *h) {
    fill(&h->low_ply[0][0], sizeof(h->low_ply) / sizeof(i16), LOW_PLY_HISTORY_START);
}