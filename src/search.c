#include <inttypes.h>
#include <stdint.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "search.h"
#include "search_cache.h"
#include "history.h"
#include "move_picker.h"
#include "move_generation.h"
#include "nnue.h"
#include "thread.h"

#define PAWN_CORRECTION_SIZE (16384)    
#define CORRECTION_HISTORY_LIMIT (1024)
#define DEFAULT_MOVE_OVERHEAD_MS (10)  

#define MAX_TIME_OPTIMUM_FACTOR (3.0)
#define MAX_TIME_CLOCK_SHARE (0.12)
#define STACK_OFFSET (7)                

typedef enum NodeType : u8 {
    NODE_TYPE_NON_PV,
    NODE_TYPE_PV,
    NODE_TYPE_ROOT
} NodeType;

typedef struct RootMove {
    Move pv[MAX_PLY + 1];
    u16 pv_len;
    Score sc;                  
    Score previous_sc;         
    Score average_sc;
    i64 mean_squared_sc;
    Score uci_sc;               
    bool sc_lower_bound;
    bool sc_upper_bound;
    u16 sel_depth;
    u64 effort;                
} RootMove;

typedef struct SearchStackEntry {
    Move *pv;
    PieceToHistory *cont_hist;  
    u16 ply;
    Move current_mv;
    Move excluded_mv;          
    Score static_eval;
    i32 stat_score;
    i32 move_count;
    i32 cutoff_cnt;
    i32 reduction;
    bool in_check;
    bool cache_pv;           
    bool cache_hit;
    bool is_cache_mv;
} SearchStackEntry;

typedef struct SearchWorker {
    SearchPool *pool;
    u16 idx;

    Thread thread;
    Mutex mutex;
    CondVar cond;
    bool searching;
    bool exit;

    Position root_pos;
    PlyState sts[MAX_PLY + 2];      
    Histories *h;
    i16 pawn_correction[PAWN_CORRECTION_SIZE][NO_COLOR];
    RootMove root_moves[MAX_MOVES];
    u16 root_moves_count;

    _Atomic u64 nodes;
    Depth root_depth;
    Depth completed_depth;
    u16 sel_depth;
    u16 nmp_min_ply;               
    _Atomic u64 best_move_changes;  
    Score root_delta;
    i64 votes;                      
    u16 pv_idx;                     
} SearchWorker;

struct SearchPool {
    SearchWorker **workers;
    u16 workers_count;
    SearchCache *cache;
    bool output;

   
    i64 move_overhead_ms;
    u16 multi_pv;
    bool ponder_enabled;            

    atomic_bool stop;
    atomic_bool aborted;            
    atomic_bool increase_depth;
    atomic_bool ponder;             
    atomic_bool stop_on_ponderhit;  

    SearchLimits limits;
    i64 start_ms;
    i64 optimum_ms;
    i64 maximum_ms;
    double original_time_adjust;
    i32 calls_cnt;
    Score best_previous_sc;
    Score best_previous_average_sc;
    Score iter_scs[4];
    double previous_time_reduction;
    SearchResult result;
};


static i32 reductions[MAX_MOVES];

static void init_reductions(void) {
    for (u16 i = 1; i < MAX_MOVES; i++)
        reductions[i] = (i32)(19.43 * log((double)i));
}

static inline i32 min_i32(i32 a, i32 b) { return a < b ? a : b; }
static inline i32 max_i32(i32 a, i32 b) { return a > b ? a : b; }
static inline i32 clamp_i32(i32 v, i32 lo, i32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline i32 abs_i32(i32 v) { return v < 0 ? -v : v; }

static inline bool is_win(Score sc) { return sc >= SCORE_MATE_IN_MAX_PLY; }
static inline bool is_loss(Score sc) { return sc <= SCORE_MATED_IN_MAX_PLY; }
static inline bool is_decisive(Score sc) { return is_win(sc) || is_loss(sc); }
static inline bool is_valid(Score sc) { return sc != SCORE_NONE; }

static i64 now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline u64 worker_nodes(const SearchWorker *w) {
    return atomic_load_explicit(&w->nodes, memory_order_relaxed);
}

static inline void add_node(SearchWorker *w) {
    atomic_store_explicit(&w->nodes, worker_nodes(w) + 1, memory_order_relaxed);
}

static u64 pool_nodes(const SearchPool *pool) {
    u64 nodes = 0;
    for (u16 i = 0; i < pool->workers_count; i++)
        nodes += worker_nodes(pool->workers[i]);
    return nodes;
}

static inline bool stopped(const SearchPool *pool) {
    return atomic_load_explicit(&pool->stop, memory_order_relaxed);
}

static inline bool is_capture_stage(const Position *pos, Move mv) {
    return IS_CAPTURE(pos, mv)
        || (MOVE_TYPE_FROM_MOVE(mv) == MOVE_TYPE_PROMOTION && PROMOTION_PIECE_TYPE_FROM_MOVE(mv) == PIECE_TYPE_QUEEN);
}

static inline Score score_draw(const SearchWorker *w) {
    return SCORE_DRAW - 1 + (Score)(worker_nodes(w) & 0x2);
}

static Score evaluate(const Position *pos) {
    Score sc = nnue_evaluate(pos);
    sc -= sc * HALFMOVE_CLOCK(pos) / 212;
    return clamp_i32(sc, SCORE_MATED_IN_MAX_PLY + 1, SCORE_MATE_IN_MAX_PLY - 1);
}

static inline i32 stat_bonus(Depth d) { return min_i32(158 * d - 98, 1622); }
static inline i32 stat_malus(Depth d) { return min_i32(802 * d - 243, 2850); }

static inline i32 correction_value(const SearchWorker *w, const Position *pos) {
    u16 idx = (u16)(PAWN_KEY(pos) & (PAWN_CORRECTION_SIZE - 1));
    return 6384 * w->pawn_correction[idx][SIDE_TO_MOVE(pos)];
}

static inline Score corrected_static_eval(Score sc, i32 cv) {
    return clamp_i32(sc + cv / 131072, SCORE_MATED_IN_MAX_PLY + 1, SCORE_MATE_IN_MAX_PLY - 1);
}

static void update_correction_history(SearchWorker *w, const Position *pos, i32 bonus) {
    u16 idx = (u16)(PAWN_KEY(pos) & (PAWN_CORRECTION_SIZE - 1));
    history_update(&w->pawn_correction[idx][SIDE_TO_MOVE(pos)], bonus, CORRECTION_HISTORY_LIMIT);
}

static void update_continuation_histories(SearchStackEntry *ss, Piece p, Square to_sq, i32 bonus) {
    static const i32 PLIES[] = { 1, 2, 3, 4, 5, 6 };
    static const i32 WEIGHTS[] = { 1029, 656, 326, 536, 120, 537 };

    for (u8 i = 0; i < 6; i++) {
        if (ss->in_check && PLIES[i] > 2)
            break;
        if (MOVE_IS_OK((ss - PLIES[i])->current_mv))
            history_update(&(ss - PLIES[i])->cont_hist->vals[p][to_sq], bonus * WEIGHTS[i] / 1024, PIECE_TO_HISTORY_LIMIT);
    }
}

static void update_quiet_histories(SearchWorker *w, const Position *pos, SearchStackEntry *ss, Move mv, i32 bonus) {
    Color stm_c = SIDE_TO_MOVE(pos);
    Piece p = MOVED_PIECE(pos, mv);
    Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);

    history_update(&w->h->butterfly[stm_c][FROM_TO_INDEX(mv)], bonus, BUTTERFLY_HISTORY_LIMIT);
    if (ss->ply < LOW_PLY_HISTORY_SIZE)
        history_update(&w->h->low_ply[ss->ply][FROM_TO_INDEX(mv)], bonus * 844 / 1024, LOW_PLY_HISTORY_LIMIT);
    update_continuation_histories(ss, p, to_sq, bonus * 964 / 1024);
    history_update(&w->h->pawn[PAWN_STRUCTURE_INDEX(pos)][p][to_sq], bonus * 615 / 1024, PAWN_HISTORY_LIMIT);
}

static void update_capture_history(SearchWorker *w, const Position *pos, Move mv, i32 bonus) {
    Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);
    i16 *entry = &w->h->capture[MOVED_PIECE(pos, mv)][to_sq][CAPTURED_PIECE_TYPE_INDEX(PIECE_ON_SQUARE(pos, to_sq))];
    history_update(entry, bonus, CAPTURE_HISTORY_LIMIT);
}

