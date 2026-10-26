#include "move_generation.h"
#include "lookups.h"

#if defined(__GNUC__) || defined(__clang__)
    #define ALWAYS_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
    #define ALWAYS_INLINE static __forceinline
#else
    #define ALWAYS_INLINE static inline
#endif

typedef enum Direction : i8 {
    DIRECTION_NORTH = 8,
    DIRECTION_SOUTH = -8,   
    DIRECTION_NORTH_EAST = 9,
    DIRECTION_NORTH_WEST = 7,
    DIRECTION_SOUTH_EAST = -7,
    DIRECTION_SOUTH_WEST = -9
} Direction;

ALWAYS_INLINE Bitboard shift_bitboard(Bitboard bb, Direction d) {
    switch (d) {
        case DIRECTION_NORTH:      return bb << 8;
        case DIRECTION_SOUTH:      return bb >> 8;
        case DIRECTION_NORTH_EAST: return (bb & ~BITBOARD_FROM_FILE(FILE_H)) << 9;
        case DIRECTION_NORTH_WEST: return (bb & ~BITBOARD_FROM_FILE(FILE_A)) << 7;
        case DIRECTION_SOUTH_EAST: return (bb & ~BITBOARD_FROM_FILE(FILE_H)) >> 7;
        case DIRECTION_SOUTH_WEST: return (bb & ~BITBOARD_FROM_FILE(FILE_A)) >> 9;
    }
    return BITBOARD_NONE;
}

ALWAYS_INLINE Bitboard piece_attacks(PieceType pt, Square sq, Bitboard occ_bb) {
    switch (pt) {
        case PIECE_TYPE_KNIGHT: return KNIGHT_ATTACKS_BITBOARD(sq);
        case PIECE_TYPE_BISHOP: return BISHOP_ATTACKS_BITBOARD(sq, occ_bb);
        case PIECE_TYPE_ROOK:   return ROOK_ATTACKS_BITBOARD(sq, occ_bb);
        case PIECE_TYPE_QUEEN:  return QUEEN_ATTACKS_BITBOARD(sq, occ_bb);
        case PIECE_TYPE_KING:   return KING_ATTACKS_BITBOARD(sq);
        default:                return BITBOARD_NONE;
    }
}

ALWAYS_INLINE ScoredMove *make_promotions(ScoredMove *smvs, Square to_sq, Direction d, MoveGenerationType mgt, bool is_capture) {
    Square from_sq = (Square)(to_sq - d);
    bool all = mgt == MOVE_GENERATION_TYPE_EVASIONS || mgt == MOVE_GENERATION_TYPE_NON_EVASIONS;

    if (mgt == MOVE_GENERATION_TYPE_CAPTURES || all)
        (smvs++)->mv = PROMOTION_MOVE(from_sq, to_sq, PIECE_TYPE_QUEEN);

    if ((mgt == MOVE_GENERATION_TYPE_CAPTURES && is_capture) || (mgt == MOVE_GENERATION_TYPE_QUIETS && !is_capture) || all) {
        (smvs++)->mv = PROMOTION_MOVE(from_sq, to_sq, PIECE_TYPE_ROOK);
        (smvs++)->mv = PROMOTION_MOVE(from_sq, to_sq, PIECE_TYPE_BISHOP);
        (smvs++)->mv = PROMOTION_MOVE(from_sq, to_sq, PIECE_TYPE_KNIGHT);
    }

    return smvs;
}

