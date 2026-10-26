#ifndef HONEY_BADGER_UCI_H
#define HONEY_BADGER_UCI_H

#include "types.h"

#define ENGINE_NAME "Honey Badger"
#define ENGINE_AUTHOR "Novac C. Andrei-Gabriel"

void uci_loop(void);

void uci_bench(Depth depth);

#define BENCH_DEFAULT_DEPTH (13)

#endif