static void update_all_stats(SearchWorker *w, const Position *pos, SearchStackEntry *ss, Move best_mv,
                             Square prev_sq, const Move *quiets, u8 quiets_count, const Move *captures,
                             u8 captures_count, Depth d, bool is_cache_mv, i32 move_count) {
    i32 bonus = stat_bonus(d) + 300 * is_cache_mv;
    i32 malus = stat_malus(d) - 34 * (move_count - 1);

    if (!is_capture_stage(pos, best_mv)) {
        update_quiet_histories(w, pos, ss, best_mv, bonus * 1216 / 1024);
        for (u8 i = 0; i < quiets_count; i++)
            update_quiet_histories(w, pos, ss, quiets[i], -malus * 1062 / 1024);
    }
    else
        update_capture_history(w, pos, best_mv, bonus * 1272 / 1024);

    if (prev_sq != NO_SQUARE && (ss - 1)->move_count == 1 + (ss - 1)->cache_hit && CAPTURED_PIECE(pos) == NO_PIECE)
        update_continuation_histories(ss - 1, PIECE_ON_SQUARE(pos, prev_sq), prev_sq, -malus * 966 / 1024);

    for (u8 i = 0; i < captures_count; i++)
        update_capture_history(w, pos, captures[i], -malus * 1205 / 1024);
}

static void init_time_management(SearchPool *pool, Color stm_c, u16 game_ply) {
    const SearchLimits *l = &pool->limits;
    pool->optimum_ms = pool->maximum_ms = 0;

    if (l->time_ms[stm_c] == 0)
        return;

    i64 time_ms = l->time_ms[stm_c];
    i64 inc_ms = l->inc_ms[stm_c];

    i64 centi_mtg = l->moves_to_go ? min_i32(l->moves_to_go * 100, 5000) : 5051;
    if (time_ms < 1000 && (double)centi_mtg / (inc_ms ? (double)inc_ms : 1.0) > 5.051)
        centi_mtg = (i64)((double)time_ms * 5.051);

    i64 overhead_ms = pool->move_overhead_ms;
    i64 time_left = time_ms + (inc_ms * (centi_mtg - 100) - overhead_ms * (200 + centi_mtg)) / 100;
    if (time_left < 1)
        time_left = 1;

    double opt_scale, max_scale;

    if (l->moves_to_go == 0) {
        if (pool->original_time_adjust < 0)
            pool->original_time_adjust = 0.3128 * log10((double)time_left) - 0.4354;

        double log_time_s = log10((double)time_ms / 1000.0);
        double opt_constant = fmin(0.0032116 + 0.000321123 * log_time_s, 0.00508017);
        double max_constant = fmax(3.3977 + 3.03950 * log_time_s, 2.94761);

        opt_scale = fmin(0.0121431 + pow(game_ply + 2.94693, 0.461073) * opt_constant,
                         0.213035 * (double)time_ms / (double)time_left) * pool->original_time_adjust;
        max_scale = fmin(6.67704, max_constant + game_ply / 11.9847);
    }
    else {
        opt_scale = fmin((0.88 + game_ply / 116.4) / (centi_mtg / 100.0), 0.88 * (double)time_ms / (double)time_left);
        max_scale = 1.3 + 0.11 * (centi_mtg / 100.0);
    }

    pool->optimum_ms = (i64)(opt_scale * (double)time_left);
    pool->maximum_ms = (i64)fmin(0.825179 * (double)time_ms - overhead_ms, max_scale * (double)pool->optimum_ms) - 10;

    double cap_ms = fmin(MAX_TIME_OPTIMUM_FACTOR * (double)pool->optimum_ms,
                         MAX_TIME_CLOCK_SHARE * (double)time_ms + (double)inc_ms);
    if ((double)pool->maximum_ms > cap_ms)
        pool->maximum_ms = (i64)cap_ms;

    if (pool->maximum_ms < 1)
        pool->maximum_ms = 1;
    if (pool->optimum_ms > pool->maximum_ms)
        pool->optimum_ms = pool->maximum_ms;

    if (pool->ponder_enabled)
        pool->optimum_ms += pool->optimum_ms / 4;
}

static inline bool uses_time_management(const SearchPool *pool, Color stm_c) {
    return pool->limits.time_ms[stm_c] != 0;
}

static void check_time(SearchWorker *w) {
    SearchPool *pool = w->pool;

    if (--pool->calls_cnt > 0)
        return;
    pool->calls_cnt = pool->limits.nodes ? min_i32(512, (i32)(pool->limits.nodes / 1024) + 1) : 512;

    i64 elapsed = now_ms() - pool->start_ms;
    Color stm_c = SIDE_TO_MOVE(&w->root_pos);

    if (atomic_load_explicit(&pool->ponder, memory_order_relaxed))
        return;

    if (w->completed_depth >= 1
     && ((uses_time_management(pool, stm_c) && elapsed > pool->maximum_ms)
      || (pool->limits.move_time_ms && elapsed >= pool->limits.move_time_ms)
      || (pool->limits.nodes && pool_nodes(pool) >= pool->limits.nodes))) {
        atomic_store(&pool->aborted, true);
        atomic_store(&pool->stop, true);
    }
}

static void update_pv(Move *pv, Move mv, const Move *child_pv) {
    *pv++ = mv;
    while (child_pv && *child_pv != MOVE_NONE)
        *pv++ = *child_pv++;
    *pv = MOVE_NONE;
}

static bool is_searchable_root_move(const SearchWorker *w, Move mv) {
    for (u16 i = w->pv_idx; i < w->root_moves_count; i++)
        if (w->root_moves[i].pv[0] == mv)
            return true;
    return false;
}

static RootMove *find_root_move(SearchWorker *w, Move mv) {
    for (u16 i = 0; i < w->root_moves_count; i++)
        if (w->root_moves[i].pv[0] == mv)
            return &w->root_moves[i];
    return NULL;
}

static void sort_root_moves(RootMove *begin, RootMove *end) {
    for (RootMove *p = begin + 1; p < end; p++) {
        RootMove tmp = *p;
        RootMove *q = p;
        while (q > begin && ((q - 1)->sc < tmp.sc || ((q - 1)->sc == tmp.sc && (q - 1)->previous_sc < tmp.previous_sc))) {
            *q = *(q - 1);
            q--;
        }
        *q = tmp;
    }
}

static void print_score(Score sc) {
    if (is_win(sc))
        printf("mate %d", (SCORE_MATE - sc + 1) / 2);
    else if (is_loss(sc))
        printf("mate %d", -(SCORE_MATE + sc) / 2);
    else
        printf("cp %d", (int)sc);
}

static void print_info(SearchWorker *w, Depth d) {
    SearchPool *pool = w->pool;
    i64 elapsed = now_ms() - pool->start_ms + 1;
    u64 nodes = pool_nodes(pool);
    u16 hashfull = search_cache_hashfull(pool->cache, 0);
    u16 multi_pv = (u16)min_i32(pool->multi_pv, w->root_moves_count);

    for (u16 i = 0; i < multi_pv; i++) {
        const RootMove *rm = &w->root_moves[i];

        bool updated = rm->sc != -SCORE_INFINITE;
        if (d == 1 && !updated && i > 0)
            continue;

        Depth shown_d = updated ? d : (Depth)max_i32(1, d - 1);
        Score sc = updated ? rm->uci_sc : rm->previous_sc;
        if (sc == -SCORE_INFINITE)
            sc = SCORE_ZERO;

        printf("info depth %d seldepth %u multipv %u score ", (int)shown_d, (unsigned)rm->sel_depth, (unsigned)(i + 1));
        print_score(sc);
        if (i == w->pv_idx && updated && rm->sc_lower_bound)
            printf(" lowerbound");
        else if (i == w->pv_idx && updated && rm->sc_upper_bound)
            printf(" upperbound");
        printf(" nodes %" PRIu64 " nps %" PRIu64 " hashfull %u time %" PRId64 " pv",
               nodes, nodes * 1000 / (u64)elapsed, (unsigned)hashfull, elapsed);

        char uci[6];
        for (u16 j = 0; j < rm->pv_len; j++) {
            move_to_uci(rm->pv[j], uci);
            printf(" %s", uci);
        }
        printf("\n");
    }

    fflush(stdout);
}

