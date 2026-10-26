#include <limits.h>
#include <stddef.h>

#include "move_picker.h"
#include "lookups.h"

#define GOOD_QUIET_THRESHOLD (-14000)

#define QUIET_SORT_LIMIT(d) (-3560 * (d))


static inline bool is_capture_stage(const Position *pos, Move mv) {
    return IS_CAPTURE(pos, mv)
        || (MOVE_TYPE_FROM_MOVE(mv) == MOVE_TYPE_PROMOTION && PROMOTION_PIECE_TYPE_FROM_MOVE(mv) == PIECE_TYPE_QUEEN);
}

void move_picker_init(MovePicker *mp, const Position *pos, Move cache_mv, Depth d, const Histories *h,
                      const PieceToHistory *const *cont_hists, u8 ply) {
    mp->pos = pos;
    mp->h = h;
    mp->cont_hists = cont_hists;
    mp->cache_mv = cache_mv;
    mp->d = d;
    mp->threshold = 0;
    mp->ply = ply;
    mp->skip_quiets = false;

    bool has_cache_mv = cache_mv != MOVE_NONE && is_pseudo_legal(pos, cache_mv);
    if (!has_cache_mv)
        mp->cache_mv = MOVE_NONE;

    if (CHECKERS_BITBOARD(pos))
        mp->stage = MOVE_PICKER_STAGE_EVASION_CACHE + !has_cache_mv;
    else
        mp->stage = (d > 0 ? MOVE_PICKER_STAGE_MAIN_CACHE : MOVE_PICKER_STAGE_QSEARCH_CACHE) + !has_cache_mv;
}

void move_picker_init_probcut(MovePicker *mp, const Position *pos, Move cache_mv, Score threshold,
                              const Histories *h) {
    mp->pos = pos;
    mp->h = h;
    mp->cont_hists = NULL;
    mp->cache_mv = cache_mv;
    mp->d = 0;
    mp->threshold = threshold;
    mp->ply = 0;
    mp->skip_quiets = false;

    bool has_cache_mv = cache_mv != MOVE_NONE
                     && is_pseudo_legal(pos, cache_mv)
                     && is_capture_stage(pos, cache_mv)
                     && see_ge(pos, cache_mv, threshold);

    mp->stage = MOVE_PICKER_STAGE_PROBCUT_CACHE + !has_cache_mv;
}


static Bitboard attacks_by(const Position *pos, Color c, PieceType pt) {
    Bitboard pieces_bb = COLOR_PIECE_TYPE_BITBOARD(pos, c, pt);

    if (pt == PIECE_TYPE_PAWN) {
        Bitboard not_a_bb = ~BITBOARD_FROM_FILE(FILE_A);
        Bitboard not_h_bb = ~BITBOARD_FROM_FILE(FILE_H);
        return c == COLOR_WHITE
             ? ((pieces_bb & not_a_bb) << 7) | ((pieces_bb & not_h_bb) << 9)
             : ((pieces_bb & not_a_bb) >> 9) | ((pieces_bb & not_h_bb) >> 7);
    }

    Bitboard occ_bb = OCCUPANCY_BITBOARD(pos);
    Bitboard attacks_bb = BITBOARD_NONE;

    for (; pieces_bb; POP_LSB(pieces_bb)) {
        Square sq = (Square)LSB(pieces_bb);
        switch (pt) {
            case PIECE_TYPE_KNIGHT: attacks_bb |= KNIGHT_ATTACKS_BITBOARD(sq); break;
            case PIECE_TYPE_BISHOP: attacks_bb |= BISHOP_ATTACKS_BITBOARD(sq, occ_bb); break;
            case PIECE_TYPE_ROOK:   attacks_bb |= ROOK_ATTACKS_BITBOARD(sq, occ_bb); break;
            case PIECE_TYPE_QUEEN:  attacks_bb |= QUEEN_ATTACKS_BITBOARD(sq, occ_bb); break;
            default:                attacks_bb |= KING_ATTACKS_BITBOARD(sq); break;
        }
    }

    return attacks_bb;
}

static void score_captures(MovePicker *mp) {
    const Position *pos = mp->pos;

    for (ScoredMove *cur = mp->cur; cur < mp->end; cur++) {
        Square to_sq = TARGET_SQUARE_FROM_MOVE(cur->mv);
        Piece captured_p = PIECE_ON_SQUARE(pos, to_sq);

        cur->sc = mp->h->capture[MOVED_PIECE(pos, cur->mv)][to_sq][CAPTURED_PIECE_TYPE_INDEX(captured_p)]
                + 7 * PIECE_VALUE(captured_p);
    }
}

