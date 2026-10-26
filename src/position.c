#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "position.h"
#include "lookups.h"
#include "move_generation.h"

static const char PIECE_CHARS[] = "PNBRQKpnbrqk";

const Score PIECE_VALUES_[NO_PIECE + 1] = {
    SCORE_PAWN, SCORE_KNIGHT, SCORE_BISHOP, SCORE_ROOK, SCORE_QUEEN, 0,
    SCORE_PAWN, SCORE_KNIGHT, SCORE_BISHOP, SCORE_ROOK, SCORE_QUEEN, 0,
    0
};


static inline void put_piece(Position *pos, Piece p, Square sq) {
    PIECE_ON_SQUARE(pos, sq) = p;
    SET_BIT(PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_FROM_PIECE(p)), sq);
    SET_BIT(COLOR_BITBOARD(pos, COLOR_FROM_PIECE(p)), sq);
    SET_BIT(OCCUPANCY_BITBOARD(pos), sq);
    PIECE_COUNT(pos, p)++;
}

static inline void remove_piece(Position *pos, Square sq) {
    Piece p = PIECE_ON_SQUARE(pos, sq);
    UNSET_BIT(COLOR_BITBOARD(pos, COLOR_FROM_PIECE(p)), sq);
    UNSET_BIT(PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_FROM_PIECE(p)), sq);
    UNSET_BIT(OCCUPANCY_BITBOARD(pos), sq);
    PIECE_ON_SQUARE(pos, sq) = NO_PIECE;
    PIECE_COUNT(pos, p)--;
}

static inline void move_piece(Position *pos, Square from_sq, Square to_sq) {
    Piece p = PIECE_ON_SQUARE(pos, from_sq);
    Bitboard from_to_bb = BITBOARD_FROM_SQUARE(from_sq) | BITBOARD_FROM_SQUARE(to_sq);

    PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_FROM_PIECE(p)) ^= from_to_bb;
    COLOR_BITBOARD(pos, COLOR_FROM_PIECE(p)) ^= from_to_bb;
    OCCUPANCY_BITBOARD(pos) ^= from_to_bb;
    PIECE_ON_SQUARE(pos, from_sq) = NO_PIECE;
    PIECE_ON_SQUARE(pos, to_sq) = p;
}

Bitboard attackers_to(const Position *pos, Square sq, Bitboard occ_bb) {
    Bitboard queens_bb = PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_QUEEN);

    return (PAWN_ATTACKS_BITBOARD(COLOR_BLACK, sq) & COLOR_PIECE_TYPE_BITBOARD(pos, COLOR_WHITE, PIECE_TYPE_PAWN))
         | (PAWN_ATTACKS_BITBOARD(COLOR_WHITE, sq) & COLOR_PIECE_TYPE_BITBOARD(pos, COLOR_BLACK, PIECE_TYPE_PAWN))
         | (KNIGHT_ATTACKS_BITBOARD(sq) & PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_KNIGHT))
         | (BISHOP_ATTACKS_BITBOARD(sq, occ_bb) & (PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_BISHOP) | queens_bb))
         | (ROOK_ATTACKS_BITBOARD(sq, occ_bb) & (PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_ROOK) | queens_bb))
         | (KING_ATTACKS_BITBOARD(sq) & PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_KING));
}


static const CastlingRights CASTLING_RIGHTS_MASKS[NO_SQUARE] = {
    [SQUARE_A1] = CASTLING_RIGHTS_WHITE_OOO,
    [SQUARE_E1] = CASTLING_RIGHTS_WHITE,
    [SQUARE_H1] = CASTLING_RIGHTS_WHITE_OO,
    [SQUARE_A8] = CASTLING_RIGHTS_BLACK_OOO,
    [SQUARE_E8] = CASTLING_RIGHTS_BLACK,
    [SQUARE_H8] = CASTLING_RIGHTS_BLACK_OO,
};

static inline void castling_rook_squares(Square king_to_sq, Square *rook_from_sq, Square *rook_to_sq) {
    bool kingside = FILE_FROM_SQUARE(king_to_sq) == FILE_G;
    *rook_from_sq = (Square)(kingside ? king_to_sq + 1 : king_to_sq - 2);
    *rook_to_sq   = (Square)(kingside ? king_to_sq - 1 : king_to_sq + 1);
}

static void update_slider_blockers(Position *pos, Color c) {
    Square king_sq = KING_SQUARE(pos, c);
    Color opp_c = OPPOSITE_COLOR(c);

    BLOCKERS_BITBOARD(pos, c) = BITBOARD_NONE;
    PINNERS_BITBOARD(pos, opp_c) = BITBOARD_NONE;

    Bitboard queens_bb = PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_QUEEN);
    Bitboard snipers_bb = ((ROOK_ATTACKS_BITBOARD(king_sq, BITBOARD_NONE) & (PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_ROOK) | queens_bb))
                         | (BISHOP_ATTACKS_BITBOARD(king_sq, BITBOARD_NONE) & (PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_BISHOP) | queens_bb)))
                        & COLOR_BITBOARD(pos, opp_c);
    Bitboard occ_bb = OCCUPANCY_BITBOARD(pos) ^ snipers_bb;

    while (snipers_bb) {
        Square sniper_sq = (Square)LSB(snipers_bb);
        POP_LSB(snipers_bb);

        Bitboard between_bb = SEGMENT_BITBOARD(king_sq, sniper_sq) & occ_bb;

        if (between_bb && COUNT_BITS(between_bb) == 1) {
            BLOCKERS_BITBOARD(pos, c) |= between_bb;
            if (between_bb & COLOR_BITBOARD(pos, c))
                SET_BIT(PINNERS_BITBOARD(pos, opp_c), sniper_sq);
        }
    }
}