static Score qsearch(SearchWorker *w, Position *pos, SearchStackEntry *ss, Score alpha, Score beta, bool pv_node) {
    Move pv[MAX_PLY + 1];
    SearchCache *cache = w->pool->cache;

    if (pv_node) {
        (ss + 1)->pv = pv;
        ss->pv[0] = MOVE_NONE;
    }

    Move best_mv = MOVE_NONE;
    ss->in_check = CHECKERS_BITBOARD(pos) != 0;
    i32 move_count = 0;

    if (pv_node && w->sel_depth < ss->ply + 1)
        w->sel_depth = ss->ply + 1;

    if (is_draw(pos, ss->ply) || ss->ply >= MAX_PLY)
        return (ss->ply >= MAX_PLY && !ss->in_check) ? evaluate(pos) : SCORE_DRAW;

    Key k = KEY(pos);
    SearchCacheData cd;
    SearchCacheWriter cw;
    ss->cache_hit = search_cache_probe(cache, k, &cd, &cw);
    Move cache_mv = ss->cache_hit ? cd.mv : MOVE_NONE;
    Score cache_sc = ss->cache_hit ? score_from_search_cache(cd.sc, (u8)ss->ply, HALFMOVE_CLOCK(pos)) : SCORE_NONE;
    bool pv_hit = ss->cache_hit && cd.is_pv;

    if (!pv_node && cd.d >= DEPTH_QS && is_valid(cache_sc)
     && (cd.b & (cache_sc >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return cache_sc;

    Score unadjusted_static_eval = SCORE_NONE;
    Score best_sc, futility_base;
    i32 cv = correction_value(w, pos);

    if (ss->in_check)
        best_sc = futility_base = -SCORE_INFINITE;
    else {
        if (ss->cache_hit) {
            unadjusted_static_eval = cd.eval_sc;
            if (!is_valid(unadjusted_static_eval))
                unadjusted_static_eval = evaluate(pos);
            ss->static_eval = best_sc = corrected_static_eval(unadjusted_static_eval, cv);

            if (is_valid(cache_sc) && !is_decisive(cache_sc)
             && (cd.b & (cache_sc > best_sc ? BOUND_LOWER : BOUND_UPPER)))
                best_sc = cache_sc;
        }
        else {
            unadjusted_static_eval = (ss - 1)->current_mv != MOVE_NULL ? evaluate(pos) : -(ss - 1)->static_eval;
            ss->static_eval = best_sc = corrected_static_eval(unadjusted_static_eval, cv);
        }

        if (best_sc >= beta) {
            if (!is_decisive(best_sc))
                best_sc = (best_sc + beta) / 2;
            if (!ss->cache_hit)
                search_cache_write(cw, k, score_to_search_cache(best_sc, (u8)ss->ply), false, BOUND_LOWER,
                                   DEPTH_UNSEARCHED, MOVE_NONE, unadjusted_static_eval, search_cache_generation(cache));
            return best_sc;
        }

        if (best_sc > alpha)
            alpha = best_sc;

        futility_base = ss->static_eval + 301;
    }

    const PieceToHistory *cont_hists[MOVE_PICKER_CONTINUATION_PLIES] = {
        (ss - 1)->cont_hist, (ss - 2)->cont_hist, (ss - 3)->cont_hist,
        (ss - 4)->cont_hist, (ss - 5)->cont_hist, (ss - 6)->cont_hist
    };
    Square prev_sq = MOVE_IS_OK((ss - 1)->current_mv) ? TARGET_SQUARE_FROM_MOVE((ss - 1)->current_mv) : NO_SQUARE;

    MovePicker mp;
    move_picker_init(&mp, pos, cache_mv, DEPTH_QS, w->h, cont_hists, (u8)min_i32(ss->ply, 255));

    Move mv;
    while ((mv = move_picker_next(&mp)) != MOVE_NONE) {
        if (!is_legal(pos, mv))
            continue;

        bool gives_chk = gives_check(pos, mv);
        bool capture = is_capture_stage(pos, mv);
        Piece moved_p = MOVED_PIECE(pos, mv);
        Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);
        move_count++;

        if (!is_loss(best_sc) && HAS_NON_PAWN_MATERIAL(pos, SIDE_TO_MOVE(pos))) {
            if (!gives_chk && to_sq != prev_sq && !is_loss(futility_base) && MOVE_TYPE_FROM_MOVE(mv) != MOVE_TYPE_PROMOTION) {
                if (move_count > 2)
                    continue;

                Score futility_sc = futility_base + PIECE_VALUE(PIECE_ON_SQUARE(pos, to_sq));
                if (futility_sc <= alpha) {
                    best_sc = max_i32(best_sc, futility_sc);
                    continue;
                }

                if (!see_ge(pos, mv, alpha - futility_base)) {
                    best_sc = min_i32(alpha, futility_base);
                    continue;
                }
            }

            if (!capture
             && cont_hists[0]->vals[moved_p][to_sq] + cont_hists[1]->vals[moved_p][to_sq]
                + w->h->pawn[PAWN_STRUCTURE_INDEX(pos)][moved_p][to_sq] <= 5228)
                continue;

            if (!see_ge(pos, mv, -80))
                continue;
        }

        ss->current_mv = mv;
        ss->cont_hist = &w->h->continuation[ss->in_check][capture].tables[moved_p][to_sq];

        add_node(w);
        do_move(pos, mv, &w->sts[ss->ply + 1]);
        search_cache_prefetch(cache, KEY(pos));
        Score sc = -qsearch(w, pos, ss + 1, -beta, -alpha, pv_node);
        undo_move(pos, mv);

        if (sc > best_sc) {
            best_sc = sc;
            if (sc > alpha) {
                best_mv = mv;
                if (pv_node)
                    update_pv(ss->pv, mv, (ss + 1)->pv);
                if (sc < beta)
                    alpha = sc;
                else
                    break;
            }
        }
    }

    if (ss->in_check && best_sc == -SCORE_INFINITE)
        return MATED_IN(ss->ply);

    if (!is_decisive(best_sc) && best_sc > beta)
        best_sc = (best_sc + beta) / 2;

    search_cache_write(cw, k, score_to_search_cache(best_sc, (u8)ss->ply), pv_hit,
                       best_sc >= beta ? BOUND_LOWER : BOUND_UPPER, DEPTH_QS, best_mv,
                       unadjusted_static_eval, search_cache_generation(cache));

    return best_sc;
}

static inline i32 futility_margin(Depth d, bool no_cache_cut_node, bool improving, bool opponent_worsening) {
    i32 futility_mult = 118 - 33 * no_cache_cut_node;
    i32 improving_deduction = improving * futility_mult * 2;
    i32 worsening_deduction = opponent_worsening * futility_mult / 3;
    return futility_mult * d - improving_deduction - worsening_deduction;
}

static inline i32 futility_move_count(bool improving, Depth d) {
    return (3 + d * d) / (2 - improving);
}

static inline i32 reduction(const SearchWorker *w, bool improving, Depth d, i32 move_count, i32 delta) {
    i32 reduction_scale = reductions[min_i32(d, MAX_MOVES - 1)] * reductions[min_i32(move_count, MAX_MOVES - 1)];
    return reduction_scale - delta * 764 / max_i32(w->root_delta, 1) + !improving * reduction_scale * 191 / 512 + 1087;
}

static Score search(SearchWorker *w, Position *pos, SearchStackEntry *ss, Score alpha, Score beta, Depth depth,
                    bool cut_node, NodeType node_type) {
    bool pv_node = node_type != NODE_TYPE_NON_PV;
    bool root_node = node_type == NODE_TYPE_ROOT;
    bool all_node = !(pv_node || cut_node);
    SearchPool *pool = w->pool;
    SearchCache *cache = pool->cache;

    if (depth <= 0)
        return qsearch(w, pos, ss, alpha, beta, pv_node);

    if (depth > MAX_PLY - 1)
        depth = MAX_PLY - 1;

    Move pv[MAX_PLY + 1];
    Move captures_searched[32], quiets_searched[32];
    u8 captures_count = 0, quiets_count = 0;

    ss->in_check = CHECKERS_BITBOARD(pos) != 0;
    bool prior_capture = CAPTURED_PIECE(pos) != NO_PIECE;
    Color stm_c = SIDE_TO_MOVE(pos);
    ss->move_count = 0;
    Score best_sc = -SCORE_INFINITE;
    Score max_sc = SCORE_INFINITE;

    if (w->idx == 0)
        check_time(w);

    if (pv_node && w->sel_depth < ss->ply + 1)
        w->sel_depth = ss->ply + 1;

    if (!root_node) {
        if (stopped(pool) || is_draw(pos, ss->ply) || ss->ply >= MAX_PLY)
            return (ss->ply >= MAX_PLY && !ss->in_check) ? evaluate(pos) : score_draw(w);

        alpha = max_i32(MATED_IN(ss->ply), alpha);
        beta = min_i32(MATE_IN(ss->ply + 1), beta);
        if (alpha >= beta)
            return alpha;
    }

    (ss + 1)->excluded_mv = MOVE_NONE;
    Move best_mv = MOVE_NONE;
    (ss + 2)->cutoff_cnt = 0;
    ss->stat_score = 0;
    Square prev_sq = MOVE_IS_OK((ss - 1)->current_mv) ? TARGET_SQUARE_FROM_MOVE((ss - 1)->current_mv) : NO_SQUARE;

    Move excluded_mv = ss->excluded_mv;
    Key k = KEY(pos);
    SearchCacheData cd;
    SearchCacheWriter cw;
    ss->cache_hit = search_cache_probe(cache, k, &cd, &cw);
    Move cache_mv = root_node ? w->root_moves[w->pv_idx].pv[0] : (ss->cache_hit ? cd.mv : MOVE_NONE);

    if (cache_mv != MOVE_NONE && !is_pseudo_legal(pos, cache_mv))
        cache_mv = MOVE_NONE;
    Score cache_sc = ss->cache_hit ? score_from_search_cache(cd.sc, (u8)ss->ply, HALFMOVE_CLOCK(pos)) : SCORE_NONE;
    ss->cache_pv = excluded_mv ? ss->cache_pv : (pv_node || (ss->cache_hit && cd.is_pv));
    bool cache_capture = cache_mv != MOVE_NONE && is_capture_stage(pos, cache_mv);

    if (!pv_node && !excluded_mv && cd.d > depth - (cache_sc <= beta) && is_valid(cache_sc)
     && (cd.b & (cache_sc >= beta ? BOUND_LOWER : BOUND_UPPER))
     && (cut_node == (cache_sc >= beta) || depth > 9)) {
        if (cache_mv != MOVE_NONE && cache_sc >= beta) {
            if (!cache_capture)
                update_quiet_histories(w, pos, ss, cache_mv, stat_bonus(depth) * 747 / 1024);

            if (prev_sq != NO_SQUARE && (ss - 1)->move_count <= 2 && !prior_capture)
                update_continuation_histories(ss - 1, PIECE_ON_SQUARE(pos, prev_sq), prev_sq,
                                              -stat_malus(depth + 1) * 1091 / 1024);
        }

        if (HALFMOVE_CLOCK(pos) < 90)
            return cache_sc;
    }

    Score unadjusted_static_eval = SCORE_NONE;
    Score eval;
    bool improving, opponent_worsening;
    i32 cv = correction_value(w, pos);

    if (ss->in_check) {
        ss->static_eval = eval = (ss - 2)->static_eval;
        improving = false;
        goto moves_loop;
    }
    else if (excluded_mv) {
        unadjusted_static_eval = eval = ss->static_eval;
    }
    else if (ss->cache_hit) {
        unadjusted_static_eval = cd.eval_sc;
        if (!is_valid(unadjusted_static_eval))
            unadjusted_static_eval = evaluate(pos);
        ss->static_eval = eval = corrected_static_eval(unadjusted_static_eval, cv);

        if (is_valid(cache_sc) && (cd.b & (cache_sc > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = cache_sc;
    }
    else {
        unadjusted_static_eval = evaluate(pos);
        ss->static_eval = eval = corrected_static_eval(unadjusted_static_eval, cv);

        search_cache_write(cw, k, SCORE_NONE, ss->cache_pv, BOUND_NONE, DEPTH_UNSEARCHED, MOVE_NONE,
                           unadjusted_static_eval, search_cache_generation(cache));
    }

    if (MOVE_IS_OK((ss - 1)->current_mv) && !(ss - 1)->in_check && !prior_capture) {
        i32 bonus = clamp_i32(-10 * ((ss - 1)->static_eval + ss->static_eval), -1831, 1428) + 623;
        history_update(&w->h->butterfly[OPPOSITE_COLOR(stm_c)][FROM_TO_INDEX((ss - 1)->current_mv)],
                       bonus * 1340 / 1024, BUTTERFLY_HISTORY_LIMIT);
        Piece prev_p = PIECE_ON_SQUARE(pos, prev_sq);
        if (PIECE_TYPE_FROM_PIECE(prev_p) != PIECE_TYPE_PAWN && MOVE_TYPE_FROM_MOVE((ss - 1)->current_mv) != MOVE_TYPE_PROMOTION)
            history_update(&w->h->pawn[PAWN_STRUCTURE_INDEX(pos)][prev_p][prev_sq], bonus * 1159 / 1024, PAWN_HISTORY_LIMIT);
    }

    improving = ss->static_eval > (ss - 2)->static_eval;
    opponent_worsening = ss->static_eval + (ss - 1)->static_eval > 2;

    if (!pv_node && eval < alpha - 462 - 297 * depth * depth)
        return qsearch(w, pos, ss, alpha - 1, alpha, false);

    if (!ss->cache_pv && depth < 14
     && eval - futility_margin(depth, cut_node && !ss->cache_hit, improving, opponent_worsening)
              - (ss - 1)->stat_score / 290 >= beta
     && eval >= beta && (cache_mv == MOVE_NONE || cache_capture)
     && !is_loss(beta) && !is_win(eval))
        return beta + (eval - beta) / 3;

    if (cut_node && (ss - 1)->current_mv != MOVE_NULL && eval >= beta
     && ss->static_eval >= beta - 21 * depth + 421 && !excluded_mv
     && HAS_NON_PAWN_MATERIAL(pos, stm_c) && ss->ply >= w->nmp_min_ply && !is_loss(beta)) {
        Depth r = (Depth)(min_i32((eval - beta) / 235, 7) + depth / 3 + 5);

        ss->current_mv = MOVE_NULL;
        ss->cont_hist = &w->h->continuation[0][0].tables[NO_PIECE][0];

        do_null_move(pos, &w->sts[ss->ply + 1]);
        Score null_sc = -search(w, pos, ss + 1, -beta, -beta + 1, depth - r, false, NODE_TYPE_NON_PV);
        undo_null_move(pos);

        if (null_sc >= beta && !is_win(null_sc)) {
            if (w->nmp_min_ply || depth < 16)
                return null_sc;

            w->nmp_min_ply = (u16)(ss->ply + 3 * (depth - r) / 4);
            Score v = search(w, pos, ss, beta - 1, beta, depth - r, false, NODE_TYPE_NON_PV);
            w->nmp_min_ply = 0;

            if (v >= beta)
                return null_sc;
        }
    }

    improving |= ss->static_eval >= beta + 97;

    if (pv_node && cache_mv == MOVE_NONE)
        depth -= 3;
    if (depth <= 0)
        return qsearch(w, pos, ss, alpha, beta, true);
    if (cut_node && depth >= 8 && (cache_mv == MOVE_NONE || cd.b == BOUND_UPPER))
        depth -= 1 + (cache_mv == MOVE_NONE);

    {
        Score probcut_beta = beta + 187 - 56 * improving;
        if (depth > 3 && !is_decisive(beta) && !(is_valid(cache_sc) && cache_sc < probcut_beta)) {
            MovePicker mp;
            move_picker_init_probcut(&mp, pos, cache_mv, probcut_beta - ss->static_eval, w->h);
            Depth probcut_depth = (Depth)max_i32(depth - 4, 0);
            Move mv;

            while ((mv = move_picker_next(&mp)) != MOVE_NONE) {
                if (mv == excluded_mv || !is_legal(pos, mv))
                    continue;

                Piece moved_p = MOVED_PIECE(pos, mv);
                ss->current_mv = mv;
                ss->is_cache_mv = mv == cache_mv;
                ss->cont_hist = &w->h->continuation[ss->in_check][true].tables[moved_p][TARGET_SQUARE_FROM_MOVE(mv)];

                add_node(w);
                do_move(pos, mv, &w->sts[ss->ply + 1]);

                Score sc = -qsearch(w, pos, ss + 1, -probcut_beta, -probcut_beta + 1, false);
                if (sc >= probcut_beta && probcut_depth > 0)
                    sc = -search(w, pos, ss + 1, -probcut_beta, -probcut_beta + 1, probcut_depth, !cut_node, NODE_TYPE_NON_PV);

                undo_move(pos, mv);

                if (sc >= probcut_beta) {
                    search_cache_write(cw, k, score_to_search_cache(sc, (u8)ss->ply), ss->cache_pv, BOUND_LOWER,
                                       probcut_depth + 1, mv, unadjusted_static_eval, search_cache_generation(cache));
                    if (!is_decisive(sc))
                        return sc - (probcut_beta - beta);
                }
            }
        }
    }

moves_loop:
    {
        Score probcut_beta = beta + 413;
        if ((cd.b & BOUND_LOWER) && cd.d >= depth - 4 && cache_sc >= probcut_beta && !is_decisive(beta)
         && is_valid(cache_sc) && !is_decisive(cache_sc))
            return probcut_beta;
    }

    const PieceToHistory *cont_hists[MOVE_PICKER_CONTINUATION_PLIES] = {
        (ss - 1)->cont_hist, (ss - 2)->cont_hist, (ss - 3)->cont_hist,
        (ss - 4)->cont_hist, (ss - 5)->cont_hist, (ss - 6)->cont_hist
    };

    MovePicker mp;
    move_picker_init(&mp, pos, cache_mv, depth, w->h, cont_hists, (u8)min_i32(ss->ply, 255));

    i32 move_count = 0;
    Move mv;

    while ((mv = move_picker_next(&mp)) != MOVE_NONE) {
        if (mv == excluded_mv || !is_legal(pos, mv))
            continue;

        if (root_node && !is_searchable_root_move(w, mv))
            continue;

        ss->move_count = ++move_count;

        if (pv_node)
            (ss + 1)->pv = NULL;

        Depth extension = 0;
        bool capture = is_capture_stage(pos, mv);
        Piece moved_p = MOVED_PIECE(pos, mv);
        Square to_sq = TARGET_SQUARE_FROM_MOVE(mv);
        bool gives_chk = gives_check(pos, mv);

        Depth new_depth = depth - 1;
        i32 delta = beta - alpha;
        i32 r = reduction(w, improving, depth, move_count, delta);

        if (ss->cache_pv)
            r -= 1037 + (cache_sc > alpha) * 965 + (cd.d >= depth) * 960;

        if (!root_node && HAS_NON_PAWN_MATERIAL(pos, stm_c) && !is_loss(best_sc)) {
            if (move_count >= futility_move_count(improving, depth))
                move_picker_skip_quiets(&mp);

            i32 lmr_depth = new_depth - r / 1024;

            if (capture || gives_chk) {
                Piece captured_p = PIECE_ON_SQUARE(pos, to_sq);
                i32 capture_hist = w->h->capture[moved_p][to_sq][CAPTURED_PIECE_TYPE_INDEX(captured_p)];

                if (!gives_chk && lmr_depth < 7 && !ss->in_check) {
                    Score futility_sc = ss->static_eval + 271 + 243 * lmr_depth + PIECE_VALUE(captured_p) + capture_hist / 7;
                    if (futility_sc <= alpha)
                        continue;
                }

                i32 see_hist = clamp_i32(capture_hist / 37, -152 * depth, 141 * depth);
                if (!see_ge(pos, mv, -156 * depth - see_hist))
                    continue;
            }
            else {
                i32 history = cont_hists[0]->vals[moved_p][to_sq] + cont_hists[1]->vals[moved_p][to_sq]
                            + w->h->pawn[PAWN_STRUCTURE_INDEX(pos)][moved_p][to_sq];

                if (history < -3901 * depth)
                    continue;

                history += 2 * w->h->butterfly[stm_c][FROM_TO_INDEX(mv)];
                lmr_depth += history / 3459;

                Score futility_sc = ss->static_eval + (best_sc < ss->static_eval - 47 ? 137 : 47) + 142 * lmr_depth;

                if (!ss->in_check && lmr_depth < 12 && futility_sc <= alpha) {
                    if (best_sc <= futility_sc && !is_decisive(best_sc) && !is_win(futility_sc))
                        best_sc = futility_sc;
                    continue;
                }

                lmr_depth = max_i32(lmr_depth, 0);

                if (!see_ge(pos, mv, -26 * lmr_depth * lmr_depth))
                    continue;
            }
        }

        if (ss->ply < w->root_depth * 2) {
            if (!root_node && mv == cache_mv && !excluded_mv && depth >= 5 - (w->completed_depth > 33) + ss->cache_pv
             && is_valid(cache_sc) && !is_decisive(cache_sc) && (cd.b & BOUND_LOWER) && cd.d >= depth - 3) {
                Score singular_beta = cache_sc - (52 + 74 * (ss->cache_pv && !pv_node)) * depth / 64;
                Depth singular_depth = new_depth / 2;

                ss->excluded_mv = mv;
                Score sc = search(w, pos, ss, singular_beta - 1, singular_beta, singular_depth, cut_node, NODE_TYPE_NON_PV);
                ss->excluded_mv = MOVE_NONE;

                if (sc < singular_beta) {
                    i32 double_margin = 262 * pv_node - 188 * !cache_capture - abs_i32(cv) / 248873;
                    i32 triple_margin = 88 + 265 * pv_node - 256 * !cache_capture + 93 * ss->cache_pv - abs_i32(cv) / 255331;
                    extension = (Depth)(1 + (sc < singular_beta - double_margin) + (sc < singular_beta - triple_margin));
                    depth += (!pv_node && depth < 14);
                }
                else if (sc >= beta && !is_decisive(sc))
                    return sc;
                else if (cache_sc >= beta)
                    extension = -3;
                else if (cut_node)
                    extension = -2;
            }
        }

        new_depth += extension;

        ss->current_mv = mv;
        ss->is_cache_mv = mv == cache_mv;
        ss->cont_hist = &w->h->continuation[ss->in_check][capture].tables[moved_p][to_sq];
        u64 nodes_before = root_node ? worker_nodes(w) : 0;

        add_node(w);
        do_move(pos, mv, &w->sts[ss->ply + 1]);
        search_cache_prefetch(cache, KEY(pos));

        if (pv_node)
            r -= 1018;
        r += 307 - move_count * 64;
        r -= abs_i32(cv) / 34112;
        if (cut_node)
            r += 2355 - (cd.d >= depth && ss->cache_pv) * 1141;
        if (cache_capture && !capture)
            r += 1087 + (depth < 8) * 990;
        if ((ss + 1)->cutoff_cnt > 3)
            r += 940 + all_node * 887;
        else if (mv == cache_mv)
            r -= 1960;

        if (capture)
            ss->stat_score = 7 * PIECE_VALUE(CAPTURED_PIECE(pos))
                           + w->h->capture[moved_p][to_sq][CAPTURED_PIECE_TYPE_INDEX(CAPTURED_PIECE(pos))] - 4666;
        else
            ss->stat_score = 2 * w->h->butterfly[stm_c][FROM_TO_INDEX(mv)]
                           + cont_hists[0]->vals[moved_p][to_sq] + cont_hists[1]->vals[moved_p][to_sq] - 3874;

        r -= ss->stat_score * 1451 / 16384;

        Score sc;

        if (depth >= 2 && move_count > 1) {
            Depth d = (Depth)max_i32(1, min_i32(new_depth - r / 1024, new_depth + !all_node + (pv_node && best_mv == MOVE_NONE)));
            ss->reduction = new_depth - d;
            sc = -search(w, pos, ss + 1, -(alpha + 1), -alpha, d, true, NODE_TYPE_NON_PV);
            ss->reduction = 0;

            if (sc > alpha && d < new_depth) {
                bool do_deeper = sc > best_sc + 40 + 2 * new_depth;
                bool do_shallower = sc < best_sc + 10;
                new_depth += do_deeper - do_shallower;

                if (new_depth > d)
                    sc = -search(w, pos, ss + 1, -(alpha + 1), -alpha, new_depth, !cut_node, NODE_TYPE_NON_PV);

                update_continuation_histories(ss, moved_p, to_sq, (sc >= beta) * 2048);
            }
        }
        else if (!pv_node || move_count > 1) {
            if (cache_mv == MOVE_NONE)
                r += 2111;
            sc = -search(w, pos, ss + 1, -(alpha + 1), -alpha, new_depth - (r > 3444) - (r > 5588 && new_depth > 2),
                         !cut_node, NODE_TYPE_NON_PV);
        }
        else
            sc = -SCORE_INFINITE;   

        if (pv_node && (move_count == 1 || sc > alpha)) {
            (ss + 1)->pv = pv;
            (ss + 1)->pv[0] = MOVE_NONE;

            if (mv == cache_mv && w->root_depth > 8)
                new_depth = (Depth)max_i32(new_depth, 1);

            sc = -search(w, pos, ss + 1, -beta, -alpha, new_depth, false, NODE_TYPE_PV);
        }

        undo_move(pos, mv);

        if (stopped(pool))
            return SCORE_ZERO;

        if (root_node) {
            RootMove *rm = find_root_move(w, mv);
            rm->effort += worker_nodes(w) - nodes_before;
            rm->average_sc = rm->average_sc != -SCORE_INFINITE ? (sc + rm->average_sc) / 2 : sc;
            rm->mean_squared_sc = rm->mean_squared_sc != -(i64)SCORE_INFINITE * SCORE_INFINITE
                                ? ((i64)sc * abs_i32(sc) + rm->mean_squared_sc) / 2
                                : (i64)sc * abs_i32(sc);

            if (move_count == 1 || sc > alpha) {
                rm->sc = rm->uci_sc = sc;
                rm->sel_depth = w->sel_depth;
                rm->sc_lower_bound = rm->sc_upper_bound = false;

                if (sc >= beta) {
                    rm->sc_lower_bound = true;
                    rm->uci_sc = beta;
                }
                else if (sc <= alpha) {
                    rm->sc_upper_bound = true;
                    rm->uci_sc = alpha;
                }

                rm->pv_len = 1;
                for (Move *m = (ss + 1)->pv; m && *m != MOVE_NONE && rm->pv_len < MAX_PLY; m++)
                    rm->pv[rm->pv_len++] = *m;

                if (move_count > 1 && !w->pv_idx)
                    atomic_store_explicit(&w->best_move_changes, atomic_load_explicit(&w->best_move_changes, memory_order_relaxed) + 1, memory_order_relaxed);
            }
            else
                rm->sc = -SCORE_INFINITE;  
        }

        i32 inc = (sc == best_sc && ss->ply + 2 >= w->root_depth && (worker_nodes(w) & 15) == 0 && !is_win(abs_i32(sc) + 1));

        if (sc + inc > best_sc) {
            best_sc = sc;

            if (sc + inc > alpha) {
                best_mv = mv;

                if (pv_node && !root_node)
                    update_pv(ss->pv, mv, (ss + 1)->pv);

                if (sc >= beta) {
                    ss->cutoff_cnt += (extension < 2) || pv_node;
                    break;
                }

                if (depth > 2 && depth < 14 && !is_decisive(sc))
                    depth -= 2;

                alpha = sc;
            }
        }

        if (mv != best_mv && move_count <= 32) {
            if (capture && captures_count < 32)
                captures_searched[captures_count++] = mv;
            else if (!capture && quiets_count < 32)
                quiets_searched[quiets_count++] = mv;
        }
    }


    if (!pv_node && best_sc >= beta && !is_decisive(best_sc) && !is_decisive(beta) && !is_decisive(alpha))
        best_sc = (best_sc * depth + beta) / (depth + 1);

    if (!move_count)
        best_sc = excluded_mv ? alpha : ss->in_check ? MATED_IN(ss->ply) : SCORE_DRAW;
    else if (best_mv != MOVE_NONE)
        update_all_stats(w, pos, ss, best_mv, prev_sq, quiets_searched, quiets_count, captures_searched,
                         captures_count, depth, best_mv == cache_mv, move_count);
    else if (!prior_capture && prev_sq != NO_SQUARE) {
        i32 bonus_scale = 118 * (depth > 5) + 36 * !all_node + 161 * ((ss - 1)->move_count > 8)
                        + 133 * (!ss->in_check && best_sc <= ss->static_eval - 107)
                        + 120 * (!(ss - 1)->in_check && best_sc <= -(ss - 1)->static_eval - 84)
                        + 81 * (ss - 1)->is_cache_mv + 100 * (ss->cutoff_cnt <= 3)
                        + min_i32(-(ss - 1)->stat_score / 108, 320);
        bonus_scale = max_i32(bonus_scale, 0);
        i32 scaled_bonus = stat_bonus(depth) * bonus_scale;
        Piece prev_p = PIECE_ON_SQUARE(pos, prev_sq);

        update_continuation_histories(ss - 1, prev_p, prev_sq, scaled_bonus * 416 / 32768);
        history_update(&w->h->butterfly[OPPOSITE_COLOR(stm_c)][FROM_TO_INDEX((ss - 1)->current_mv)],
                       scaled_bonus * 212 / 32768, BUTTERFLY_HISTORY_LIMIT);
        if (PIECE_TYPE_FROM_PIECE(prev_p) != PIECE_TYPE_PAWN && MOVE_TYPE_FROM_MOVE((ss - 1)->current_mv) != MOVE_TYPE_PROMOTION)
            history_update(&w->h->pawn[PAWN_STRUCTURE_INDEX(pos)][prev_p][prev_sq], scaled_bonus * 1073 / 32768, PAWN_HISTORY_LIMIT);
    }
    else if (prior_capture && prev_sq != NO_SQUARE) {
        Piece captured_p = CAPTURED_PIECE(pos);
        history_update(&w->h->capture[PIECE_ON_SQUARE(pos, prev_sq)][prev_sq][CAPTURED_PIECE_TYPE_INDEX(captured_p)],
                       stat_bonus(depth) * 2, CAPTURE_HISTORY_LIMIT);
    }

    if (pv_node)
        best_sc = min_i32(best_sc, max_sc);

    if (best_sc <= alpha)
        ss->cache_pv = ss->cache_pv || ((ss - 1)->cache_pv && depth > 3);

    if (!excluded_mv && !(root_node && w->pv_idx))
        search_cache_write(cw, k, score_to_search_cache(best_sc, (u8)ss->ply), ss->cache_pv,
                           best_sc >= beta ? BOUND_LOWER : (pv_node && best_mv != MOVE_NONE) ? BOUND_EXACT : BOUND_UPPER,
                           depth, best_mv, unadjusted_static_eval, search_cache_generation(cache));

    if (!ss->in_check && !(best_mv != MOVE_NONE && IS_CAPTURE(pos, best_mv))
     && ((best_sc < ss->static_eval && best_sc < beta) || (best_sc > ss->static_eval && best_mv != MOVE_NONE))) {
        i32 bonus = clamp_i32((best_sc - ss->static_eval) * depth / 8, -CORRECTION_HISTORY_LIMIT / 4, CORRECTION_HISTORY_LIMIT / 4);
        update_correction_history(w, pos, bonus);
    }

    return best_sc;
}

static void iterative_deepening(SearchWorker *w) {
    SearchPool *pool = w->pool;
    bool is_main = w->idx == 0;
    Position *pos = &w->root_pos;
    Color stm_c = SIDE_TO_MOVE(pos);

    Move pv[MAX_PLY + 1];
    Depth last_best_move_depth = 0;
    Score last_best_sc = -SCORE_INFINITE;
    RootMove last_best = w->root_moves[0];
    Score best_sc = -SCORE_INFINITE;
    double time_reduction = 1.0, total_best_move_changes = 0;
    u8 iter_idx = 0;
    i32 search_again_counter = 0;

    SearchStackEntry stack[MAX_PLY + STACK_OFFSET + 3];
    memset(stack, 0, sizeof(stack));
    SearchStackEntry *ss = stack + STACK_OFFSET;

    for (u8 i = 1; i <= STACK_OFFSET; i++) {
        (ss - i)->cont_hist = &w->h->continuation[0][0].tables[NO_PIECE][0];    
        (ss - i)->static_eval = SCORE_NONE;
    }
    for (u16 i = 0; i <= MAX_PLY + 2; i++)
        (ss + i)->ply = i;
    ss->pv = pv;

    if (is_main)
        for (u8 i = 0; i < 4; i++)
            pool->iter_scs[i] = pool->best_previous_sc == SCORE_INFINITE ? SCORE_ZERO : pool->best_previous_sc;

    histories_new_search(w->h);

    while (++w->root_depth < MAX_PLY && !stopped(pool)
        && !(pool->limits.depth && is_main && w->root_depth > pool->limits.depth)) {
        if (is_main)
            total_best_move_changes /= 2;

        for (u16 i = 0; i < w->root_moves_count; i++)
            w->root_moves[i].previous_sc = w->root_moves[i].sc;

        if (!atomic_load(&pool->increase_depth))
            search_again_counter++;

        u16 multi_pv = (u16)min_i32(pool->multi_pv, w->root_moves_count);

        for (w->pv_idx = 0; w->pv_idx < multi_pv; w->pv_idx++) {
            RootMove *pv_rm = &w->root_moves[w->pv_idx];
            w->sel_depth = 0;

            i32 delta = 5 + (i32)(llabs(pv_rm->mean_squared_sc) / 13461);
            Score avg = pv_rm->average_sc;
            Score alpha = max_i32(avg - delta, -SCORE_INFINITE);
            Score beta = min_i32(avg + delta, SCORE_INFINITE);
            i32 failed_high_cnt = 0;

            while (true) {
                Depth adjusted_depth = (Depth)max_i32(1, w->root_depth - failed_high_cnt - 3 * (search_again_counter + 1) / 4);
                w->root_delta = beta - alpha;
                best_sc = search(w, pos, ss, alpha, beta, adjusted_depth, false, NODE_TYPE_ROOT);

                sort_root_moves(w->root_moves + w->pv_idx, w->root_moves + w->root_moves_count);

                if (stopped(pool))
                    break;

                if (best_sc <= alpha) {
                    beta = (alpha + beta) / 2;
                    alpha = max_i32(best_sc - delta, -SCORE_INFINITE);
                    failed_high_cnt = 0;
                    atomic_store(&pool->stop_on_ponderhit, false);
                }
                else if (best_sc >= beta) {
                    beta = min_i32(best_sc + delta, SCORE_INFINITE);
                    failed_high_cnt++;
                }
                else
                    break;

                delta += delta / 3;
            }

            sort_root_moves(w->root_moves, w->root_moves + w->pv_idx + 1);

            if (is_main && pool->output && (stopped(pool) || w->pv_idx + 1 == multi_pv)
             && !(atomic_load(&pool->aborted) && is_loss(w->root_moves[0].uci_sc)))
                print_info(w, w->root_depth);

            if (stopped(pool))
                break;
        }
        w->pv_idx = 0;

        if (!stopped(pool))
            w->completed_depth = w->root_depth;

        if (atomic_load(&pool->aborted) && w->root_moves[0].sc != -SCORE_INFINITE && is_loss(w->root_moves[0].sc)) {
            RootMove *rm = find_root_move(w, last_best.pv[0]);
            if (rm) {
                RootMove tmp = *rm;
                memmove(w->root_moves + 1, w->root_moves, (size_t)(rm - w->root_moves) * sizeof(RootMove));
                w->root_moves[0] = tmp;
            }
            w->root_moves[0].pv_len = last_best.pv_len;
            memcpy(w->root_moves[0].pv, last_best.pv, last_best.pv_len * sizeof(Move));
            w->root_moves[0].sc = w->root_moves[0].uci_sc = last_best_sc;
        }
        else if (w->root_moves[0].pv[0] != last_best.pv[0]) {
            last_best = w->root_moves[0];
            last_best_sc = w->root_moves[0].sc;
            last_best_move_depth = w->root_depth;
        }

        if (!is_main)
            continue;

        if (uses_time_management(pool, stm_c) && !stopped(pool) && !atomic_load(&pool->stop_on_ponderhit)) {
            for (u16 i = 0; i < pool->workers_count; i++) {
                total_best_move_changes += (double)atomic_exchange_explicit(&pool->workers[i]->best_move_changes, 0, memory_order_relaxed);
            }

            u64 nodes = worker_nodes(w);
            i64 nodes_effort = (i64)(w->root_moves[0].effort * 100000 / (nodes ? nodes : 1));
            double falling_eval = (11 + 2 * (pool->best_previous_average_sc - best_sc) + (pool->iter_scs[iter_idx] - best_sc)) / 100.0;
            falling_eval = fmin(fmax(falling_eval, 0.580), 1.667);

            time_reduction = last_best_move_depth + 8 < w->completed_depth ? 1.495 : 0.687;
            double reduction_factor = (1.48 + pool->previous_time_reduction) / (2.17 * time_reduction);
            double best_move_instability = 1 + 1.88 * total_best_move_changes / pool->workers_count;
            double total_time = (double)pool->optimum_ms * falling_eval * reduction_factor * best_move_instability;

            if (w->root_moves_count == 1)
                total_time = fmin(500.0, total_time);

            i64 elapsed = now_ms() - pool->start_ms;

            bool pondering = atomic_load(&pool->ponder);

            if (w->completed_depth >= 10 && nodes_effort >= 97056 && (double)elapsed > total_time * 0.6540 && !pondering)
                atomic_store(&pool->stop, true);

            if ((double)elapsed > total_time) {
                if (pondering)
                    atomic_store(&pool->stop_on_ponderhit, true);
                else
                    atomic_store(&pool->stop, true);
            }
            else
                atomic_store(&pool->increase_depth, pondering || (double)elapsed <= total_time * 0.5138);
        }

        pool->iter_scs[iter_idx] = best_sc;
        iter_idx = (iter_idx + 1) & 3;
    }

    if (is_main)
        pool->previous_time_reduction = time_reduction;
}

static SearchWorker *best_worker(SearchPool *pool) {
    SearchWorker *best = pool->workers[0];
    Score min_sc = SCORE_NONE;

    for (u16 i = 0; i < pool->workers_count; i++)
        min_sc = min_i32(min_sc, pool->workers[i]->root_moves[0].sc);

    #define VOTING_VALUE(wk) ((i64)((wk)->root_moves[0].sc - min_sc + 14) * (wk)->completed_depth)

    for (u16 i = 0; i < pool->workers_count; i++) {
        pool->workers[i]->votes = 0;
        for (u16 j = 0; j < pool->workers_count; j++)
            if (pool->workers[j]->root_moves[0].pv[0] == pool->workers[i]->root_moves[0].pv[0])
                pool->workers[i]->votes += VOTING_VALUE(pool->workers[j]);
    }

    for (u16 i = 1; i < pool->workers_count; i++) {
        SearchWorker *wk = pool->workers[i];
        Score best_sc = best->root_moves[0].sc;
        Score new_sc = wk->root_moves[0].sc;
        bool best_in_win = is_win(best_sc);
        bool new_in_win = is_win(new_sc);
        bool best_in_loss = best_sc != -SCORE_INFINITE && is_loss(best_sc);
        bool new_in_loss = new_sc != -SCORE_INFINITE && is_loss(new_sc);

        bool better_voting_value = VOTING_VALUE(wk) * (wk->root_moves[0].pv_len > 2)
                                 > VOTING_VALUE(best) * (best->root_moves[0].pv_len > 2);

        if (best_in_win) {
            if (new_sc > best_sc)       
                best = wk;
        }
        else if (best_in_loss) {
            if (new_in_loss && new_sc < best_sc)    
                best = wk;
        }
        else if (new_in_win || new_in_loss
              || (!is_loss(new_sc) && (wk->votes > best->votes || (wk->votes == best->votes && better_voting_value))))
            best = wk;
    }

    #undef VOTING_VALUE
    return best;
}

static void main_search(SearchWorker *w);

static void start_worker(SearchWorker *w) {
    mutex_lock(&w->mutex);
    w->searching = true;
    cond_broadcast(&w->cond);
    mutex_unlock(&w->mutex);
}

static void wait_worker(SearchWorker *w) {
    mutex_lock(&w->mutex);
    while (w->searching)
        cond_wait(&w->cond, &w->mutex);
    mutex_unlock(&w->mutex);
}

static void idle_loop(void *arg) {
    SearchWorker *w = arg;

    while (true) {
        mutex_lock(&w->mutex);
        while (!w->searching && !w->exit)
            cond_wait(&w->cond, &w->mutex);
        bool exit = w->exit;
        mutex_unlock(&w->mutex);

        if (exit)
            return;

        if (w->idx == 0)
            main_search(w);
        else
            iterative_deepening(w);

        mutex_lock(&w->mutex);
        w->searching = false;
        cond_broadcast(&w->cond);
        mutex_unlock(&w->mutex);
    }
}

static void main_search(SearchWorker *w) {
    SearchPool *pool = w->pool;

    if (w->root_moves_count == 0) {
        pool->result = (SearchResult){
            .best_mv = MOVE_NONE, .ponder_mv = MOVE_NONE,
            .sc = CHECKERS_BITBOARD(&w->root_pos) ? -SCORE_MATE : SCORE_DRAW
        };
        if (pool->output) {
            printf("info depth 0 score %s\nbestmove (none)\n", CHECKERS_BITBOARD(&w->root_pos) ? "mate 0" : "cp 0");
            fflush(stdout);
        }
        return;
    }

    for (u16 i = 1; i < pool->workers_count; i++)
        start_worker(pool->workers[i]);

    iterative_deepening(w);

    while (!stopped(pool) && (pool->limits.infinite || atomic_load(&pool->ponder)))
        thread_sleep_ms(1);

    atomic_store(&pool->stop, true);
    for (u16 i = 1; i < pool->workers_count; i++)
        wait_worker(pool->workers[i]);

    SearchWorker *best = w;
    if (pool->multi_pv == 1 && !pool->limits.depth && w->root_moves[0].pv[0] != MOVE_NONE)
        best = best_worker(pool);

    pool->best_previous_sc = best->root_moves[0].sc;
    pool->best_previous_average_sc = best->root_moves[0].average_sc;

    if (best != w && pool->output)
        print_info(best, best->completed_depth);

    const RootMove *rm = &best->root_moves[0];
    pool->result = (SearchResult){
        .best_mv = rm->pv[0],
        .ponder_mv = rm->pv_len > 1 ? rm->pv[1] : MOVE_NONE,
        .sc = rm->sc,
        .depth = best->completed_depth,
        .nodes = pool_nodes(pool),
        .time_ms = now_ms() - pool->start_ms
    };

    if (pool->output) {
        char best_uci[6], ponder_uci[6];
        move_to_uci(pool->result.best_mv, best_uci);
        printf("bestmove %s", best_uci);
        if (pool->result.ponder_mv != MOVE_NONE) {
            move_to_uci(pool->result.ponder_mv, ponder_uci);
            printf(" ponder %s", ponder_uci);
        }
        printf("\n");
        fflush(stdout);
    }
}


static void *alloc_aligned_64(size_t size) {
    u8 *raw = calloc(1, size + 64 + sizeof(void *));
    if (!raw)
        return NULL;
    uintptr_t aligned = ((uintptr_t)(raw + sizeof(void *)) + 63) & ~(uintptr_t)63;
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
}

static void free_aligned_64(void *p) {
    if (p)
        free(((void **)p)[-1]);
}

static SearchWorker *worker_create(SearchPool *pool, u16 idx) {
    SearchWorker *w = alloc_aligned_64(sizeof(SearchWorker));
    if (!w)
        return NULL;

    w->pool = pool;
    w->idx = idx;
    w->h = histories_create();
    if (!w->h) {
        free_aligned_64(w);
        return NULL;
    }
    for (u32 i = 0; i < PAWN_CORRECTION_SIZE; i++)
        w->pawn_correction[i][COLOR_WHITE] = w->pawn_correction[i][COLOR_BLACK] = 0;

    mutex_init(&w->mutex);
    cond_init(&w->cond);

    if (!thread_create(&w->thread, idle_loop, w)) {
        cond_destroy(&w->cond);
        mutex_destroy(&w->mutex);
        histories_destroy(w->h);
        free_aligned_64(w);
        return NULL;
    }

    return w;
}

static void worker_destroy(SearchWorker *w) {
    mutex_lock(&w->mutex);
    w->exit = true;
    cond_broadcast(&w->cond);
    mutex_unlock(&w->mutex);

    thread_join(w->thread);
    cond_destroy(&w->cond);
    mutex_destroy(&w->mutex);
    histories_destroy(w->h);
    free_aligned_64(w);
}

static void destroy_workers(SearchPool *pool) {
    for (u16 i = 0; i < pool->workers_count; i++)
        worker_destroy(pool->workers[i]);
    free(pool->workers);
    pool->workers = NULL;
    pool->workers_count = 0;
}

bool search_pool_set_threads(SearchPool *pool, u16 threads) {
    if (pool->workers_count)
        wait_worker(pool->workers[0]);
    destroy_workers(pool);

    if (threads == 0)
        threads = 1;

    pool->workers = calloc(threads, sizeof(SearchWorker *));
    if (!pool->workers)
        return false;

    for (u16 i = 0; i < threads; i++) {
        pool->workers[i] = worker_create(pool, i);
        if (!pool->workers[i]) {
            destroy_workers(pool);
            return false;
        }
        pool->workers_count = i + 1;
    }

    return true;
}

SearchPool *search_pool_create(u16 threads, u64 cache_mb) {
    static bool reductions_ready;
    if (!reductions_ready) {
        init_reductions();
        reductions_ready = true;
    }

    SearchPool *pool = calloc(1, sizeof(SearchPool));
    if (!pool)
        return NULL;

    pool->output = true;
    pool->move_overhead_ms = DEFAULT_MOVE_OVERHEAD_MS;
    pool->multi_pv = 1;
    pool->ponder_enabled = false;
    pool->cache = search_cache_create(cache_mb);
    if (!pool->cache || !search_pool_set_threads(pool, threads)) {
        search_pool_destroy(pool);
        return NULL;
    }

    search_pool_clear(pool);
    return pool;
}

void search_pool_destroy(SearchPool *pool) {
    if (!pool)
        return;
    if (pool->workers_count)
        wait_worker(pool->workers[0]);
    destroy_workers(pool);
    search_cache_destroy(pool->cache);
    free(pool);
}

bool search_pool_set_cache_size(SearchPool *pool, u64 cache_mb) {
    wait_worker(pool->workers[0]);
    return search_cache_resize(pool->cache, cache_mb);
}

void search_pool_clear(SearchPool *pool) {
    wait_worker(pool->workers[0]);
    search_cache_clear(pool->cache);

    for (u16 i = 0; i < pool->workers_count; i++) {
        SearchWorker *w = pool->workers[i];
        histories_clear(w->h);
        memset(w->pawn_correction, 0, sizeof(w->pawn_correction));
    }

    pool->best_previous_sc = SCORE_INFINITE;
    pool->best_previous_average_sc = SCORE_INFINITE;
    pool->previous_time_reduction = 1.0;
    pool->original_time_adjust = -1;
}

void search_pool_set_output(SearchPool *pool, bool enabled) {
    pool->output = enabled;
}

void search_pool_start(SearchPool *pool, const Position *pos, const SearchLimits *limits) {
    wait_worker(pool->workers[0]);

    pool->limits = *limits;
    pool->start_ms = now_ms();
    pool->calls_cnt = 0;
    atomic_store(&pool->stop, false);
    atomic_store(&pool->aborted, false);
    atomic_store(&pool->increase_depth, true);
    atomic_store(&pool->ponder, limits->ponder);
    atomic_store(&pool->stop_on_ponderhit, false);

    ScoredMove smvs[MAX_MOVES];
    ScoredMove *end = generate_legal(pos, smvs);
    u16 root_moves_count = (u16)(end - smvs);

    nnue_evaluate(pos);

    for (u16 i = 0; i < pool->workers_count; i++) {
        SearchWorker *w = pool->workers[i];

        w->root_pos = *pos;
        w->sts[0] = *pos->st;
        w->root_pos.st = &w->sts[0];

        atomic_store(&w->nodes, 0);
        w->root_depth = w->completed_depth = 0;
        w->sel_depth = 0;
        w->nmp_min_ply = 0;
        atomic_store(&w->best_move_changes, 0);
        w->pv_idx = 0;

        w->root_moves_count = root_moves_count;
        for (u16 j = 0; j < root_moves_count; j++) {
            RootMove *rm = &w->root_moves[j];
            memset(rm, 0, sizeof(RootMove));
            rm->pv[0] = smvs[j].mv;
            rm->pv_len = 1;
            rm->sc = rm->previous_sc = rm->uci_sc = rm->average_sc = -SCORE_INFINITE;
            rm->mean_squared_sc = -(i64)SCORE_INFINITE * SCORE_INFINITE;
        }
    }

    search_cache_new_search(pool->cache);
    init_time_management(pool, SIDE_TO_MOVE(pos), GAME_PLY(pos));

    start_worker(pool->workers[0]);
}

void search_pool_stop(SearchPool *pool) {
    atomic_store(&pool->stop, true);
}

SearchResult search_pool_wait(SearchPool *pool) {
    wait_worker(pool->workers[0]);
    return pool->result;
}

void search_pool_ponderhit(SearchPool *pool) {
    atomic_store(&pool->ponder, false);
    if (atomic_load(&pool->stop_on_ponderhit))
        atomic_store(&pool->stop, true);
}

void search_pool_set_move_overhead(SearchPool *pool, i64 ms) {
    wait_worker(pool->workers[0]);
    pool->move_overhead_ms = ms < 0 ? 0 : ms;
}

void search_pool_set_multi_pv(SearchPool *pool, u16 lines) {
    wait_worker(pool->workers[0]);
    pool->multi_pv = lines < 1 ? 1 : lines;
}

void search_pool_set_ponder(SearchPool *pool, bool enabled) {
    wait_worker(pool->workers[0]);
    pool->ponder_enabled = enabled;
}