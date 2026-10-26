#ifndef HONEY_BADGER_MOVE_PICKER_H
#define HONEY_BADGER_MOVE_PICKER_H

#include "types.h"
#include "position.h"
#include "move_generation.h"
#include "history.h"

#define MOVE_PICKER_CONTINUATION_PLIES (6)

typedef enum MovePickerStage : u8 {
    MOVE_PICKER_STAGE_MAIN_CACHE,
    MOVE_PICKER_STAGE_CAPTURE_INIT,
    MOVE_PICKER_STAGE_GOOD_CAPTURE,
    MOVE_PICKER_STAGE_QUIET_INIT,
    MOVE_PICKER_STAGE_GOOD_QUIET,
    MOVE_PICKER_STAGE_BAD_CAPTURE,
    MOVE_PICKER_STAGE_BAD_QUIET,

    MOVE_PICKER_STAGE_EVASION_CACHE,
    MOVE_PICKER_STAGE_EVASION_INIT,
    MOVE_PICKER_STAGE_EVASION,

    MOVE_PICKER_STAGE_PROBCUT_CACHE,
    MOVE_PICKER_STAGE_PROBCUT_INIT,
    MOVE_PICKER_STAGE_PROBCUT,

    MOVE_PICKER_STAGE_QSEARCH_CACHE,
    MOVE_PICKER_STAGE_QCAPTURE_INIT,
    MOVE_PICKER_STAGE_QCAPTURE
} MovePickerStage;

typedef struct MovePicker {
    const Position *pos;
    const Histories *h;
    const PieceToHistory *const *cont_hists;
    Move cache_mv;
    Depth d;
    Score threshold;            
    u8 ply;                     
    MovePickerStage stage;
    bool skip_quiets;

    ScoredMove *cur;
    ScoredMove *end;
    ScoredMove *end_bad_captures;   
    ScoredMove *end_quiets;
    ScoredMove smvs[MAX_MOVES];
} MovePicker;

void move_picker_init(MovePicker *mp, const Position *pos, Move cache_mv, Depth d, const Histories *h,
                      const PieceToHistory *const *cont_hists, u8 ply);

void move_picker_init_probcut(MovePicker *mp, const Position *pos, Move cache_mv, Score threshold,
                              const Histories *h);

Move move_picker_next(MovePicker *mp);

static inline void move_picker_skip_quiets(MovePicker *mp) {
    mp->skip_quiets = true;
}

#endif