static void set_check_squares(Position *pos) {
    Color nstm_c = OPPOSITE_COLOR(SIDE_TO_MOVE(pos));
    Square king_sq = KING_SQUARE(pos, nstm_c);
    Bitboard occ_bb = OCCUPANCY_BITBOARD(pos);

    CHECK_SQUARES_BITBOARD(pos, PIECE_TYPE_PAWN) = PAWN_ATTACKS_BITBOARD(nstm_c, king_sq);
    CHECK_SQUARES_BITBOARD(pos, PIECE_TYPE_KNIGHT) = KNIGHT_ATTACKS_BITBOARD(king_sq);
    CHECK_SQUARES_BITBOARD(pos, PIECE_TYPE_BISHOP) = BISHOP_ATTACKS_BITBOARD(king_sq, occ_bb);
    CHECK_SQUARES_BITBOARD(pos, PIECE_TYPE_ROOK) = ROOK_ATTACKS_BITBOARD(king_sq, occ_bb);
    CHECK_SQUARES_BITBOARD(pos, PIECE_TYPE_QUEEN) = CHECK_SQUARES_BITBOARD(pos, PIECE_TYPE_BISHOP)
                                                  | CHECK_SQUARES_BITBOARD(pos, PIECE_TYPE_ROOK);
    CHECK_SQUARES_BITBOARD(pos, PIECE_TYPE_KING) = BITBOARD_NONE;
}

static void set_check_info(Position *pos) {
    update_slider_blockers(pos, COLOR_WHITE);
    update_slider_blockers(pos, COLOR_BLACK);
    set_check_squares(pos);
}

static void set_state(Position *pos) {
    Color stm_c = SIDE_TO_MOVE(pos);

    KEY(pos) = 0;
    for (Bitboard occ_bb = OCCUPANCY_BITBOARD(pos); occ_bb; POP_LSB(occ_bb)) {
        Square sq = (Square)LSB(occ_bb);
        KEY(pos) ^= PIECE_SQUARE_KEY(PIECE_ON_SQUARE(pos, sq), sq);
    }

    PAWN_KEY(pos) = 0;
    for (Bitboard pawns_bb = PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_PAWN); pawns_bb; POP_LSB(pawns_bb)) {
        Square sq = (Square)LSB(pawns_bb);
        PAWN_KEY(pos) ^= PIECE_SQUARE_KEY(PIECE_ON_SQUARE(pos, sq), sq);
    }

    if (EN_PASSANT_SQUARE(pos) != NO_SQUARE)
        KEY(pos) ^= EN_PASSANT_KEY(FILE_FROM_SQUARE(EN_PASSANT_SQUARE(pos)));

    if (stm_c == COLOR_BLACK)
        KEY(pos) ^= SIDE_TO_MOVE_KEY;

    KEY(pos) ^= CASTLING_RIGHTS_KEY(CASTLING_RIGHTS(pos));

    CHECKERS_BITBOARD(pos) = attackers_to(pos, KING_SQUARE(pos, stm_c), OCCUPANCY_BITBOARD(pos))
                           & COLOR_BITBOARD(pos, OPPOSITE_COLOR(stm_c));

    set_check_info(pos);
}

static void clear_illegal_en_passant(Position *pos) {
    Square ep_sq = EN_PASSANT_SQUARE(pos);

    if (ep_sq == NO_SQUARE)
        return;

    Color stm_c = SIDE_TO_MOVE(pos);
    Bitboard capturers_bb = PAWN_ATTACKS_BITBOARD(OPPOSITE_COLOR(stm_c), ep_sq)
                          & COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, PIECE_TYPE_PAWN);

    for (; capturers_bb; POP_LSB(capturers_bb))
        if (is_legal(pos, EN_PASSANT_MOVE((Square)LSB(capturers_bb), ep_sq)))
            return;

    KEY(pos) ^= EN_PASSANT_KEY(FILE_FROM_SQUARE(ep_sq));
    EN_PASSANT_SQUARE(pos) = NO_SQUARE;
}