ALWAYS_INLINE ScoredMove *generate_pawn_moves(const Position *pos, ScoredMove *smvs,
                                           Color stm_c, MoveGenerationType mgt, Bitboard target_bb) {
    Color nstm_c = OPPOSITE_COLOR(stm_c);
    Bitboard rank_7_bb = BITBOARD_FROM_RANK(stm_c == COLOR_WHITE ? RANK_7 : RANK_2);
    Bitboard rank_3_bb = BITBOARD_FROM_RANK(stm_c == COLOR_WHITE ? RANK_3 : RANK_6);
    Direction up_d = stm_c == COLOR_WHITE ? DIRECTION_NORTH : DIRECTION_SOUTH;
    Direction up_right_d = stm_c == COLOR_WHITE ? DIRECTION_NORTH_EAST : DIRECTION_SOUTH_WEST;
    Direction up_left_d = stm_c == COLOR_WHITE ? DIRECTION_NORTH_WEST : DIRECTION_SOUTH_EAST;

    Bitboard empty_bb = ~OCCUPANCY_BITBOARD(pos);
    Bitboard enemies_bb = mgt == MOVE_GENERATION_TYPE_EVASIONS ? CHECKERS_BITBOARD(pos) : COLOR_BITBOARD(pos, nstm_c);
    Bitboard pawns_bb = COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, PIECE_TYPE_PAWN);
    Bitboard pawns_on_7_bb = pawns_bb & rank_7_bb;
    Bitboard pawns_not_on_7_bb = pawns_bb & ~rank_7_bb;

    // Single and double pushes, no promotions
    if (mgt != MOVE_GENERATION_TYPE_CAPTURES) {
        Bitboard single_bb = shift_bitboard(pawns_not_on_7_bb, up_d) & empty_bb;
        Bitboard double_bb = shift_bitboard(single_bb & rank_3_bb, up_d) & empty_bb;

        if (mgt == MOVE_GENERATION_TYPE_EVASIONS) {
            single_bb &= target_bb;
            double_bb &= target_bb;
        }

        for (; single_bb; POP_LSB(single_bb)) {
            Square to_sq = (Square)LSB(single_bb);
            (smvs++)->mv = NORMAL_MOVE(to_sq - up_d, to_sq);
        }
        for (; double_bb; POP_LSB(double_bb)) {
            Square to_sq = (Square)LSB(double_bb);
            (smvs++)->mv = NORMAL_MOVE(to_sq - 2 * up_d, to_sq);
        }
    }

    if (pawns_on_7_bb) {
        if (mgt != MOVE_GENERATION_TYPE_QUIETS) {
            Bitboard right_bb = shift_bitboard(pawns_on_7_bb, up_right_d) & enemies_bb;
            Bitboard left_bb = shift_bitboard(pawns_on_7_bb, up_left_d) & enemies_bb;

            for (; right_bb; POP_LSB(right_bb))
                smvs = make_promotions(smvs, (Square)LSB(right_bb), up_right_d, mgt, true);
            for (; left_bb; POP_LSB(left_bb))
                smvs = make_promotions(smvs, (Square)LSB(left_bb), up_left_d, mgt, true);
        }

        Bitboard push_bb = shift_bitboard(pawns_on_7_bb, up_d) & empty_bb;

        if (mgt == MOVE_GENERATION_TYPE_EVASIONS)
            push_bb &= target_bb;

        for (; push_bb; POP_LSB(push_bb))
            smvs = make_promotions(smvs, (Square)LSB(push_bb), up_d, mgt, false);
    }

    if (mgt == MOVE_GENERATION_TYPE_CAPTURES || mgt == MOVE_GENERATION_TYPE_EVASIONS || mgt == MOVE_GENERATION_TYPE_NON_EVASIONS) {
        Bitboard right_bb = shift_bitboard(pawns_not_on_7_bb, up_right_d) & enemies_bb;
        Bitboard left_bb = shift_bitboard(pawns_not_on_7_bb, up_left_d) & enemies_bb;

        for (; right_bb; POP_LSB(right_bb)) {
            Square to_sq = (Square)LSB(right_bb);
            (smvs++)->mv = NORMAL_MOVE(to_sq - up_right_d, to_sq);
        }
        for (; left_bb; POP_LSB(left_bb)) {
            Square to_sq = (Square)LSB(left_bb);
            (smvs++)->mv = NORMAL_MOVE(to_sq - up_left_d, to_sq);
        }

        Square ep_sq = EN_PASSANT_SQUARE(pos);
        if (ep_sq != NO_SQUARE) {
            Bitboard attackers_bb = pawns_not_on_7_bb & PAWN_ATTACKS_BITBOARD(nstm_c, ep_sq);

            for (; attackers_bb; POP_LSB(attackers_bb))
                (smvs++)->mv = EN_PASSANT_MOVE((Square)LSB(attackers_bb), ep_sq);
        }
    }

    return smvs;
}

ALWAYS_INLINE ScoredMove *generate_piece_moves(const Position *pos, ScoredMove *smvs,
                                            Color stm_c, PieceType pt, Bitboard target_bb) {
    Bitboard occ_bb = OCCUPANCY_BITBOARD(pos);

    for (Bitboard pieces_bb = COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, pt); pieces_bb; POP_LSB(pieces_bb)) {
        Square from_sq = (Square)LSB(pieces_bb);

        for (Bitboard attacks_bb = piece_attacks(pt, from_sq, occ_bb) & target_bb; attacks_bb; POP_LSB(attacks_bb))
            (smvs++)->mv = NORMAL_MOVE(from_sq, (Square)LSB(attacks_bb));
    }

    return smvs;
}

