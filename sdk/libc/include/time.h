#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>

#define CLOCKS_PER_SEC 1000

typedef long clock_t;

time_t time (time_t *t);
clock_t clock (void);

#endif