void set_position_from_fen(Position *pos, const char *fen, PlyState *st) {
    memset(pos, 0, sizeof(*pos));
    memset(st, 0, sizeof(*st));
    pos->st = st;

    for (Square sq = SQUARE_A1; sq < NO_SQUARE; sq++) {
        PIECE_ON_SQUARE(pos, sq) = NO_PIECE;
    }

    EN_PASSANT_SQUARE(pos) = NO_SQUARE;
    CAPTURED_PIECE(pos) = NO_PIECE;

    const char *ch = fen;

    i8 f = FILE_A;
    i8 r = RANK_8;

    for (; *ch && *ch != ' '; ch++) {
        if (*ch == '/') {
            r--;
            f = FILE_A;
        } else if (*ch >= '1' && *ch <= '8') {
            f += *ch - '0';
        } else {
            Piece p = (Piece)(strchr(PIECE_CHARS, *ch) - PIECE_CHARS);
            put_piece(pos, p, SQUARE_FROM_FILE_RANK(f, r));
            f++;
        }
    }

    ch++;
    SIDE_TO_MOVE(pos) = (*ch == 'w') ? COLOR_WHITE : COLOR_BLACK;
    ch += 2;

    for (; *ch && *ch != ' '; ch++) {
        CastlingRights cr;
        Square rook_sq;

        switch (*ch) {
            case 'K': cr = CASTLING_RIGHTS_WHITE_OO;  rook_sq = SQUARE_H1; break;
            case 'Q': cr = CASTLING_RIGHTS_WHITE_OOO; rook_sq = SQUARE_A1; break;
            case 'k': cr = CASTLING_RIGHTS_BLACK_OO;  rook_sq = SQUARE_H8; break;
            case 'q': cr = CASTLING_RIGHTS_BLACK_OOO; rook_sq = SQUARE_A8; break;
            default: continue;
        }

        Color c = (cr & CASTLING_RIGHTS_WHITE) ? COLOR_WHITE : COLOR_BLACK;

        if (PIECE_ON_SQUARE(pos, RELATIVE_SQUARE(c, SQUARE_E1)) == PIECE_FROM_COLOR_PIECE_TYPE(c, PIECE_TYPE_KING)
         && PIECE_ON_SQUARE(pos, rook_sq) == PIECE_FROM_COLOR_PIECE_TYPE(c, PIECE_TYPE_ROOK))
            CASTLING_RIGHTS(pos) |= cr;
    }

    ch++;
    if (*ch != '-') {
        Square ep_sq = SQUARE_FROM_FILE_RANK(ch[0] - 'a', ch[1] - '1');
        Color stm_c = SIDE_TO_MOVE(pos);
        Color nstm_c = OPPOSITE_COLOR(stm_c);
        Square pushed_pawn_sq = (stm_c == COLOR_WHITE) ? ep_sq - 8 : ep_sq + 8;

        bool can_capture = PAWN_ATTACKS_BITBOARD(nstm_c, ep_sq) & COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, PIECE_TYPE_PAWN);
        bool pawn_pushed = PIECE_ON_SQUARE(pos, pushed_pawn_sq) == PIECE_FROM_COLOR_PIECE_TYPE(nstm_c, PIECE_TYPE_PAWN);

        if (can_capture && pawn_pushed)
            EN_PASSANT_SQUARE(pos) = ep_sq;

        ch += 2;
    } else {
        ch++;
    }

    int halfmv_clock = 0;
    int fullmv_nb = 1;

    if (*ch == ' ') {
        char *end;
        halfmv_clock = (int)strtol(ch, &end, 10);
        fullmv_nb = (int)strtol(end, &end, 10);
        if (fullmv_nb < 1) fullmv_nb = 1;
    }

    HALFMOVE_CLOCK(pos) = (u8)halfmv_clock;
    GAME_PLY(pos) = (u16)(2 * (fullmv_nb - 1) + (SIDE_TO_MOVE(pos) == COLOR_BLACK));

    set_state(pos);
    clear_illegal_en_passant(pos);
}

void fen_from_position(const Position *pos, char *buff) {
    char *ch = buff;

    for (i8 r = RANK_8; r >= RANK_1; r--) {
        u8 empty = 0;

        for (i8 f = FILE_A; f <= FILE_H; f++) {
            Piece p = PIECE_ON_SQUARE(pos, SQUARE_FROM_FILE_RANK(f, r));

            if (p == NO_PIECE) {
                empty++;
            } else {
                if (empty) *ch++ = (char)('0' + empty);
                *ch++ = PIECE_CHARS[p];
                empty = 0;
            }
        }

        if (empty) *ch++ = (char)('0' + empty);
        if (r > RANK_1) *ch++ = '/';
    }

    *ch++ = ' ';
    *ch++ = SIDE_TO_MOVE(pos) == COLOR_WHITE ? 'w' : 'b';
    *ch++ = ' ';

    if (CASTLING_RIGHTS(pos) == CASTLING_RIGHTS_NONE) {
        *ch++ = '-';
    } else {
        if (CAN_CASTLE(pos, CASTLING_RIGHTS_WHITE_OO))  *ch++ = 'K';
        if (CAN_CASTLE(pos, CASTLING_RIGHTS_WHITE_OOO)) *ch++ = 'Q';
        if (CAN_CASTLE(pos, CASTLING_RIGHTS_BLACK_OO))  *ch++ = 'k';
        if (CAN_CASTLE(pos, CASTLING_RIGHTS_BLACK_OOO)) *ch++ = 'q';
    }
    *ch++ = ' ';

    if (EN_PASSANT_SQUARE(pos) == NO_SQUARE) {
        *ch++ = '-';
    } else {
        *ch++ = (char)('a' + FILE_FROM_SQUARE(EN_PASSANT_SQUARE(pos)));
        *ch++ = (char)('1' + RANK_FROM_SQUARE(EN_PASSANT_SQUARE(pos)));
    }

    sprintf(ch, " %u %u", (unsigned)HALFMOVE_CLOCK(pos),
            1u + (GAME_PLY(pos) - (SIDE_TO_MOVE(pos) == COLOR_BLACK)) / 2u);
}


