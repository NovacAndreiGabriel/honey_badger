#ifndef HONEY_BADGER_TYPES_H
#define HONEY_BADGER_TYPES_H

#include <stdint.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef enum File : u8 {
    FILE_A,
    FILE_B,
    FILE_C,
    FILE_D,
    FILE_E,
    FILE_F,
    FILE_G,
    FILE_H,
    NO_FILE
} File;

typedef enum Rank : u8 {
    RANK_1,
    RANK_2,
    RANK_3,
    RANK_4,
    RANK_5,
    RANK_6,
    RANK_7,
    RANK_8,
    NO_RANK
} Rank;

typedef enum Square : u8 {
    SQUARE_A1, SQUARE_B1, SQUARE_C1, SQUARE_D1, SQUARE_E1, SQUARE_F1, SQUARE_G1, SQUARE_H1,
    SQUARE_A2, SQUARE_B2, SQUARE_C2, SQUARE_D2, SQUARE_E2, SQUARE_F2, SQUARE_G2, SQUARE_H2,
    SQUARE_A3, SQUARE_B3, SQUARE_C3, SQUARE_D3, SQUARE_E3, SQUARE_F3, SQUARE_G3, SQUARE_H3,
    SQUARE_A4, SQUARE_B4, SQUARE_C4, SQUARE_D4, SQUARE_E4, SQUARE_F4, SQUARE_G4, SQUARE_H4,
    SQUARE_A5, SQUARE_B5, SQUARE_C5, SQUARE_D5, SQUARE_E5, SQUARE_F5, SQUARE_G5, SQUARE_H5,
    SQUARE_A6, SQUARE_B6, SQUARE_C6, SQUARE_D6, SQUARE_E6, SQUARE_F6, SQUARE_G6, SQUARE_H6,
    SQUARE_A7, SQUARE_B7, SQUARE_C7, SQUARE_D7, SQUARE_E7, SQUARE_F7, SQUARE_G7, SQUARE_H7,
    SQUARE_A8, SQUARE_B8, SQUARE_C8, SQUARE_D8, SQUARE_E8, SQUARE_F8, SQUARE_G8, SQUARE_H8,
    NO_SQUARE
} Square;

#define FILE_FROM_SQUARE(sq) ((sq) & 7)
#define RANK_FROM_SQUARE(sq) ((sq) >> 3)
#define SQUARE_FROM_FILE_RANK(f, r) (((r) << 3) | (f))

typedef u64 Bitboard;

#define BITBOARD_NONE ((Bitboard)0)

#define BITBOARD_FROM_FILE(f) (UINT64_C(0x0101010101010101) << (f))
#define BITBOARD_FROM_RANK(r) (UINT64_C(0x00000000000000FF) << ((r) << 3))
#define BITBOARD_FROM_SQUARE(sq) (UINT64_C(1) << (sq))

#define SET_BIT(bb, sq) ((bb) |= BITBOARD_FROM_SQUARE(sq))
#define UNSET_BIT(bb, sq) ((bb) &= ~BITBOARD_FROM_SQUARE(sq))
#define TEST_BIT(bb, sq) (((bb) & BITBOARD_FROM_SQUARE(sq)) != 0)

#if defined(__GNUC__) || defined(__clang__)

    #define LSB(bb) ((u8)__builtin_ctzll(bb))
    #define COUNT_BITS(bb) ((u8)__builtin_popcountll(bb))

#elif defined(_MSC_VER)

    #include <intrin.h>

    static inline u8 lsb_msvc_(Bitboard bb) {
        unsigned long index;
        _BitScanForward64(&index, bb);
        return (u8)index;
    }

    #define LSB(bb) lsb_msvc_(bb)
    #define COUNT_BITS(bb) ((u8)__popcnt64(bb))

