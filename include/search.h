#ifndef HONEY_BADGER_SEARCH_H
#define HONEY_BADGER_SEARCH_H

#include "types.h"
#include "position.h"

typedef struct SearchLimits {
    i64 time_ms[NO_COLOR];      
    i64 inc_ms[NO_COLOR];       
    u16 moves_to_go;           
    i64 move_time_ms;           
    Depth depth;
    u64 nodes;
    bool infinite;              
    bool ponder;                
} SearchLimits;

typedef struct SearchResult {
    Move best_mv;               
    Move ponder_mv;             
    Score sc;
    Depth depth;                
    u64 nodes;                  
    i64 time_ms;
} SearchResult;

typedef struct SearchPool SearchPool;

SearchPool *search_pool_create(u16 threads, u64 cache_mb);
void search_pool_destroy(SearchPool *pool);

bool search_pool_set_threads(SearchPool *pool, u16 threads);
bool search_pool_set_cache_size(SearchPool *pool, u64 cache_mb);

void search_pool_clear(SearchPool *pool);

void search_pool_set_output(SearchPool *pool, bool enabled);

void search_pool_start(SearchPool *pool, const Position *pos, const SearchLimits *limits);

void search_pool_stop(SearchPool *pool);

void search_pool_ponderhit(SearchPool *pool);

void search_pool_set_move_overhead(SearchPool *pool, i64 ms);  
void search_pool_set_multi_pv(SearchPool *pool, u16 lines);    
void search_pool_set_ponder(SearchPool *pool, bool enabled);   

SearchResult search_pool_wait(SearchPool *pool);

#endif