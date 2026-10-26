#ifndef HONEY_BADGER_HISTORY_H
#define HONEY_BADGER_HISTORY_H

#include "types.h"

#define PAWN_HISTORY_SIZE (512)         
#define LOW_PLY_HISTORY_SIZE (4)

#define BUTTERFLY_HISTORY_LIMIT (7183)
#define LOW_PLY_HISTORY_LIMIT (7183)
#define CAPTURE_HISTORY_LIMIT (10692)
#define PIECE_TO_HISTORY_LIMIT (30000)
#define PAWN_HISTORY_LIMIT (8192)

#define FROM_TO_INDEX(mv) ((mv) & 0xFFF)

#define CAPTURED_PIECE_TYPE_INDEX(p) ((p) == NO_PIECE ? NO_PIECE_TYPE : PIECE_TYPE_FROM_PIECE(p))

#define PAWN_STRUCTURE_INDEX(pos) (PAWN_KEY(pos) & (PAWN_HISTORY_SIZE - 1))

typedef struct PieceToHistory {
    i16 vals[NO_PIECE][NO_SQUARE];
} PieceToHistory;

typedef struct ContinuationHistory {
    PieceToHistory tables[NO_PIECE + 1][NO_SQUARE];
} ContinuationHistory;

typedef struct Histories {
    i16 butterfly[NO_COLOR][NO_SQUARE * NO_SQUARE];

    i16 low_ply[LOW_PLY_HISTORY_SIZE][NO_SQUARE * NO_SQUARE];

    i16 capture[NO_PIECE][NO_SQUARE][NO_PIECE_TYPE + 1];

    i16 pawn[PAWN_HISTORY_SIZE][NO_PIECE][NO_SQUARE];

    ContinuationHistory continuation[2][2];
} Histories;

Histories *histories_create(void);
void histories_destroy(Histories *h);

void histories_clear(Histories *h);

void histories_new_search(Histories *h);

static inline void history_update(i16 *entry, i32 bonus, i32 limit) {
    i32 clamped = bonus < -limit ? -limit : (bonus > limit ? limit : bonus);
    i32 abs_clamped = clamped < 0 ? -clamped : clamped;
    *entry = (i16)(*entry + clamped - *entry * abs_clamped / limit);
}

#endif