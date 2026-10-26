#include <string.h>

#include "nnue.h"
#include "position.h"

#if defined(NNUE_NO_SIMD)
    #define NNUE_SIMD_SCALAR
#elif defined(__AVX2__)
    #include <immintrin.h>
    #define NNUE_SIMD_AVX2
#elif defined(__SSE2__) || defined(_M_X64)
    #include <emmintrin.h>
    #define NNUE_SIMD_SSE2
#elif defined(__ARM_NEON) && defined(__aarch64__)
    #include <arm_neon.h>
    #define NNUE_SIMD_NEON
#else
    #define NNUE_SIMD_SCALAR
#endif


#if defined(NNUE_SIMD_AVX2)

    typedef __m256i VecI32;
    #define VEC_I32_LANES (8)

    static inline VecI32 vec_load_i32(const i32 *p) { return _mm256_load_si256((const __m256i *)p); }
    static inline void vec_store_i32(i32 *p, VecI32 v) { _mm256_store_si256((__m256i *)p, v); }
    static inline VecI32 vec_add_i32(VecI32 a, VecI32 b) { return _mm256_add_epi32(a, b); }
    static inline VecI32 vec_sub_i32(VecI32 a, VecI32 b) { return _mm256_sub_epi32(a, b); }
    static inline VecI32 vec_load_i16_widened(const i16 *p) {
        return _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i *)p));
    }

#elif defined(NNUE_SIMD_SSE2)

    typedef __m128i VecI32;
    #define VEC_I32_LANES (4)

    static inline VecI32 vec_load_i32(const i32 *p) { return _mm_load_si128((const __m128i *)p); }
    static inline void vec_store_i32(i32 *p, VecI32 v) { _mm_store_si128((__m128i *)p, v); }
    static inline VecI32 vec_add_i32(VecI32 a, VecI32 b) { return _mm_add_epi32(a, b); }
    static inline VecI32 vec_sub_i32(VecI32 a, VecI32 b) { return _mm_sub_epi32(a, b); }
    static inline VecI32 vec_load_i16_widened(const i16 *p) {
        __m128i x = _mm_loadl_epi64((const __m128i *)p);
        return _mm_srai_epi32(_mm_unpacklo_epi16(x, x), 16);
    }

#elif defined(NNUE_SIMD_NEON)

    typedef int32x4_t VecI32;
    #define VEC_I32_LANES (4)

    static inline VecI32 vec_load_i32(const i32 *p) { return vld1q_s32(p); }
    static inline void vec_store_i32(i32 *p, VecI32 v) { vst1q_s32(p, v); }
    static inline VecI32 vec_add_i32(VecI32 a, VecI32 b) { return vaddq_s32(a, b); }
    static inline VecI32 vec_sub_i32(VecI32 a, VecI32 b) { return vsubq_s32(a, b); }
    static inline VecI32 vec_load_i16_widened(const i16 *p) { return vmovl_s16(vld1_s16(p)); }

#else

    typedef i32 VecI32;
    #define VEC_I32_LANES (1)

    static inline VecI32 vec_load_i32(const i32 *p) { return *p; }
    static inline void vec_store_i32(i32 *p, VecI32 v) { *p = v; }
    static inline VecI32 vec_add_i32(VecI32 a, VecI32 b) { return a + b; }
    static inline VecI32 vec_sub_i32(VecI32 a, VecI32 b) { return a - b; }
    static inline VecI32 vec_load_i16_widened(const i16 *p) { return *p; }

#endif


#if defined(NNUE_SIMD_AVX2)

static inline i32 screlu_dot(const i32 *vals, const i16 *ws) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i qa = _mm256_set1_epi16(NNUE_QA);
    __m256i sum = zero;

    for (u16 i = 0; i < NNUE_HIDDEN_SIZE; i += 16) {
        __m256i packed = _mm256_packs_epi32(vec_load_i32(vals + i), vec_load_i32(vals + i + 8));
        __m256i v = _mm256_permute4x64_epi64(packed, 0xD8);
        v = _mm256_min_epi16(_mm256_max_epi16(v, zero), qa);

        __m256i w = _mm256_loadu_si256((const __m256i *)(ws + i));
        sum = _mm256_add_epi32(sum, _mm256_madd_epi16(v, _mm256_mullo_epi16(v, w)));
    }

    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    return _mm_cvtsi128_si32(s);
}

#elif defined(NNUE_SIMD_SSE2)

static inline i32 screlu_dot(const i32 *vals, const i16 *ws) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i qa = _mm_set1_epi16(NNUE_QA);
    __m128i sum = zero;

    for (u16 i = 0; i < NNUE_HIDDEN_SIZE; i += 8) {
        __m128i v = _mm_packs_epi32(vec_load_i32(vals + i), vec_load_i32(vals + i + 4));
        v = _mm_min_epi16(_mm_max_epi16(v, zero), qa);

        __m128i w = _mm_loadu_si128((const __m128i *)(ws + i));
        sum = _mm_add_epi32(sum, _mm_madd_epi16(v, _mm_mullo_epi16(v, w)));
    }

    sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0x4E));
    sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0xB1));
    return _mm_cvtsi128_si32(sum);
}

#elif defined(NNUE_SIMD_NEON)

static inline i32 screlu_dot(const i32 *vals, const i16 *ws) {
    const int16x8_t zero = vdupq_n_s16(0);
    const int16x8_t qa = vdupq_n_s16(NNUE_QA);
    int32x4_t sum = vdupq_n_s32(0);

    for (u16 i = 0; i < NNUE_HIDDEN_SIZE; i += 8) {
        int16x8_t v = vcombine_s16(vqmovn_s32(vld1q_s32(vals + i)), vqmovn_s32(vld1q_s32(vals + i + 4)));
        v = vminq_s16(vmaxq_s16(v, zero), qa);

        int16x8_t vw = vmulq_s16(v, vld1q_s16(ws + i));
        sum = vmlal_s16(sum, vget_low_s16(v), vget_low_s16(vw));
        sum = vmlal_high_s16(sum, v, vw);
    }

    return vaddvq_s32(sum);
}

