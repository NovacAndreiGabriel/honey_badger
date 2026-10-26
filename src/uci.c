#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "uci.h"
#include "position.h"
#include "move_generation.h"
#include "nnue.h"
#include "search.h"

#define DEFAULT_THREADS (1)
#define MAX_THREADS (1024)
#define DEFAULT_HASH_MB (16)
#define MAX_HASH_MB (65536)
#define DEFAULT_MOVE_OVERHEAD_MS (10)
#define MAX_MOVE_OVERHEAD_MS (5000)
#define MAX_MULTI_PV (256)

#define MAX_GAME_PLIES (2048)

typedef struct UciState {
    SearchPool *pool;
    Position pos;
    PlyState *sts;       
    u16 threads;
} UciState;


static i64 now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

static char *read_line(void) {
    size_t capacity = 256, len = 0;
    char *line = malloc(capacity);
    if (!line)
        return NULL;

    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (len + 1 >= capacity) {
            char *bigger = realloc(line, capacity * 2);
            if (!bigger) {
                free(line);
                return NULL;
            }
            line = bigger;
            capacity *= 2;
        }
        line[len++] = (char)c;
    }

    if (c == EOF && len == 0) {
        free(line);
        return NULL;
    }

    if (len > 0 && line[len - 1] == '\r')
        len--;
    line[len] = '\0';
    return line;
}

static char *next_token(char **cursor) {
    char *s = *cursor;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return NULL;

    char *token = s;
    while (*s && !isspace((unsigned char)*s))
        s++;
    if (*s)
        *s++ = '\0';

    *cursor = s;
    return token;
}