bool is_legal(const Position *pos, Move mv) {
    Color stm_c = SIDE_TO_MOVE(pos);
    Color nstm_c = OPPOSITE_COLOR(stm_c);
    Square from_sq = ORIGIN_SQUARE_FROM_MOVE(mv);
    Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);
    MoveType mt = MOVE_TYPE_FROM_MOVE(mv);
    Square king_sq = KING_SQUARE(pos, stm_c);

    if (mt == MOVE_TYPE_EN_PASSANT) {
        Square captured_sq = (stm_c == COLOR_WHITE) ? to_sq - 8 : to_sq + 8;
        Bitboard occ_bb = (OCCUPANCY_BITBOARD(pos) ^ BITBOARD_FROM_SQUARE(from_sq) ^ BITBOARD_FROM_SQUARE(captured_sq))
                        | BITBOARD_FROM_SQUARE(to_sq);
        Bitboard queens_bb = COLOR_PIECE_TYPE_BITBOARD(pos, nstm_c, PIECE_TYPE_QUEEN);

        return !(ROOK_ATTACKS_BITBOARD(king_sq, occ_bb) & (COLOR_PIECE_TYPE_BITBOARD(pos, nstm_c, PIECE_TYPE_ROOK) | queens_bb))
            && !(BISHOP_ATTACKS_BITBOARD(king_sq, occ_bb) & (COLOR_PIECE_TYPE_BITBOARD(pos, nstm_c, PIECE_TYPE_BISHOP) | queens_bb));
    }

    if (mt == MOVE_TYPE_CASTLING) {
        Square crossed_sq = (Square)((from_sq + to_sq) / 2);
        Bitboard enemies_bb = COLOR_BITBOARD(pos, nstm_c);

        return !(attackers_to(pos, crossed_sq, OCCUPANCY_BITBOARD(pos)) & enemies_bb)
            && !(attackers_to(pos, to_sq, OCCUPANCY_BITBOARD(pos)) & enemies_bb);
    }

    if (PIECE_TYPE_FROM_PIECE(MOVED_PIECE(pos, mv)) == PIECE_TYPE_KING)
        return !(attackers_to(pos, to_sq, OCCUPANCY_BITBOARD(pos) ^ BITBOARD_FROM_SQUARE(from_sq))
                 & COLOR_BITBOARD(pos, nstm_c));

    return !TEST_BIT(BLOCKERS_BITBOARD(pos, stm_c), from_sq)
        || TEST_BIT(LINE_BITBOARD(from_sq, to_sq), king_sq);
}

bool see_ge(const Position *pos, Move mv, Score threshold) {
    if (MOVE_TYPE_FROM_MOVE(mv) != MOVE_TYPE_NORMAL)
        return SCORE_ZERO >= threshold;

    Square from_sq = ORIGIN_SQUARE_FROM_MOVE(mv);
    Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);

    Score swap = PIECE_VALUE(PIECE_ON_SQUARE(pos, to_sq)) - threshold;
    if (swap < 0)
        return false;

    swap = PIECE_VALUE(PIECE_ON_SQUARE(pos, from_sq)) - swap;
    if (swap <= 0)
        return true;

    Bitboard occ_bb = OCCUPANCY_BITBOARD(pos) ^ BITBOARD_FROM_SQUARE(from_sq) ^ BITBOARD_FROM_SQUARE(to_sq);
    Color stm_c = SIDE_TO_MOVE(pos);
    Bitboard attackers_bb = attackers_to(pos, to_sq, occ_bb);
    Bitboard bishops_queens_bb = PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_BISHOP) | PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_QUEEN);
    Bitboard rooks_queens_bb = PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_ROOK) | PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_QUEEN);
    bool res = true;    
    while (true) {
        stm_c = OPPOSITE_COLOR(stm_c);
        attackers_bb &= occ_bb;

        Bitboard stm_attackers_bb = attackers_bb & COLOR_BITBOARD(pos, stm_c);
        if (!stm_attackers_bb)
            break;

        if (PINNERS_BITBOARD(pos, OPPOSITE_COLOR(stm_c)) & occ_bb) {
            stm_attackers_bb &= ~BLOCKERS_BITBOARD(pos, stm_c);
            if (!stm_attackers_bb)
                break;
        }

        res = !res;

        Bitboard bb;

        if ((bb = stm_attackers_bb & PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_PAWN))) {
            if ((swap = SCORE_PAWN - swap) < res)
                break;
            occ_bb ^= bb & (~bb + 1);
            attackers_bb |= BISHOP_ATTACKS_BITBOARD(to_sq, occ_bb) & bishops_queens_bb;
        }
        else if ((bb = stm_attackers_bb & PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_KNIGHT))) {
            if ((swap = SCORE_KNIGHT - swap) < res)
                break;
            occ_bb ^= bb & (~bb + 1);
        }
        else if ((bb = stm_attackers_bb & PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_BISHOP))) {
            if ((swap = SCORE_BISHOP - swap) < res)
                break;
            occ_bb ^= bb & (~bb + 1);
            attackers_bb |= BISHOP_ATTACKS_BITBOARD(to_sq, occ_bb) & bishops_queens_bb;
        }
        else if ((bb = stm_attackers_bb & PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_ROOK))) {
            if ((swap = SCORE_ROOK - swap) < res)
                break;
            occ_bb ^= bb & (~bb + 1);
            attackers_bb |= ROOK_ATTACKS_BITBOARD(to_sq, occ_bb) & rooks_queens_bb;
        }
        else if ((bb = stm_attackers_bb & PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_QUEEN))) {
            if ((swap = SCORE_QUEEN - swap) < res)
                break;
            occ_bb ^= bb & (~bb + 1);
            attackers_bb |= (BISHOP_ATTACKS_BITBOARD(to_sq, occ_bb) & bishops_queens_bb)
                          | (ROOK_ATTACKS_BITBOARD(to_sq, occ_bb) & rooks_queens_bb);
        }
        else {
            return (attackers_bb & ~COLOR_BITBOARD(pos, stm_c)) ? !res : res;
        }
    }

    return res;
}