#else

static inline i32 screlu_dot(const i32 *vals, const i16 *ws) {
    i32 sum = 0;

    for (u16 i = 0; i < NNUE_HIDDEN_SIZE; i++) {
        i32 v = vals[i] < 0 ? 0 : (vals[i] > NNUE_QA ? NNUE_QA : vals[i]);
        sum += v * (v * ws[i]);
    }

    return sum;
}

#endif

const char *nnue_simd_name(void) {
#if defined(NNUE_SIMD_AVX2)
    return "AVX2";
#elif defined(NNUE_SIMD_SSE2)
    return "SSE2";
#elif defined(NNUE_SIMD_NEON)
    return "NEON";
#else
    return "scalar";
#endif
}

static inline void add_feature(i32 *vals, u16 f) {
    const i16 *ws = NNUE_ACCUMULATOR_WEIGHTS(f);
    for (u16 i = 0; i < NNUE_HIDDEN_SIZE; i += VEC_I32_LANES)
        vec_store_i32(vals + i, vec_add_i32(vec_load_i32(vals + i), vec_load_i16_widened(ws + i)));
}

static inline void sub_feature(i32 *vals, u16 f) {
    const i16 *ws = NNUE_ACCUMULATOR_WEIGHTS(f);
    for (u16 i = 0; i < NNUE_HIDDEN_SIZE; i += VEC_I32_LANES)
        vec_store_i32(vals + i, vec_sub_i32(vec_load_i32(vals + i), vec_load_i16_widened(ws + i)));
}

static inline void move_feature(i32 *vals, u16 from_f, u16 to_f) {
    const i16 *from_ws = NNUE_ACCUMULATOR_WEIGHTS(from_f);
    const i16 *to_ws = NNUE_ACCUMULATOR_WEIGHTS(to_f);
    for (u16 i = 0; i < NNUE_HIDDEN_SIZE; i += VEC_I32_LANES) {
        VecI32 v = vec_add_i32(vec_load_i32(vals + i), vec_load_i16_widened(to_ws + i));
        vec_store_i32(vals + i, vec_sub_i32(v, vec_load_i16_widened(from_ws + i)));
    }
}

static void refresh(const Position *pos, Accumulator *acc, Color c) {
    i32 *vals = acc->vals[c];

    for (u16 i = 0; i < NNUE_HIDDEN_SIZE; i += VEC_I32_LANES)
        vec_store_i32(vals + i, vec_load_i16_widened(&NNUE_ACCUMULATOR_BIAS(i)));

    for (Bitboard occ_bb = OCCUPANCY_BITBOARD(pos); occ_bb; POP_LSB(occ_bb)) {
        Square sq = (Square)LSB(occ_bb);
        add_feature(vals, NNUE_FEATURE(c, PIECE_ON_SQUARE(pos, sq), sq));
    }

    acc->computed[c] = true;
}

static void update_from_parent(PlyState *st, Color c) {
    i32 *vals = st->acc.vals[c];
    const DirtyPieces *dp = &st->dirty;

    memcpy(vals, st->prev_st->acc.vals[c], sizeof(st->acc.vals[c]));

    for (u8 j = 0; j < dp->count; j++) {
        Piece p = dp->ps[j];
        Square from_sq = dp->from_sqs[j];
        Square to_sq = dp->to_sqs[j];

        if (from_sq == NO_SQUARE)
            add_feature(vals, NNUE_FEATURE(c, p, to_sq));
        else if (to_sq == NO_SQUARE)
            sub_feature(vals, NNUE_FEATURE(c, p, from_sq));
        else
            move_feature(vals, NNUE_FEATURE(c, p, from_sq), NNUE_FEATURE(c, p, to_sq));
    }

    st->acc.computed[c] = true;
}

static void update_accumulator(const Position *pos, Color c) {
    PlyState *st = pos->st;
    PlyState *chain[NNUE_MAX_UPDATE_DISTANCE];
    u8 n = 0;

    while (!st->acc.computed[c]) {
        if (!st->prev_st || n == NNUE_MAX_UPDATE_DISTANCE) {
            refresh(pos, &pos->st->acc, c);
            return;
        }
        chain[n++] = st;
        st = st->prev_st;
    }

    while (n > 0)
        update_from_parent(chain[--n], c);
}

static Score output(const Accumulator *acc, Color stm_c) {
    i64 sum = (i64)screlu_dot(acc->vals[stm_c], &NNUE_OUTPUT_WEIGHT(0))
            + screlu_dot(acc->vals[OPPOSITE_COLOR(stm_c)], &NNUE_OUTPUT_WEIGHT(NNUE_HIDDEN_SIZE));

    sum = sum / NNUE_QA + NNUE_OUTPUT_BIAS;
    return (Score)(sum * NNUE_SCALE / (NNUE_QA * NNUE_QB));
}


Score nnue_evaluate(const Position *pos) {
    update_accumulator(pos, COLOR_WHITE);
    update_accumulator(pos, COLOR_BLACK);

    Score sc = output(&pos->st->acc, SIDE_TO_MOVE(pos));

    return sc;
}

Score nnue_evaluate_from_scratch(const Position *pos) {
    Accumulator acc;
    refresh(pos, &acc, COLOR_WHITE);
    refresh(pos, &acc, COLOR_BLACK);
    return output(&acc, SIDE_TO_MOVE(pos));
}