#else

    static const u8 LSB_INDEX_[64] = {
         0,  1, 48,  2, 57, 49, 28,  3, 61, 58, 50, 42, 38, 29, 17,  4,
        62, 55, 59, 36, 53, 51, 43, 22, 45, 39, 33, 30, 24, 18, 12,  5,
        63, 47, 56, 27, 60, 41, 37, 16, 54, 35, 52, 21, 44, 32, 23, 11,
        46, 26, 40, 15, 34, 20, 31, 10, 25, 14, 19,  9, 13,  8,  7,  6
    };

    static inline u8 lsb_portable_(Bitboard bb) {
        return LSB_INDEX_[((bb & (0 - bb)) * UINT64_C(0x03F79D71B4CB0A89)) >> 58];
    }

    static inline u8 count_bits_portable_(Bitboard bb) {
        bb = bb - ((bb >> 1) & UINT64_C(0x5555555555555555));
        bb = (bb & UINT64_C(0x3333333333333333)) + ((bb >> 2) & UINT64_C(0x3333333333333333));
        bb = (bb + (bb >> 4)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
        return (u8)((bb * UINT64_C(0x0101010101010101)) >> 56);
    }

    #define LSB(bb) lsb_portable_(bb)
    #define COUNT_BITS(bb) count_bits_portable_(bb)

#endif

#define POP_LSB(bb) ((bb) &= ((bb) - 1))

typedef enum Color : u8 {
    COLOR_WHITE,
    COLOR_BLACK,
    NO_COLOR
} Color;

#define OPPOSITE_COLOR(c) ((c) ^ 1)

#define RELATIVE_SQUARE(c, sq) ((Square)((sq) ^ ((c) * 56)))

typedef enum PieceType : u8 {
    PIECE_TYPE_PAWN,
    PIECE_TYPE_KNIGHT,
    PIECE_TYPE_BISHOP,
    PIECE_TYPE_ROOK,
    PIECE_TYPE_QUEEN,
    PIECE_TYPE_KING,
    NO_PIECE_TYPE
} PieceType;

typedef enum Piece : u8 {
    PIECE_WHITE_PAWN,
    PIECE_WHITE_KNIGHT,
    PIECE_WHITE_BISHOP,
    PIECE_WHITE_ROOK,
    PIECE_WHITE_QUEEN,
    PIECE_WHITE_KING,
    PIECE_BLACK_PAWN,
    PIECE_BLACK_KNIGHT,
    PIECE_BLACK_BISHOP,
    PIECE_BLACK_ROOK,
    PIECE_BLACK_QUEEN,
    PIECE_BLACK_KING,
    NO_PIECE
} Piece;

#define PIECE_FROM_COLOR_PIECE_TYPE(c, pt) ((c) * 6 + (pt))
#define COLOR_FROM_PIECE(p) ((p) / 6)
#define PIECE_TYPE_FROM_PIECE(p) ((p) % 6)

typedef enum MoveType : u16 {
    MOVE_TYPE_NORMAL = 0 << 14,
    MOVE_TYPE_PROMOTION = 1 << 14,
    MOVE_TYPE_EN_PASSANT = 2 << 14,
    MOVE_TYPE_CASTLING = 3 << 14
} MoveType;

typedef u16 Move;

#define MOVE_NONE ((Move)0)
#define MOVE_NULL ((Move)65)

#define MOVE_MAKE_(from_sq, to_sq, mv_type, promotion_bits) \
    ((Move)((mv_type) | ((promotion_bits) << 12) | ((from_sq) << 6) | (to_sq)))

#define NORMAL_MOVE(from_sq, to_sq) MOVE_MAKE_(from_sq, to_sq, MOVE_TYPE_NORMAL, 0)
#define PROMOTION_MOVE(from_sq, to_sq, pt) MOVE_MAKE_(from_sq, to_sq, MOVE_TYPE_PROMOTION, (pt) - PIECE_TYPE_KNIGHT)
#define EN_PASSANT_MOVE(from_sq, to_sq) MOVE_MAKE_(from_sq, to_sq, MOVE_TYPE_EN_PASSANT, 0)
#define CASTLING_MOVE(king_from_sq, king_to_sq) MOVE_MAKE_(king_from_sq, king_to_sq, MOVE_TYPE_CASTLING, 0)

#define ORIGIN_SQUARE_FROM_MOVE(mv) ((Square)(((mv) >> 6) & 0x3F))
#define TARGET_SQUARE_FROM_MOVE(mv) ((Square)((mv) & 0x3F))
#define MOVE_TYPE_FROM_MOVE(mv) ((MoveType)((mv) & 0xC000))
#define PROMOTION_PIECE_TYPE_FROM_MOVE(mv) ((PieceType)((((mv) >> 12) & 3) + PIECE_TYPE_KNIGHT))

#define MOVE_IS_OK(mv) (ORIGIN_SQUARE_FROM_MOVE(mv) != TARGET_SQUARE_FROM_MOVE(mv))

typedef enum CastlingRights : u8 {
    CASTLING_RIGHTS_NONE = 0,
    CASTLING_RIGHTS_WHITE_OO = 1,
    CASTLING_RIGHTS_WHITE_OOO = 2,
    CASTLING_RIGHTS_BLACK_OO = 4,
    CASTLING_RIGHTS_BLACK_OOO = 8,
    CASTLING_RIGHTS_WHITE = CASTLING_RIGHTS_WHITE_OO | CASTLING_RIGHTS_WHITE_OOO,
    CASTLING_RIGHTS_BLACK = CASTLING_RIGHTS_BLACK_OO | CASTLING_RIGHTS_BLACK_OOO,
    CASTLING_RIGHTS_OO = CASTLING_RIGHTS_WHITE_OO | CASTLING_RIGHTS_BLACK_OO,
    CASTLING_RIGHTS_OOO = CASTLING_RIGHTS_WHITE_OOO | CASTLING_RIGHTS_BLACK_OOO,
    CASTLING_RIGHTS_ALL = CASTLING_RIGHTS_WHITE_OO | CASTLING_RIGHTS_WHITE_OOO | CASTLING_RIGHTS_BLACK_OO | CASTLING_RIGHTS_BLACK_OOO
} CastlingRights;

typedef u64 Key;
typedef u16 ShortKey;

typedef i32 Score;
typedef i16 ShortScore;

#define MAX_PLY (128)

#define SCORE_ZERO (0)
#define SCORE_DRAW (0)
#define SCORE_MATE (32000)
#define SCORE_INFINITE (32001)
#define SCORE_NONE (32002)      

#define SCORE_MATE_IN_MAX_PLY (SCORE_MATE - MAX_PLY)
#define SCORE_MATED_IN_MAX_PLY (-SCORE_MATE_IN_MAX_PLY)

#define MATE_IN(ply) (SCORE_MATE - (ply))       
#define MATED_IN(ply) (-SCORE_MATE + (ply))     

#define SCORE_PAWN (208)
#define SCORE_KNIGHT (781)
#define SCORE_BISHOP (825)
#define SCORE_ROOK (1276)
#define SCORE_QUEEN (2538)

typedef i16 Depth;

#define DEPTH_QS (0)                
#define DEPTH_UNSEARCHED (-2)       
#define DEPTH_ENTRY_OFFSET (-3)     
#endif