bool gives_check(const Position *pos, Move mv) {
    Color stm_c = SIDE_TO_MOVE(pos);
    Color nstm_c = OPPOSITE_COLOR(stm_c);
    Square from_sq = ORIGIN_SQUARE_FROM_MOVE(mv);
    Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);
    Square king_sq = KING_SQUARE(pos, nstm_c);
    MoveType mt = MOVE_TYPE_FROM_MOVE(mv);

    if (CHECK_SQUARES_BITBOARD(pos, PIECE_TYPE_FROM_PIECE(PIECE_ON_SQUARE(pos, from_sq))) & BITBOARD_FROM_SQUARE(to_sq))
        return true;

    if (BLOCKERS_BITBOARD(pos, nstm_c) & BITBOARD_FROM_SQUARE(from_sq))
        return !(LINE_BITBOARD(from_sq, to_sq) & BITBOARD_FROM_SQUARE(king_sq)) || mt == MOVE_TYPE_CASTLING;

    Bitboard occ_bb = OCCUPANCY_BITBOARD(pos) ^ BITBOARD_FROM_SQUARE(from_sq);

    switch (mt) {
        case MOVE_TYPE_NORMAL:
            return false;

        case MOVE_TYPE_PROMOTION: {
            Bitboard attacks_bb;
            switch (PROMOTION_PIECE_TYPE_FROM_MOVE(mv)) {
                case PIECE_TYPE_KNIGHT: attacks_bb = KNIGHT_ATTACKS_BITBOARD(to_sq); break;
                case PIECE_TYPE_BISHOP: attacks_bb = BISHOP_ATTACKS_BITBOARD(to_sq, occ_bb); break;
                case PIECE_TYPE_ROOK:   attacks_bb = ROOK_ATTACKS_BITBOARD(to_sq, occ_bb); break;
                default:                attacks_bb = QUEEN_ATTACKS_BITBOARD(to_sq, occ_bb); break;
            }
            return (attacks_bb & BITBOARD_FROM_SQUARE(king_sq)) != 0;
        }

        case MOVE_TYPE_EN_PASSANT: {
            Square captured_sq = (Square)(stm_c == COLOR_WHITE ? to_sq - 8 : to_sq + 8);
            Bitboard b = (occ_bb ^ BITBOARD_FROM_SQUARE(captured_sq)) | BITBOARD_FROM_SQUARE(to_sq);
            Bitboard rooks_queens_bb = COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, PIECE_TYPE_ROOK)
                                     | COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, PIECE_TYPE_QUEEN);
            Bitboard bishops_queens_bb = COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, PIECE_TYPE_BISHOP)
                                       | COLOR_PIECE_TYPE_BITBOARD(pos, stm_c, PIECE_TYPE_QUEEN);
            return ((ROOK_ATTACKS_BITBOARD(king_sq, b) & rooks_queens_bb)
                  | (BISHOP_ATTACKS_BITBOARD(king_sq, b) & bishops_queens_bb)) != 0;
        }

        default: {
            Square rook_from_sq, rook_to_sq;
            castling_rook_squares(to_sq, &rook_from_sq, &rook_to_sq);
            return (CHECK_SQUARES_BITBOARD(pos, PIECE_TYPE_ROOK) & BITBOARD_FROM_SQUARE(rook_to_sq)) != 0;
        }
    }
}

bool is_draw(const Position *pos, u16 ply) {
    if (HALFMOVE_CLOCK(pos) > 99) {
        if (!CHECKERS_BITBOARD(pos))
            return true;
        ScoredMove smvs[MAX_MOVES];
        if (generate_legal(pos, smvs) != smvs)
            return true;
    }

    i16 repetition = pos->st->repetition;
    return repetition && repetition < (i16)ply;
}

static bool list_contains(const ScoredMove *begin, const ScoredMove *end, Move mv) {
    for (const ScoredMove *cur = begin; cur < end; cur++)
        if (cur->mv == mv)
            return true;
    return false;
}

