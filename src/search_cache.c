#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
    #include <intrin.h>
    #include <xmmintrin.h>
#endif

#include "search_cache.h"
#include "large_pages.h"

#define SEARCH_CACHE_CLUSTER_SIZE (3)

struct SearchCacheEntry {
    ShortKey sk;
    u8 depth8;          
    u8 gen_bound8;      
    Move mv;
    ShortScore ssc;
    ShortScore eval_ssc;
};

typedef struct SearchCacheCluster {
    SearchCacheEntry entries[SEARCH_CACHE_CLUSTER_SIZE];
    u16 padding;
} SearchCacheCluster;

struct SearchCache {
    SearchCacheCluster *clusters;
    u64 count;
    u8 generation8;
    bool large_pages;
};

static_assert(sizeof(SearchCacheEntry) == 10, "SearchCacheEntry must be 10 bytes");
static_assert(sizeof(SearchCacheCluster) == 32, "SearchCacheCluster must be 32 bytes, two per cache line");

#define GENERATION_BITS (3)
#define GENERATION_DELTA (1 << GENERATION_BITS)
#define GENERATION_CYCLE (255 + GENERATION_DELTA)
#define GENERATION_MASK ((0xFF << GENERATION_BITS) & 0xFF)

static inline bool entry_is_occupied(const SearchCacheEntry *e) {
    return e->depth8 != 0;
}

static inline u8 entry_relative_age(const SearchCacheEntry *e, u8 generation8) {
    return (u8)((GENERATION_CYCLE + generation8 - e->gen_bound8) & GENERATION_MASK);
}

static inline SearchCacheData entry_read(const SearchCacheEntry *e) {
    return (SearchCacheData){
        .mv = e->mv,
        .sc = e->ssc,
        .eval_sc = e->eval_ssc,
        .d = (Depth)(e->depth8 + DEPTH_ENTRY_OFFSET),
        .b = (Bound)(e->gen_bound8 & 0x3),
        .is_pv = (e->gen_bound8 & 0x4) != 0,
    };
}

void search_cache_write(SearchCacheWriter writer, Key k, Score sc, bool is_pv, Bound b, Depth d, Move mv,
                        Score eval_sc, u8 generation8) {
    SearchCacheEntry *e = writer.entry;
    ShortKey sk = (ShortKey)k;

    if (mv != MOVE_NONE || sk != e->sk)
        e->mv = mv;

    if (b == BOUND_EXACT
     || sk != e->sk
     || d - DEPTH_ENTRY_OFFSET + 2 * is_pv > e->depth8 - 4
     || entry_relative_age(e, generation8)) {
        e->sk = sk;
        e->depth8 = (u8)(d - DEPTH_ENTRY_OFFSET);
        e->gen_bound8 = (u8)(generation8 | ((u8)is_pv << 2) | b);
        e->ssc = (ShortScore)sc;
        e->eval_ssc = (ShortScore)eval_sc;
    }
}

SearchCache *search_cache_create(u64 size_mb) {
    SearchCache *cache = calloc(1, sizeof(SearchCache));
    if (!cache)
        return NULL;

    if (!search_cache_resize(cache, size_mb)) {
        free(cache);
        return NULL;
    }

    return cache;
}

void search_cache_destroy(SearchCache *cache) {
    if (!cache)
        return;
    large_pages_free(cache->clusters);
    free(cache);
}

bool search_cache_resize(SearchCache *cache, u64 size_mb) {
    large_pages_free(cache->clusters);
    cache->clusters = NULL;
    cache->large_pages = false;

    cache->count = size_mb * 1024 * 1024 / sizeof(SearchCacheCluster);
    if (cache->count)
        cache->clusters = large_pages_alloc(cache->count * sizeof(SearchCacheCluster), &cache->large_pages);

    if (!cache->clusters) {
        cache->count = 0;
        return false;
    }

    search_cache_clear(cache);
    return true;
}

void search_cache_clear(SearchCache *cache) {
    memset(cache->clusters, 0, cache->count * sizeof(SearchCacheCluster));
    cache->generation8 = 0;
}

void search_cache_new_search(SearchCache *cache) {
    cache->generation8 += GENERATION_DELTA;
}

u8 search_cache_generation(const SearchCache *cache) {
    return cache->generation8;
}

bool search_cache_large_pages(const SearchCache *cache) {
    return cache->large_pages;
}

static inline u64 mul_hi64(u64 a, u64 b) {
#if defined(__SIZEOF_INT128__)
    __extension__ typedef unsigned __int128 u128;
    return (u64)(((u128)a * b) >> 64);
#elif defined(_MSC_VER)
    return __umulh(a, b);
#else
    u64 a_lo = (u32)a, a_hi = a >> 32, b_lo = (u32)b, b_hi = b >> 32;
    u64 mid = a_hi * b_lo + ((a_lo * b_lo) >> 32);
    u64 mid2 = a_lo * b_hi + (u32)mid;
    return a_hi * b_hi + (mid >> 32) + (mid2 >> 32);
#endif
}

static inline SearchCacheEntry *first_entry(const SearchCache *cache, Key k) {
    return cache->clusters[mul_hi64(k, cache->count)].entries;
}

void search_cache_prefetch(const SearchCache *cache, Key k) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(first_entry(cache, k));
#elif defined(_MSC_VER)
    _mm_prefetch((const char *)first_entry(cache, k), _MM_HINT_T0);
#else
    (void)cache;
    (void)k;
#endif
}

bool search_cache_probe(const SearchCache *cache, Key k, SearchCacheData *data, SearchCacheWriter *writer) {
    SearchCacheEntry *entries = first_entry(cache, k);
    ShortKey sk = (ShortKey)k;

    for (u8 i = 0; i < SEARCH_CACHE_CLUSTER_SIZE; i++) {
        if (entries[i].sk == sk) {
            *data = entry_read(&entries[i]);
            writer->entry = &entries[i];
            return entry_is_occupied(&entries[i]);
        }
    }

    SearchCacheEntry *replace = &entries[0];
    for (u8 i = 1; i < SEARCH_CACHE_CLUSTER_SIZE; i++)
        if (replace->depth8 - entry_relative_age(replace, cache->generation8) * 2
            > entries[i].depth8 - entry_relative_age(&entries[i], cache->generation8) * 2)
            replace = &entries[i];

    *data = (SearchCacheData){
        .mv = MOVE_NONE,
        .sc = SCORE_NONE,
        .eval_sc = SCORE_NONE,
        .d = DEPTH_ENTRY_OFFSET,
        .b = BOUND_NONE,
        .is_pv = false,
    };
    writer->entry = replace;
    return false;
}

u16 search_cache_hashfull(const SearchCache *cache, u8 max_age) {
    u16 max_age_internal = (u16)(max_age << GENERATION_BITS);
    u32 used = 0;
    u64 sample = cache->count < 1000 ? cache->count : 1000;

    for (u64 i = 0; i < sample; i++)
        for (u8 j = 0; j < SEARCH_CACHE_CLUSTER_SIZE; j++) {
            const SearchCacheEntry *e = &cache->clusters[i].entries[j];
            used += entry_is_occupied(e) && entry_relative_age(e, cache->generation8) <= max_age_internal;
        }

    return sample ? (u16)(used * 1000 / (sample * SEARCH_CACHE_CLUSTER_SIZE)) : 0;
}