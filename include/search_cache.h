#ifndef HONEY_BADGER_SEARCH_CACHE_H
#define HONEY_BADGER_SEARCH_CACHE_H

#include "types.h"

typedef enum Bound : u8 {
    BOUND_NONE,
    BOUND_UPPER,                                
    BOUND_LOWER,                                
    BOUND_EXACT = BOUND_UPPER | BOUND_LOWER
} Bound;

typedef struct SearchCache SearchCache;
typedef struct SearchCacheEntry SearchCacheEntry;

typedef struct SearchCacheData {
    Move mv;
    Score sc;           
    Score eval_sc;
    Depth d;
    Bound b;
    bool is_pv;
} SearchCacheData;

typedef struct SearchCacheWriter {
    SearchCacheEntry *entry;
} SearchCacheWriter;

SearchCache *search_cache_create(u64 size_mb);
void search_cache_destroy(SearchCache *cache);

bool search_cache_resize(SearchCache *cache, u64 size_mb);

void search_cache_clear(SearchCache *cache);

void search_cache_new_search(SearchCache *cache);
u8 search_cache_generation(const SearchCache *cache);

bool search_cache_probe(const SearchCache *cache, Key k, SearchCacheData *data, SearchCacheWriter *writer);

void search_cache_write(SearchCacheWriter writer, Key k, Score sc, bool is_pv, Bound b, Depth d, Move mv,
                        Score eval_sc, u8 generation8);

void search_cache_prefetch(const SearchCache *cache, Key k);

u16 search_cache_hashfull(const SearchCache *cache, u8 max_age);

bool search_cache_large_pages(const SearchCache *cache);

static inline Score score_to_search_cache(Score sc, u8 ply) {
    if (sc >= SCORE_MATE_IN_MAX_PLY)
        return sc + ply;
    if (sc <= SCORE_MATED_IN_MAX_PLY)
        return sc - ply;
    return sc;
}

static inline Score score_from_search_cache(Score sc, u8 ply, u8 halfmv_clock) {
    if (sc == SCORE_NONE)
        return SCORE_NONE;

    if (sc >= SCORE_MATE_IN_MAX_PLY) {
        if (SCORE_MATE - sc > 100 - halfmv_clock)
            return SCORE_MATE_IN_MAX_PLY - 1;
        return sc - ply;
    }

    if (sc <= SCORE_MATED_IN_MAX_PLY) {
        if (SCORE_MATE + sc > 100 - halfmv_clock)
            return SCORE_MATED_IN_MAX_PLY + 1;
        return sc + ply;
    }

    return sc;
}

#endif