bool is_pseudo_legal(const Position *pos, Move mv) {
    Color stm_c = SIDE_TO_MOVE(pos);
    Square from_sq = ORIGIN_SQUARE_FROM_MOVE(mv);
    Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);
    MoveType mt = MOVE_TYPE_FROM_MOVE(mv);
    Piece p = PIECE_ON_SQUARE(pos, from_sq);

    if (!MOVE_IS_OK(mv))
        return false;

    if (mt != MOVE_TYPE_NORMAL) {
        ScoredMove smvs[MAX_MOVES];
        ScoredMove *end = CHECKERS_BITBOARD(pos) ? generate_evasions(pos, smvs) : generate_non_evasions(pos, smvs);
        return list_contains(smvs, end, mv);
    }

    if ((mv >> 12) & 3)
        return false;

    if (p == NO_PIECE || COLOR_FROM_PIECE(p) != stm_c)
        return false;

    if (TEST_BIT(COLOR_BITBOARD(pos, stm_c), to_sq))
        return false;

    PieceType pt = PIECE_TYPE_FROM_PIECE(p);

    if (pt == PIECE_TYPE_PAWN) {
        if ((BITBOARD_FROM_RANK(RANK_1) | BITBOARD_FROM_RANK(RANK_8)) & BITBOARD_FROM_SQUARE(to_sq))
            return false;

        i8 push = stm_c == COLOR_WHITE ? 8 : -8;
        bool is_capture = TEST_BIT(PAWN_ATTACKS_BITBOARD(stm_c, from_sq) & COLOR_BITBOARD(pos, OPPOSITE_COLOR(stm_c)), to_sq);
        bool is_single_push = from_sq + push == to_sq && IS_SQUARE_EMPTY(pos, to_sq);
        bool is_double_push = from_sq + 2 * push == to_sq
                           && RANK_FROM_SQUARE(RELATIVE_SQUARE(stm_c, from_sq)) == RANK_2
                           && IS_SQUARE_EMPTY(pos, to_sq)
                           && IS_SQUARE_EMPTY(pos, to_sq - push);

        if (!is_capture && !is_single_push && !is_double_push)
            return false;
    }
    else {
        Bitboard attacks_bb;
        Bitboard occ_bb = OCCUPANCY_BITBOARD(pos);

        switch (pt) {
            case PIECE_TYPE_KNIGHT: attacks_bb = KNIGHT_ATTACKS_BITBOARD(from_sq); break;
            case PIECE_TYPE_BISHOP: attacks_bb = BISHOP_ATTACKS_BITBOARD(from_sq, occ_bb); break;
            case PIECE_TYPE_ROOK:   attacks_bb = ROOK_ATTACKS_BITBOARD(from_sq, occ_bb); break;
            case PIECE_TYPE_QUEEN:  attacks_bb = QUEEN_ATTACKS_BITBOARD(from_sq, occ_bb); break;
            default:                attacks_bb = KING_ATTACKS_BITBOARD(from_sq); break;
        }

        if (!TEST_BIT(attacks_bb, to_sq))
            return false;
    }

    Bitboard checkers_bb = CHECKERS_BITBOARD(pos);
    if (checkers_bb) {
        if (pt != PIECE_TYPE_KING) {
            if (checkers_bb & (checkers_bb - 1))
                return false;

            Square checker_sq = (Square)LSB(checkers_bb);
            if (!TEST_BIT(SEGMENT_BITBOARD(KING_SQUARE(pos, stm_c), checker_sq) | checkers_bb, to_sq))
                return false;
        }
        else if (attackers_to(pos, to_sq, OCCUPANCY_BITBOARD(pos) ^ BITBOARD_FROM_SQUARE(from_sq))
                 & COLOR_BITBOARD(pos, OPPOSITE_COLOR(stm_c)))
            return false;
    }

    return true;
}

