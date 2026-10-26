#ifndef HONEY_BADGER_POSITION_H
#define HONEY_BADGER_POSITION_H

#include "types.h"
#include "nnue.h"

typedef struct PlyState {
    CastlingRights cr;
    Square ep_sq;
    u8 halfmv_clock;
    u16 plies_from_null;
    Key pawn_k;             
    
    // memcpy boundary

    Key k;
    Bitboard checkers_bb;
    Bitboard blockers_bbs[NO_COLOR];
    Bitboard pinners_bbs[NO_COLOR];
    Bitboard check_sqs_bbs[NO_PIECE_TYPE];
    Piece captured_p;
    i16 repetition;
    struct PlyState *prev_st;

    // null move copy boundary

    DirtyPieces dirty;
    Accumulator acc;
} PlyState;
 
typedef struct Position {
    Piece ps[NO_SQUARE];
    Bitboard bbs_by_pt[NO_PIECE_TYPE];
    Bitboard bbs_by_c[NO_COLOR];
    Bitboard occ_bb;
    u8 p_counts[NO_PIECE];
    PlyState *st;
    u16 game_ply;
    Color stm_c;
} Position;

#define KEY(pos) ((pos)->st->k)
#define PAWN_KEY(pos) ((pos)->st->pawn_k)
#define EN_PASSANT_SQUARE(pos) ((pos)->st->ep_sq)
#define HALFMOVE_CLOCK(pos) ((pos)->st->halfmv_clock)
#define CHECKERS_BITBOARD(pos) ((pos)->st->checkers_bb)
#define BLOCKERS_BITBOARD(pos, c) ((pos)->st->blockers_bbs[(c)])
#define PINNERS_BITBOARD(pos, c) ((pos)->st->pinners_bbs[(c)])
#define CHECK_SQUARES_BITBOARD(pos, pt) ((pos)->st->check_sqs_bbs[(pt)])
#define CAPTURED_PIECE(pos) ((pos)->st->captured_p)

#define PIECE_ON_SQUARE(pos, sq) ((pos)->ps[(sq)])
#define IS_SQUARE_EMPTY(pos, sq) (PIECE_ON_SQUARE(pos, sq) == NO_PIECE)
#define OCCUPANCY_BITBOARD(pos) ((pos)->occ_bb)
#define COLOR_BITBOARD(pos, c) ((pos)->bbs_by_c[(c)])
#define PIECE_TYPE_BITBOARD(pos, pt) ((pos)->bbs_by_pt[(pt)])
#define COLOR_PIECE_TYPE_BITBOARD(pos, c, pt) (COLOR_BITBOARD(pos, c) & PIECE_TYPE_BITBOARD(pos, pt))
#define PIECE_COUNT(pos, p) ((pos)->p_counts[(p)])
#define KING_SQUARE(pos, c) ((Square)LSB(COLOR_PIECE_TYPE_BITBOARD(pos, c, PIECE_TYPE_KING)))
#define SIDE_TO_MOVE(pos) ((pos)->stm_c)
#define GAME_PLY(pos) ((pos)->game_ply)

#define CASTLING_RIGHTS(pos) ((pos)->st->cr)
#define CAN_CASTLE(pos, cr) ((CASTLING_RIGHTS(pos) & (cr)) != 0)

#define CASTLING_PATH_BITBOARD(cr) \
    ((cr) == CASTLING_RIGHTS_WHITE_OO  ? UINT64_C(0x0000000000000060) :  \
     (cr) == CASTLING_RIGHTS_WHITE_OOO ? UINT64_C(0x000000000000000E) :  \
     (cr) == CASTLING_RIGHTS_BLACK_OO  ? UINT64_C(0x6000000000000000) :  \
                                         UINT64_C(0x0E00000000000000))  
#define CASTLING_IMPEDED(pos, cr) ((OCCUPANCY_BITBOARD(pos) & CASTLING_PATH_BITBOARD(cr)) != 0)

#define MOVED_PIECE(pos, mv) PIECE_ON_SQUARE(pos, ORIGIN_SQUARE_FROM_MOVE(mv))
#define IS_CAPTURE(pos, mv) \
    (!IS_SQUARE_EMPTY(pos, TARGET_SQUARE_FROM_MOVE(mv)) || MOVE_TYPE_FROM_MOVE(mv) == MOVE_TYPE_EN_PASSANT)


#define START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

void set_position_from_fen(Position *pos, const char *fen, PlyState *st);
void fen_from_position(const Position *pos, char *buff);

Bitboard attackers_to(const Position *pos, Square sq, Bitboard occ_bb);

void do_move(Position *pos, Move mv, PlyState *new_st);
void undo_move(Position *pos, Move mv);

void do_null_move(Position *pos, PlyState *new_st);
void undo_null_move(Position *pos);

extern const Score PIECE_VALUES_[NO_PIECE + 1];
#define PIECE_VALUE(p) (PIECE_VALUES_[(p)])

bool see_ge(const Position *pos, Move mv, Score threshold);

bool gives_check(const Position *pos, Move mv);

bool is_draw(const Position *pos, u16 ply);

#define HAS_NON_PAWN_MATERIAL(pos, c) \
    ((COLOR_BITBOARD(pos, c) & ~PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_PAWN) & ~PIECE_TYPE_BITBOARD(pos, PIECE_TYPE_KING)) != 0)

bool is_pseudo_legal(const Position *pos, Move mv);

bool is_legal(const Position *pos, Move mv);

void move_to_uci(Move mv, char *buffer);

void print_position(const Position* pos);
#endif