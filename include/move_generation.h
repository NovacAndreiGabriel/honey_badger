#ifndef HONEY_BADGER_MOVE_GENERATION_H
#define HONEY_BADGER_MOVE_GENERATION_H

#include "types.h"
#include "list.h"
#include "position.h"

#define MAX_MOVES (256)

typedef enum MoveGenerationType : u8 {
    MOVE_GENERATION_TYPE_CAPTURES,      
    MOVE_GENERATION_TYPE_QUIETS,        
    MOVE_GENERATION_TYPE_EVASIONS,      
    MOVE_GENERATION_TYPE_NON_EVASIONS,  
    MOVE_GENERATION_TYPE_LEGAL          
} MoveGenerationType;

typedef struct ScoredMove {
    Move mv;
    Score sc;
} ScoredMove;

LIST_DEFINE(ScoredMove, MAX_MOVES);

ScoredMove *generate_captures(const Position *pos, ScoredMove *smvs);
ScoredMove *generate_quiets(const Position *pos, ScoredMove *smvs);
ScoredMove *generate_evasions(const Position *pos, ScoredMove *smvs);
ScoredMove *generate_non_evasions(const Position *pos, ScoredMove *smvs);
ScoredMove *generate_legal(const Position *pos, ScoredMove *smvs);

#define LIST_GENERATE(list, pos, gen) ((list).end = generate_##gen((pos), LIST_START(list)))

#endif