void do_move(Position *pos, Move mv, PlyState *new_st) {
    Key k = KEY(pos) ^ SIDE_TO_MOVE_KEY;

    memcpy(new_st, pos->st, offsetof(PlyState, k));
    new_st->prev_st = pos->st;
    new_st->dirty.count = 0;
    new_st->acc.computed[COLOR_WHITE] = false;
    new_st->acc.computed[COLOR_BLACK] = false;
    pos->st = new_st;

    GAME_PLY(pos)++;
    HALFMOVE_CLOCK(pos)++;
    pos->st->plies_from_null++;

    Color stm_c = SIDE_TO_MOVE(pos);
    Color nstm_c = OPPOSITE_COLOR(stm_c);
    Square from_sq = ORIGIN_SQUARE_FROM_MOVE(mv);
    Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);
    MoveType mt = MOVE_TYPE_FROM_MOVE(mv);
    Piece p = MOVED_PIECE(pos, mv);
    Piece captured_p = (mt == MOVE_TYPE_EN_PASSANT)
                     ? PIECE_FROM_COLOR_PIECE_TYPE(nstm_c, PIECE_TYPE_PAWN)
                     : PIECE_ON_SQUARE(pos, to_sq);

    if (mt == MOVE_TYPE_CASTLING) {
        Piece rook_p = PIECE_FROM_COLOR_PIECE_TYPE(stm_c, PIECE_TYPE_ROOK);
        Square rook_from_sq, rook_to_sq;
        castling_rook_squares(to_sq, &rook_from_sq, &rook_to_sq);

        move_piece(pos, rook_from_sq, rook_to_sq);
        DIRTY_PIECES_ADD(pos->st->dirty, rook_p, rook_from_sq, rook_to_sq);
        k ^= PIECE_SQUARE_KEY(rook_p, rook_from_sq) ^ PIECE_SQUARE_KEY(rook_p, rook_to_sq);
    }

    if (captured_p != NO_PIECE) {
        Square captured_sq = to_sq;

        if (mt == MOVE_TYPE_EN_PASSANT)
            captured_sq = (stm_c == COLOR_WHITE) ? to_sq - 8 : to_sq + 8;

        remove_piece(pos, captured_sq);
        DIRTY_PIECES_ADD(pos->st->dirty, captured_p, captured_sq, NO_SQUARE);
        k ^= PIECE_SQUARE_KEY(captured_p, captured_sq);
        if (PIECE_TYPE_FROM_PIECE(captured_p) == PIECE_TYPE_PAWN)
            PAWN_KEY(pos) ^= PIECE_SQUARE_KEY(captured_p, captured_sq);
        HALFMOVE_CLOCK(pos) = 0;
    }

    if (EN_PASSANT_SQUARE(pos) != NO_SQUARE) {
        k ^= EN_PASSANT_KEY(FILE_FROM_SQUARE(EN_PASSANT_SQUARE(pos)));
        EN_PASSANT_SQUARE(pos) = NO_SQUARE;
    }

    CastlingRights lost_cr = CASTLING_RIGHTS_MASKS[from_sq] | CASTLING_RIGHTS_MASKS[to_sq];
    if (CAN_CASTLE(pos, lost_cr)) {
        k ^= CASTLING_RIGHTS_KEY(CASTLING_RIGHTS(pos));
        CASTLING_RIGHTS(pos) &= ~lost_cr;
        k ^= CASTLING_RIGHTS_KEY(CASTLING_RIGHTS(pos));
    }

    move_piece(pos, from_sq, to_sq);
    DIRTY_PIECES_ADD(pos->st->dirty, p, from_sq, to_sq);

    k ^= PIECE_SQUARE_KEY(p, from_sq) ^ PIECE_SQUARE_KEY(p, to_sq);

    if (PIECE_TYPE_FROM_PIECE(p) == PIECE_TYPE_PAWN) {
        PAWN_KEY(pos) ^= PIECE_SQUARE_KEY(p, from_sq) ^ PIECE_SQUARE_KEY(p, to_sq);

        if ((to_sq ^ from_sq) == 16) {
            Square ep_sq = (Square)((from_sq + to_sq) / 2);

            if (PAWN_ATTACKS_BITBOARD(stm_c, ep_sq) & COLOR_PIECE_TYPE_BITBOARD(pos, nstm_c, PIECE_TYPE_PAWN)) {
                EN_PASSANT_SQUARE(pos) = ep_sq;
                k ^= EN_PASSANT_KEY(FILE_FROM_SQUARE(ep_sq));
            }
        } else if (mt == MOVE_TYPE_PROMOTION) {
            Piece promoted_p = PIECE_FROM_COLOR_PIECE_TYPE(stm_c, PROMOTION_PIECE_TYPE_FROM_MOVE(mv));

            remove_piece(pos, to_sq);
            put_piece(pos, promoted_p, to_sq);

            pos->st->dirty.to_sqs[pos->st->dirty.count - 1] = NO_SQUARE;
            DIRTY_PIECES_ADD(pos->st->dirty, promoted_p, NO_SQUARE, to_sq);
            k ^= PIECE_SQUARE_KEY(p, to_sq) ^ PIECE_SQUARE_KEY(promoted_p, to_sq);
            PAWN_KEY(pos) ^= PIECE_SQUARE_KEY(p, to_sq);
        }

        HALFMOVE_CLOCK(pos) = 0;
    }

    CAPTURED_PIECE(pos) = captured_p;
    KEY(pos) = k;
    SIDE_TO_MOVE(pos) = nstm_c;

    CHECKERS_BITBOARD(pos) = attackers_to(pos, KING_SQUARE(pos, nstm_c), OCCUPANCY_BITBOARD(pos))
                           & COLOR_BITBOARD(pos, stm_c);

    set_check_info(pos);
    clear_illegal_en_passant(pos);

    pos->st->repetition = 0;
    u16 end = HALFMOVE_CLOCK(pos) < pos->st->plies_from_null ? HALFMOVE_CLOCK(pos) : pos->st->plies_from_null;
    if (end >= 4) {
        const PlyState *prev_st = pos->st->prev_st->prev_st;
        for (u16 i = 4; i <= end; i += 2) {
            prev_st = prev_st->prev_st->prev_st;
            if (prev_st->k == pos->st->k) {
                pos->st->repetition = prev_st->repetition ? -(i16)i : (i16)i;
                break;
            }
        }
    }
}

void undo_move(Position *pos, Move mv) {
    SIDE_TO_MOVE(pos) = OPPOSITE_COLOR(SIDE_TO_MOVE(pos));

    Color stm_c = SIDE_TO_MOVE(pos);
    Square from_sq = ORIGIN_SQUARE_FROM_MOVE(mv);
    Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);
    MoveType mt = MOVE_TYPE_FROM_MOVE(mv);

    if (mt == MOVE_TYPE_PROMOTION) {
        remove_piece(pos, to_sq);
        put_piece(pos, PIECE_FROM_COLOR_PIECE_TYPE(stm_c, PIECE_TYPE_PAWN), to_sq);
    }

    move_piece(pos, to_sq, from_sq);

    if (mt == MOVE_TYPE_CASTLING) {
        Square rook_from_sq, rook_to_sq;
        castling_rook_squares(to_sq, &rook_from_sq, &rook_to_sq);
        move_piece(pos, rook_to_sq, rook_from_sq);
    }

    Piece captured_p = CAPTURED_PIECE(pos);
    if (captured_p != NO_PIECE) {
        Square captured_sq = to_sq;

        if (mt == MOVE_TYPE_EN_PASSANT)
            captured_sq = (stm_c == COLOR_WHITE) ? to_sq - 8 : to_sq + 8;

        put_piece(pos, captured_p, captured_sq);
    }

    pos->st = pos->st->prev_st;
    GAME_PLY(pos)--;
}