ALWAYS_INLINE ScoredMove *generate_all(const Position *pos, ScoredMove *smvs, Color stm_c, MoveGenerationType mgt) {
    Color nstm_c = OPPOSITE_COLOR(stm_c);
    Square king_sq = KING_SQUARE(pos, stm_c);
    Bitboard checkers_bb = CHECKERS_BITBOARD(pos);
    Bitboard target_bb = BITBOARD_NONE;

    if (mgt != MOVE_GENERATION_TYPE_EVASIONS || COUNT_BITS(checkers_bb) == 1) {
        if (mgt == MOVE_GENERATION_TYPE_EVASIONS) {
            Square checker_sq = (Square)LSB(checkers_bb);
            target_bb = SEGMENT_BITBOARD(king_sq, checker_sq) | BITBOARD_FROM_SQUARE(checker_sq);
        } else if (mgt == MOVE_GENERATION_TYPE_NON_EVASIONS) {
            target_bb = ~COLOR_BITBOARD(pos, stm_c);
        } else if (mgt == MOVE_GENERATION_TYPE_CAPTURES) {
            target_bb = COLOR_BITBOARD(pos, nstm_c);
        } else {
            target_bb = ~OCCUPANCY_BITBOARD(pos);
        }

        smvs = generate_pawn_moves(pos, smvs, stm_c, mgt, target_bb);
        smvs = generate_piece_moves(pos, smvs, stm_c, PIECE_TYPE_KNIGHT, target_bb);
        smvs = generate_piece_moves(pos, smvs, stm_c, PIECE_TYPE_BISHOP, target_bb);
        smvs = generate_piece_moves(pos, smvs, stm_c, PIECE_TYPE_ROOK, target_bb);
        smvs = generate_piece_moves(pos, smvs, stm_c, PIECE_TYPE_QUEEN, target_bb);
    }

    Bitboard king_target_bb = mgt == MOVE_GENERATION_TYPE_EVASIONS ? ~COLOR_BITBOARD(pos, stm_c) : target_bb;
    for (Bitboard attacks_bb = KING_ATTACKS_BITBOARD(king_sq) & king_target_bb; attacks_bb; POP_LSB(attacks_bb))
        (smvs++)->mv = NORMAL_MOVE(king_sq, (Square)LSB(attacks_bb));

    if (mgt == MOVE_GENERATION_TYPE_QUIETS || mgt == MOVE_GENERATION_TYPE_NON_EVASIONS) {
        CastlingRights oo_cr = stm_c == COLOR_WHITE ? CASTLING_RIGHTS_WHITE_OO : CASTLING_RIGHTS_BLACK_OO;
        CastlingRights ooo_cr = stm_c == COLOR_WHITE ? CASTLING_RIGHTS_WHITE_OOO : CASTLING_RIGHTS_BLACK_OOO;

        if (CAN_CASTLE(pos, oo_cr) && !CASTLING_IMPEDED(pos, oo_cr))
            (smvs++)->mv = CASTLING_MOVE(king_sq, RELATIVE_SQUARE(stm_c, SQUARE_G1));
        if (CAN_CASTLE(pos, ooo_cr) && !CASTLING_IMPEDED(pos, ooo_cr))
            (smvs++)->mv = CASTLING_MOVE(king_sq, RELATIVE_SQUARE(stm_c, SQUARE_C1));
    }

    return smvs;
}

ScoredMove *generate_captures(const Position *pos, ScoredMove *smvs) {
    return SIDE_TO_MOVE(pos) == COLOR_WHITE
         ? generate_all(pos, smvs, COLOR_WHITE, MOVE_GENERATION_TYPE_CAPTURES)
         : generate_all(pos, smvs, COLOR_BLACK, MOVE_GENERATION_TYPE_CAPTURES);
}

ScoredMove *generate_quiets(const Position *pos, ScoredMove *smvs) {
    return SIDE_TO_MOVE(pos) == COLOR_WHITE
         ? generate_all(pos, smvs, COLOR_WHITE, MOVE_GENERATION_TYPE_QUIETS)
         : generate_all(pos, smvs, COLOR_BLACK, MOVE_GENERATION_TYPE_QUIETS);
}

ScoredMove *generate_evasions(const Position *pos, ScoredMove *smvs) {
    return SIDE_TO_MOVE(pos) == COLOR_WHITE
         ? generate_all(pos, smvs, COLOR_WHITE, MOVE_GENERATION_TYPE_EVASIONS)
         : generate_all(pos, smvs, COLOR_BLACK, MOVE_GENERATION_TYPE_EVASIONS);
}

ScoredMove *generate_non_evasions(const Position *pos, ScoredMove *smvs) {
    return SIDE_TO_MOVE(pos) == COLOR_WHITE
         ? generate_all(pos, smvs, COLOR_WHITE, MOVE_GENERATION_TYPE_NON_EVASIONS)
         : generate_all(pos, smvs, COLOR_BLACK, MOVE_GENERATION_TYPE_NON_EVASIONS);
}

ScoredMove *generate_legal(const Position *pos, ScoredMove *smvs) {
    Color stm_c = SIDE_TO_MOVE(pos);
    Bitboard pinned_bb = BLOCKERS_BITBOARD(pos, stm_c) & COLOR_BITBOARD(pos, stm_c);
    Square king_sq = KING_SQUARE(pos, stm_c);

    ScoredMove *end = CHECKERS_BITBOARD(pos) ? generate_evasions(pos, smvs) : generate_non_evasions(pos, smvs);

    for (ScoredMove *cur = smvs; cur != end;) {
        Square from_sq = ORIGIN_SQUARE_FROM_MOVE(cur->mv);
        bool may_be_illegal = TEST_BIT(pinned_bb, from_sq)
                           || from_sq == king_sq
                           || MOVE_TYPE_FROM_MOVE(cur->mv) == MOVE_TYPE_EN_PASSANT;

        if (may_be_illegal && !is_legal(pos, cur->mv))
            *cur = *(--end);  
        else
            cur++;
    }

    return end;
}