static bool equals_ignore_case(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

static Move parse_uci_move(const Position *pos, const char *token) {
    ScoredMove smvs[MAX_MOVES];
    ScoredMove *end = generate_legal(pos, smvs);
    char uci[6];

    for (ScoredMove *cur = smvs; cur < end; cur++) {
        move_to_uci(cur->mv, uci);
        if (strcmp(uci, token) == 0)
            return cur->mv;
    }

    return MOVE_NONE;
}

static void cmd_uci(void) {
    printf("id name %s\n", ENGINE_NAME);
    printf("id author %s\n", ENGINE_AUTHOR);
    printf("option name Threads type spin default %d min 1 max %d\n", DEFAULT_THREADS, MAX_THREADS);
    printf("option name Hash type spin default %d min 1 max %d\n", DEFAULT_HASH_MB, MAX_HASH_MB);
    printf("option name Clear Hash type button\n");
    printf("option name Ponder type check default false\n");
    printf("option name MultiPV type spin default 1 min 1 max %d\n", MAX_MULTI_PV);
    printf("option name Move Overhead type spin default %d min 0 max %d\n", DEFAULT_MOVE_OVERHEAD_MS, MAX_MOVE_OVERHEAD_MS);
    printf("uciok\n");
    fflush(stdout);
}

static void cmd_setoption(UciState *uci, char *args) {
    char name[64] = { 0 }, value[64] = { 0 };
    char *token;
    bool in_name = false, in_value = false;

    while ((token = next_token(&args))) {
        if (strcmp(token, "name") == 0) {
            in_name = true;
            in_value = false;
        }
        else if (strcmp(token, "value") == 0) {
            in_name = false;
            in_value = true;
        }
        else if (in_name) {
            if (name[0])
                strncat(name, " ", sizeof(name) - strlen(name) - 1);
            strncat(name, token, sizeof(name) - strlen(name) - 1);
        }
        else if (in_value) {
            if (value[0])
                strncat(value, " ", sizeof(value) - strlen(value) - 1);
            strncat(value, token, sizeof(value) - strlen(value) - 1);
        }
    }

    search_pool_wait(uci->pool);

    if (equals_ignore_case(name, "Threads")) {
        long threads = strtol(value, NULL, 10);
        threads = threads < 1 ? 1 : (threads > MAX_THREADS ? MAX_THREADS : threads);
        if (search_pool_set_threads(uci->pool, (u16)threads))
            uci->threads = (u16)threads;
        else {
            printf("info string could not start %ld threads, using 1\n", threads);
            search_pool_set_threads(uci->pool, 1);
            uci->threads = 1;
        }
    }
    else if (equals_ignore_case(name, "Hash")) {
        long mb = strtol(value, NULL, 10);
        mb = mb < 1 ? 1 : (mb > MAX_HASH_MB ? MAX_HASH_MB : mb);
        if (!search_pool_set_cache_size(uci->pool, (u64)mb)) {
            printf("info string could not allocate %ld MB, using %d MB\n", mb, DEFAULT_HASH_MB);
            search_pool_set_cache_size(uci->pool, DEFAULT_HASH_MB);
        }
    }
    else if (equals_ignore_case(name, "Clear Hash"))
        search_pool_clear(uci->pool);
    else if (equals_ignore_case(name, "Ponder"))
        search_pool_set_ponder(uci->pool, equals_ignore_case(value, "true"));
    else if (equals_ignore_case(name, "MultiPV")) {
        long lines = strtol(value, NULL, 10);
        search_pool_set_multi_pv(uci->pool, (u16)(lines < 1 ? 1 : (lines > MAX_MULTI_PV ? MAX_MULTI_PV : lines)));
    }
    else if (equals_ignore_case(name, "Move Overhead")) {
        long ms = strtol(value, NULL, 10);
        search_pool_set_move_overhead(uci->pool, ms < 0 ? 0 : (ms > MAX_MOVE_OVERHEAD_MS ? MAX_MOVE_OVERHEAD_MS : ms));
    }
    else
        printf("info string unknown option: %s\n", name);

    fflush(stdout);
}

static void cmd_position(UciState *uci, char *args) {
    search_pool_stop(uci->pool);
    search_pool_wait(uci->pool);

    char fen[128] = { 0 };
    char *token = next_token(&args);

    if (token && strcmp(token, "startpos") == 0) {
        strcpy(fen, START_FEN);
        token = next_token(&args);
    }
    else if (token && strcmp(token, "fen") == 0) {
        u8 fields = 0;
        while ((token = next_token(&args)) && strcmp(token, "moves") != 0) {
            if (fields++)
                strncat(fen, " ", sizeof(fen) - strlen(fen) - 1);
            strncat(fen, token, sizeof(fen) - strlen(fen) - 1);
        }
        if (fields == 4)
            strncat(fen, " 0 1", sizeof(fen) - strlen(fen) - 1);
    }
    else {
        printf("info string expected startpos or fen\n");
        fflush(stdout);
        return;
    }

    set_position_from_fen(&uci->pos, fen, &uci->sts[0]);

    if (token && strcmp(token, "moves") == 0) {
        u16 ply = 0;
        while ((token = next_token(&args))) {
            if (ply >= MAX_GAME_PLIES) {
                printf("info string game too long, ignoring moves after ply %d\n", MAX_GAME_PLIES);
                break;
            }

            Move mv = parse_uci_move(&uci->pos, token);
            if (mv == MOVE_NONE) {
                printf("info string illegal move: %s\n", token);
                break;
            }

            do_move(&uci->pos, mv, &uci->sts[++ply]);
        }
    }

    fflush(stdout);
}

static u64 perft(Position *pos, PlyState *sts, u8 depth) {
    ScoredMove smvs[MAX_MOVES];
    ScoredMove *end = generate_legal(pos, smvs);

    if (depth <= 1)
        return (u64)(end - smvs);

    u64 nodes = 0;
    for (ScoredMove *cur = smvs; cur < end; cur++) {
        do_move(pos, cur->mv, sts);
        nodes += perft(pos, sts + 1, depth - 1);
        undo_move(pos, cur->mv);
    }
    return nodes;
}

static void cmd_perft(UciState *uci, u8 depth) {
    static PlyState *perft_sts;
    if (!perft_sts)
        perft_sts = alloc_aligned_64(sizeof(PlyState) * 64);
    if (!perft_sts || depth == 0)
        return;

    ScoredMove smvs[MAX_MOVES];
    ScoredMove *end = generate_legal(&uci->pos, smvs);
    u64 total = 0;
    i64 start_ms = now_ms();
    char buffer[6];

    for (ScoredMove *cur = smvs; cur < end; cur++) {
        u64 nodes = 1;
        if (depth > 1) {
            do_move(&uci->pos, cur->mv, &perft_sts[0]);
            nodes = perft(&uci->pos, &perft_sts[1], depth - 1);
            undo_move(&uci->pos, cur->mv);
        }
        move_to_uci(cur->mv, buffer);
        printf("%s: %" PRIu64 "\n", buffer, nodes);
        total += nodes;
    }

    i64 ms = now_ms() - start_ms;
    printf("\nNodes searched: %" PRIu64 "\n", total);
    printf("Time: %" PRId64 " ms (%" PRIu64 " nodes/s)\n\n", ms, total * 1000 / (u64)(ms > 0 ? ms : 1));
    fflush(stdout);
}

static void cmd_go(UciState *uci, char *args) {
    search_pool_stop(uci->pool);
    search_pool_wait(uci->pool);

    SearchLimits limits = { 0 };
    char *token;

    while ((token = next_token(&args))) {
        char *value = NULL;
        #define NEXT_VALUE() ((value = next_token(&args)) ? strtoll(value, NULL, 10) : 0)

        if (strcmp(token, "wtime") == 0)            limits.time_ms[COLOR_WHITE] = NEXT_VALUE();
        else if (strcmp(token, "btime") == 0)       limits.time_ms[COLOR_BLACK] = NEXT_VALUE();
        else if (strcmp(token, "winc") == 0)        limits.inc_ms[COLOR_WHITE] = NEXT_VALUE();
        else if (strcmp(token, "binc") == 0)        limits.inc_ms[COLOR_BLACK] = NEXT_VALUE();
        else if (strcmp(token, "movestogo") == 0)   limits.moves_to_go = (u16)NEXT_VALUE();
        else if (strcmp(token, "movetime") == 0)    limits.move_time_ms = NEXT_VALUE();
        else if (strcmp(token, "depth") == 0)       limits.depth = (Depth)NEXT_VALUE();
        else if (strcmp(token, "nodes") == 0)       limits.nodes = (u64)NEXT_VALUE();
        else if (strcmp(token, "infinite") == 0)    limits.infinite = true;
        else if (strcmp(token, "ponder") == 0)      limits.ponder = true;
        else if (strcmp(token, "perft") == 0) {
            cmd_perft(uci, (u8)NEXT_VALUE());
            return;
        }

        #undef NEXT_VALUE
    }

    for (Color c = COLOR_WHITE; c < NO_COLOR; c++)
        if (limits.time_ms[c] < 0)
            limits.time_ms[c] = 1;

    search_pool_start(uci->pool, &uci->pos, &limits);
}

static void cmd_eval(UciState *uci) {
    Score sc = nnue_evaluate(&uci->pos);
    printf("NNUE evaluation: %+.2f (side to move), %+.2f (white)\n\n",
           sc / 100.0, (SIDE_TO_MOVE(&uci->pos) == COLOR_WHITE ? sc : -sc) / 100.0);
    fflush(stdout);
}

static void cmd_d(UciState *uci) {
    char fen[128];
    print_position(&uci->pos);
    fen_from_position(&uci->pos, fen);
    printf("\nFen: %s\n\n", fen);
    fflush(stdout);
}

static const char *BENCH_FENS[] = {
    START_FEN,
    "r3k2r/2pb1ppp/2pp1q2/p7/1nP1B3/1P2P3/P2N1PPP/R2QK2R w KQkq a6 0 14",
    "4rrk1/2p1b1p1/p1p3q1/4p3/2P2n1p/1P1NR2P/PB3PP1/3R1QK1 b - - 2 24",
    "r3qbrk/6p1/2b2pPp/p3pP1Q/PpPpP2P/3P1B2/2PB3K/R5R1 w - - 16 42",
    "6k1/1R3p2/6p1/2Bp3p/3P2q1/P7/1P2rQ1K/5R2 b - - 4 44",
    "8/8/1p2k1p1/3p3p/1p1P1P1P/1P2PK2/8/8 w - - 3 54",
    "7r/2p3k1/1p1p1qp1/1P1Bp3/p1P2r1P/P7/4R3/Q4RK1 w - - 0 36",
    "r1bq1rk1/pp2b1pp/n1pp1n2/3P1p2/2P1p3/2N1P2N/PP2BPPP/R1BQ1RK1 b - - 2 10",
    "3r3k/2r4p/1p1b3q/p4P2/P2Pp3/1B2P3/3BQ1RP/6K1 w - - 3 87",
    "2r4r/1p4k1/1Pnp4/3Qb1pq/8/4BpPp/5P2/2RR1BK1 w - - 0 42",
    "4q1bk/6b1/7p/p1p4p/PNPpP2P/KN4P1/3Q4/4R3 b - - 0 37",
    "2q3r1/1r2pk2/pp3pp1/2pP3p/P1Pb1BbP/1P4Q1/R3NPP1/4R1K1 w - - 2 34",
    "1r2r2k/1b4q1/pp5p/2pPp1p1/P3Pn2/1P1B1Q1P/2R3P1/4BR1K b - - 1 37",
    "r3kb1r/3n1pp1/p6p/2pPp2q/Pp2N3/3B2PP/1PQ2P2/R3K2R w KQkq - 0 1",
    "8/8/8/8/5kp1/P7/8/1K1N4 w - - 0 1",
    "8/3k4/8/8/8/4B3/4KB2/2B5 w - - 0 1",
};

void uci_bench(Depth depth) {
    SearchPool *pool = search_pool_create(1, DEFAULT_HASH_MB);
    if (!pool) {
        printf("bench: could not create the search pool\n");
        return;
    }
    search_pool_set_output(pool, false);

    u16 count = sizeof(BENCH_FENS) / sizeof(BENCH_FENS[0]);
    u64 total_nodes = 0;
    i64 total_ms = 0;

    for (u16 i = 0; i < count; i++) {
        Position pos;
        PlyState *st = alloc_aligned_64(sizeof(PlyState));
        if (!st)
            break;
        set_position_from_fen(&pos, BENCH_FENS[i], st);

        search_pool_clear(pool);
        SearchLimits limits = { .depth = depth };
        search_pool_start(pool, &pos, &limits);
        SearchResult result = search_pool_wait(pool);

        char best[6] = "none";
        if (result.best_mv != MOVE_NONE)
            move_to_uci(result.best_mv, best);
        printf("Position %2u/%u: bestmove %-5s nodes %10" PRIu64 "\n", (unsigned)(i + 1), (unsigned)count,
               best, result.nodes);

        total_nodes += result.nodes;
        total_ms += result.time_ms;
        free_aligned_64(st);
    }

    printf("\n===========================\n");
    printf("Total time (ms) : %" PRId64 "\n", total_ms);
    printf("Nodes searched  : %" PRIu64 "\n", total_nodes);
    printf("Nodes/second    : %" PRIu64 "\n", total_nodes * 1000 / (u64)(total_ms > 0 ? total_ms : 1));
    fflush(stdout);

    search_pool_destroy(pool);
}

void uci_loop(void) {
    UciState uci = { .threads = DEFAULT_THREADS };

    uci.sts = alloc_aligned_64(sizeof(PlyState) * (MAX_GAME_PLIES + 1));
    uci.pool = search_pool_create(DEFAULT_THREADS, DEFAULT_HASH_MB);
    if (!uci.sts || !uci.pool) {
        printf("info string out of memory\n");
        free_aligned_64(uci.sts);
        search_pool_destroy(uci.pool);
        return;
    }

    set_position_from_fen(&uci.pos, START_FEN, &uci.sts[0]);

    printf("%s (NNUE: %s)\n", ENGINE_NAME, nnue_simd_name());
    fflush(stdout);

    char *line;
    while ((line = read_line())) {
        char *args = line;
        char *cmd = next_token(&args);

        if (!cmd) {
            free(line);
            continue;
        }

        if (strcmp(cmd, "uci") == 0)
            cmd_uci();
        else if (strcmp(cmd, "isready") == 0) {
            printf("readyok\n");
            fflush(stdout);
        }
        else if (strcmp(cmd, "ucinewgame") == 0) {
            search_pool_stop(uci.pool);
            search_pool_clear(uci.pool);
        }
        else if (strcmp(cmd, "setoption") == 0)
            cmd_setoption(&uci, args);
        else if (strcmp(cmd, "position") == 0)
            cmd_position(&uci, args);
        else if (strcmp(cmd, "go") == 0)
            cmd_go(&uci, args);
        else if (strcmp(cmd, "stop") == 0)
            search_pool_stop(uci.pool);
        else if (strcmp(cmd, "ponderhit") == 0)
            search_pool_ponderhit(uci.pool);
        else if (strcmp(cmd, "quit") == 0) {
            free(line);
            break;
        }
        else if (strcmp(cmd, "d") == 0)
            cmd_d(&uci);
        else if (strcmp(cmd, "eval") == 0)
            cmd_eval(&uci);
        else if (strcmp(cmd, "bench") == 0) {
            char *depth = next_token(&args);
            search_pool_wait(uci.pool);
            uci_bench(depth ? (Depth)strtol(depth, NULL, 10) : BENCH_DEFAULT_DEPTH);
        }
        else {
            printf("Unknown command: '%s'\n", cmd);
            fflush(stdout);
        }

        free(line);
    }

    search_pool_stop(uci.pool);
    search_pool_wait(uci.pool);
    search_pool_destroy(uci.pool);
    free_aligned_64(uci.sts);
}