void do_null_move(Position *pos, PlyState *new_st) {
    memcpy(new_st, pos->st, offsetof(PlyState, dirty));
    new_st->prev_st = pos->st;
    new_st->dirty.count = 0;
    new_st->acc.computed[COLOR_WHITE] = false;
    new_st->acc.computed[COLOR_BLACK] = false;
    pos->st = new_st;

    if (EN_PASSANT_SQUARE(pos) != NO_SQUARE) {
        KEY(pos) ^= EN_PASSANT_KEY(FILE_FROM_SQUARE(EN_PASSANT_SQUARE(pos)));
        EN_PASSANT_SQUARE(pos) = NO_SQUARE;
    }

    KEY(pos) ^= SIDE_TO_MOVE_KEY;
    HALFMOVE_CLOCK(pos)++;
    pos->st->plies_from_null = 0;  
    pos->st->repetition = 0;
    CAPTURED_PIECE(pos) = NO_PIECE;
    CHECKERS_BITBOARD(pos) = BITBOARD_NONE;

    SIDE_TO_MOVE(pos) = OPPOSITE_COLOR(SIDE_TO_MOVE(pos));
    set_check_squares(pos);
}

void undo_null_move(Position *pos) {
    pos->st = pos->st->prev_st;
    SIDE_TO_MOVE(pos) = OPPOSITE_COLOR(SIDE_TO_MOVE(pos));
}

void move_to_uci(Move mv, char *buffer) {
    Square from_sq = ORIGIN_SQUARE_FROM_MOVE(mv);
    Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);
    MoveType mt = MOVE_TYPE_FROM_MOVE(mv);

    buffer[0] = (char)('a' + FILE_FROM_SQUARE(from_sq));
    buffer[1] = (char)('1' + RANK_FROM_SQUARE(from_sq));
    buffer[2] = (char)('a' + FILE_FROM_SQUARE(to_sq));
    buffer[3] = (char)('1' + RANK_FROM_SQUARE(to_sq));
    buffer[4] = (mt == MOVE_TYPE_PROMOTION) ? "nbrq"[PROMOTION_PIECE_TYPE_FROM_MOVE(mv) - PIECE_TYPE_KNIGHT] : '\0';
    buffer[5] = '\0';
}

static void print_square(Square sq) {
    printf("%c%c", 'a' + FILE_FROM_SQUARE(sq), '1' + RANK_FROM_SQUARE(sq));
}

void print_position(const Position *pos) {
    const char *separator = " +---+---+---+---+---+---+---+---+\n";

    printf("\n%s", separator);
    for (i8 r = RANK_8; r >= RANK_1; r--) {
        printf(" |");
        for (i8 f = FILE_A; f <= FILE_H; f++) {
            Piece p = PIECE_ON_SQUARE(pos, SQUARE_FROM_FILE_RANK(f, r));
            printf(" %c |", p == NO_PIECE ? ' ' : PIECE_CHARS[p]);
        }
        printf(" %d\n%s", r + 1, separator);
    }
    printf("   a   b   c   d   e   f   g   h\n\n");

    printf("Side to move:    %s\n", SIDE_TO_MOVE(pos) == COLOR_WHITE ? "white" : "black");

    printf("Castling rights: ");
    if (CASTLING_RIGHTS(pos) == CASTLING_RIGHTS_NONE) {
        printf("-");
    } else {
        if (CAN_CASTLE(pos, CASTLING_RIGHTS_WHITE_OO))  printf("K");
        if (CAN_CASTLE(pos, CASTLING_RIGHTS_WHITE_OOO)) printf("Q");
        if (CAN_CASTLE(pos, CASTLING_RIGHTS_BLACK_OO))  printf("k");
        if (CAN_CASTLE(pos, CASTLING_RIGHTS_BLACK_OOO)) printf("q");
    }
    printf("\n");

    printf("En passant:      ");
    if (EN_PASSANT_SQUARE(pos) == NO_SQUARE) printf("-");
    else print_square(EN_PASSANT_SQUARE(pos));
    printf("\n");

    printf("Halfmove clock:  %u\n", HALFMOVE_CLOCK(pos));
    printf("Fullmove number: %u\n", 1 + (GAME_PLY(pos) - (SIDE_TO_MOVE(pos) == COLOR_BLACK)) / 2);
    printf("Key:             %016" PRIX64 "\n", KEY(pos));

    printf("Checkers:        ");
    Bitboard checkers_bb = CHECKERS_BITBOARD(pos);
    if (!checkers_bb) printf("-");
    for (; checkers_bb; POP_LSB(checkers_bb)) {
        print_square((Square)LSB(checkers_bb));
        printf(" ");
    }
    printf("\n");
}