static void score_quiets(MovePicker *mp) {
    const Position *pos = mp->pos;
    const Histories *h = mp->h;
    Color stm_c = SIDE_TO_MOVE(pos);
    Color nstm_c = OPPOSITE_COLOR(stm_c);
    u16 pawn_index = (u16)PAWN_STRUCTURE_INDEX(pos);

    Bitboard threatened_by_pawn_bb = attacks_by(pos, nstm_c, PIECE_TYPE_PAWN);
    Bitboard threatened_by_minor_bb = attacks_by(pos, nstm_c, PIECE_TYPE_KNIGHT)
                                    | attacks_by(pos, nstm_c, PIECE_TYPE_BISHOP)
                                    | threatened_by_pawn_bb;
    Bitboard threatened_by_rook_bb = attacks_by(pos, nstm_c, PIECE_TYPE_ROOK) | threatened_by_minor_bb;

    Bitboard threatened_pieces_bb = (COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, PIECE_TYPE_QUEEN) & threatened_by_rook_bb)
                                  | (COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, PIECE_TYPE_ROOK) & threatened_by_minor_bb)
                                  | ((COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, PIECE_TYPE_KNIGHT)
                                    | COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, PIECE_TYPE_BISHOP)) & threatened_by_pawn_bb);

    for (ScoredMove *cur = mp->cur; cur < mp->end; cur++) {
        Move mv = cur->mv;
        Piece p = MOVED_PIECE(pos, mv);
        PieceType pt = PIECE_TYPE_FROM_PIECE(p);
        Square from_sq = ORIGIN_SQUARE_FROM_MOVE(mv);
        Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);
        Bitboard from_bb = BITBOARD_FROM_SQUARE(from_sq);
        Bitboard to_bb = BITBOARD_FROM_SQUARE(to_sq);

        Score sc = 2 * h->butterfly[stm_c][FROM_TO_INDEX(mv)];
        sc += 2 * h->pawn[pawn_index][p][to_sq];
        sc += mp->cont_hists[0]->vals[p][to_sq];
        sc += mp->cont_hists[1]->vals[p][to_sq];
        sc += mp->cont_hists[2]->vals[p][to_sq];
        sc += mp->cont_hists[3]->vals[p][to_sq];
        sc += mp->cont_hists[5]->vals[p][to_sq];

        if (CHECK_SQUARES_BITBOARD(pos, pt) & to_bb)
            sc += 16384;

        if (threatened_pieces_bb & from_bb) {
            if (pt == PIECE_TYPE_QUEEN && !(to_bb & threatened_by_rook_bb))
                sc += 51700;
            else if (pt == PIECE_TYPE_ROOK && !(to_bb & threatened_by_minor_bb))
                sc += 25600;
            else if (pt != PIECE_TYPE_QUEEN && pt != PIECE_TYPE_ROOK && !(to_bb & threatened_by_pawn_bb))
                sc += 14450;
        }

        if (pt == PIECE_TYPE_QUEEN && (to_bb & threatened_by_rook_bb))
            sc -= 49000;
        else if (pt == PIECE_TYPE_ROOK && (to_bb & threatened_by_minor_bb))
            sc -= 24335;

        if (mp->ply < LOW_PLY_HISTORY_SIZE)
            sc += 8 * h->low_ply[mp->ply][FROM_TO_INDEX(mv)] / (1 + 2 * mp->ply);

        cur->sc = sc;
    }
}

static void score_evasions(MovePicker *mp) {
    const Position *pos = mp->pos;
    const Histories *h = mp->h;
    Color stm_c = SIDE_TO_MOVE(pos);
    u16 pawn_index = (u16)PAWN_STRUCTURE_INDEX(pos);

    for (ScoredMove *cur = mp->cur; cur < mp->end; cur++) {
        Move mv = cur->mv;
        Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);
        Piece p = MOVED_PIECE(pos, mv);

        if (is_capture_stage(pos, mv))
            cur->sc = PIECE_VALUE(PIECE_ON_SQUARE(pos, to_sq)) + (1 << 28);
        else
            cur->sc = h->butterfly[stm_c][FROM_TO_INDEX(mv)]
                    + mp->cont_hists[0]->vals[p][to_sq]
                    + h->pawn[pawn_index][p][to_sq];
    }
}

static void partial_insertion_sort(ScoredMove *begin, ScoredMove *end, Score limit) {
    for (ScoredMove *sorted_end = begin, *p = begin + 1; p < end; p++) {
        if (p->sc >= limit) {
            ScoredMove tmp = *p;
            *p = *++sorted_end;

            ScoredMove *q = sorted_end;
            for (; q != begin && (q - 1)->sc < tmp.sc; q--)
                *q = *(q - 1);
            *q = tmp;
        }
    }
}


