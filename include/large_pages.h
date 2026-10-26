#ifndef HONEY_BADGER_LARGE_PAGES_H
#define HONEY_BADGER_LARGE_PAGES_H

#include "types.h"

void *large_pages_alloc(u64 size, bool *large_pages);
void large_pages_free(void *mem);

#endif