/* pool: the cores a voice's tasks run on (notes/multicore-2026-10-18.md).
 *
 * pool_start makes the workers, once; a thread that calls pool_claim
 * (the engine's mix thread, a test) may then run a voice's tasks on them
 * through pool_run, which the generated tick_block calls through the
 * header's `par`. Any other thread, or a second one while the pool is
 * busy, gets 0 back and runs the tasks itself, in order. */
#ifndef AITHER_POOL_H
#define AITHER_POOL_H

/* Start the workers: AITHER_THREADS threads in all, counting the caller
 * (default: every core online). Returns how many; 1 makes none. */
int pool_start(void);

/* How many threads the pool runs tasks on (1 before pool_start). */
int pool_threads(void);

/* The calling thread may use the pool. */
void pool_claim(void);

/* Run nt tasks over a run of n samples. Task t waits for waits[t] others
 * to finish, and when it ends, the tasks feeds[feeds_at[t] ..
 * feeds_at[t + 1]) each wait for one fewer. Returns 0, having run
 * nothing, when the caller hasn't claimed the pool or it is busy. */
int pool_run(void* s, long nt, void* const* fns, const int* waits, const int* feeds_at, const int* feeds,
             long n, long S, double* B);

#endif