typedef enum SelectFilter : u8 {
    SELECT_ANY,
    SELECT_GOOD_CAPTURE,    
    SELECT_GOOD_QUIET,
    SELECT_BAD_QUIET,
    SELECT_PROBCUT          
} SelectFilter;

static Move select_move(MovePicker *mp, SelectFilter filter) {
    for (; mp->cur < mp->end; mp->cur++) {
        ScoredMove *cur = mp->cur;

        if (cur->mv == mp->cache_mv)
            continue;

        bool pass;
        switch (filter) {
            case SELECT_GOOD_CAPTURE:
                pass = see_ge(mp->pos, cur->mv, -cur->sc / 18);
                if (!pass)
                    *mp->end_bad_captures++ = *cur;     
                break;
            case SELECT_GOOD_QUIET: pass = cur->sc > GOOD_QUIET_THRESHOLD; break;
            case SELECT_BAD_QUIET:  pass = cur->sc <= GOOD_QUIET_THRESHOLD; break;
            case SELECT_PROBCUT:    pass = see_ge(mp->pos, cur->mv, mp->threshold); break;
            default:                pass = true; break;
        }

        if (pass)
            return (mp->cur++)->mv;
    }

    return MOVE_NONE;
}

Move move_picker_next(MovePicker *mp) {
    Move mv;

    while (true) {
        switch (mp->stage) {
            case MOVE_PICKER_STAGE_MAIN_CACHE:
            case MOVE_PICKER_STAGE_EVASION_CACHE:
            case MOVE_PICKER_STAGE_QSEARCH_CACHE:
            case MOVE_PICKER_STAGE_PROBCUT_CACHE:
                mp->stage++;
                return mp->cache_mv;

            case MOVE_PICKER_STAGE_CAPTURE_INIT:
            case MOVE_PICKER_STAGE_PROBCUT_INIT:
            case MOVE_PICKER_STAGE_QCAPTURE_INIT:
                mp->cur = mp->end_bad_captures = mp->smvs;
                mp->end = generate_captures(mp->pos, mp->cur);
                score_captures(mp);
                partial_insertion_sort(mp->cur, mp->end, INT_MIN);
                mp->stage++;
                continue;

            case MOVE_PICKER_STAGE_GOOD_CAPTURE:
                if ((mv = select_move(mp, SELECT_GOOD_CAPTURE)) != MOVE_NONE)
                    return mv;
                mp->stage++;
                continue;

            case MOVE_PICKER_STAGE_QUIET_INIT:
                if (!mp->skip_quiets) {
                    mp->cur = mp->end_bad_captures;
                    mp->end = mp->end_quiets = generate_quiets(mp->pos, mp->cur);
                    score_quiets(mp);
                    partial_insertion_sort(mp->cur, mp->end, QUIET_SORT_LIMIT(mp->d));
                }
                mp->stage++;
                continue;

            case MOVE_PICKER_STAGE_GOOD_QUIET:
                if (!mp->skip_quiets && (mv = select_move(mp, SELECT_GOOD_QUIET)) != MOVE_NONE)
                    return mv;

                mp->cur = mp->smvs;
                mp->end = mp->end_bad_captures;
                mp->stage++;
                continue;

            case MOVE_PICKER_STAGE_BAD_CAPTURE:
                if ((mv = select_move(mp, SELECT_ANY)) != MOVE_NONE)
                    return mv;

                mp->cur = mp->end_bad_captures;
                mp->end = mp->skip_quiets ? mp->end_bad_captures : mp->end_quiets;
                mp->stage++;
                continue;

            case MOVE_PICKER_STAGE_BAD_QUIET:
                if (!mp->skip_quiets)
                    return select_move(mp, SELECT_BAD_QUIET);
                return MOVE_NONE;

            case MOVE_PICKER_STAGE_EVASION_INIT:
                mp->cur = mp->smvs;
                mp->end = generate_evasions(mp->pos, mp->cur);
                score_evasions(mp);
                partial_insertion_sort(mp->cur, mp->end, INT_MIN);
                mp->stage++;
                continue;

            case MOVE_PICKER_STAGE_EVASION:
            case MOVE_PICKER_STAGE_QCAPTURE:
                return select_move(mp, SELECT_ANY);

            case MOVE_PICKER_STAGE_PROBCUT:
                return select_move(mp, SELECT_PROBCUT);
        }
    }
}