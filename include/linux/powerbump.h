#pragma once

/* in nsecs */
#define BUMP_LATENCY_THRESHOLD 2000


/* bump time constants, in msec */
#define BUMP_FOR_DISK		3
#define BUMP_FOR_NETWORK	3
#define BUMP_FOR_FUTEX		3



/* API prototypes */
extern void give_power_bump(int msecs);
extern int in_power_bump(void);
