#ifndef HONEY_BADGER_NNUE_H
#define HONEY_BADGER_NNUE_H

#include "types.h"
#include "lookups.h"

#define NNUE_QA (255)       
#define NNUE_QB (64)        
#define NNUE_SCALE (600)    

#define NNUE_MAX_UPDATE_DISTANCE (32)

typedef struct Accumulator {
    alignas(64) i32 vals[NO_COLOR][NNUE_HIDDEN_SIZE];
    bool computed[NO_COLOR];
} Accumulator;

typedef struct DirtyPieces {
    u8 count;
    Piece ps[3];
    Square from_sqs[3];
    Square to_sqs[3];
} DirtyPieces;

#define DIRTY_PIECES_ADD(dp, p, from_sq, to_sq) \
    ((dp).ps[(dp).count] = (p), (dp).from_sqs[(dp).count] = (from_sq), (dp).to_sqs[(dp).count++] = (to_sq))

#define NNUE_FEATURE(c, p, sq) \
    ((c) == COLOR_WHITE ? (p) * 64 + (sq) : (((p) + 6) % 12) * 64 + ((sq) ^ 56))

struct Position;

const char *nnue_simd_name(void);

Score nnue_evaluate(const struct Position *pos);

Score nnue_evaluate_from_scratch(const struct Position *pos);

#endif