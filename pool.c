/* pool: a voice's tasks on several cores (pool.h).
 *
 * One job at a time: the tasks of one run of one voice. The caller opens
 * it, works on it like any worker, and closes it when every task is done.
 * A task is ready when the tasks it needs have finished; whoever finds a
 * ready task first takes it. Every task writes only its own buffers and
 * state, and reads only what the tasks it waited for wrote, so the bits
 * are the same as one core running them in order.
 *
 * `gen` says whether a job is open: even while one is, odd between.
 * A worker counts itself `inside` before it touches the job and checks
 * that the job it saw is still the one open; the caller closes the job,
 * then waits for `inside` to reach 0 before the next can be set up.
 *
 * Between jobs a worker spins a little (the runs of a block come back to
 * back), then sleeps on a futex until the next job opens. */
#define _GNU_SOURCE
#include "pool.h"
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

int aither_realtime_worker(int k);   /* realtime.c: the k-th worker's core, the audio thread's priority */
int aither_cores_online(void);       /* realtime.c */

typedef void (*TaskFn)(void* s, long n, long S, double* B);

static struct {
  void* s;
  long nt;
  void* const* fns;
  const int* feeds_at;
  const int* feeds;
  long n, S;
  double* B;
  _Atomic int* left;     /* each task: how many it still waits for */
  _Atomic int* queue;    /* the ready tasks, in the order they became ready; -1: not yet written */
  _Atomic int head, tail;
  long cap;
  _Atomic int done;
} job;

static _Atomic int gen = 1;
static _Atomic int inside;
static _Atomic int sleepers;
static _Atomic int busy;
static int threads = 1;
static _Thread_local int claimed;

/* How long a worker spins for the next job before it sleeps. A block's
 * runs are 2.7 ms of sound apart at most; between blocks the mix thread
 * waits on the pipe. */
enum { SPIN_NS = 200000 };

static inline void relax(void) {
#if defined(__aarch64__)
  __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

static double now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}

static void futex_wait(_Atomic int* addr, int val) {
  syscall(SYS_futex, (int*)addr, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
}

static void futex_wake(_Atomic int* addr) {
  syscall(SYS_futex, (int*)addr, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}

static void push(int t) {
  int i = atomic_fetch_add(&job.tail, 1);
  atomic_store(&job.queue[i], t);
}

/* Take ready tasks until every task is done. A task joins the queue
 * when the last task it waits for ends: each is taken once, by whoever
 * reaches it first. */
static void work(void) {
  long nt = job.nt;
  while (atomic_load(&job.done) < nt) {
    int h = atomic_load(&job.head);
    if (h >= atomic_load(&job.tail)) { relax(); continue; }
    if (!atomic_compare_exchange_weak(&job.head, &h, h + 1)) continue;
    int t;
    while ((t = atomic_load(&job.queue[h])) < 0) relax();
    ((TaskFn)job.fns[t])(job.s, job.n, job.S, job.B);
    for (int k = job.feeds_at[t]; k < job.feeds_at[t + 1]; k++)
      if (atomic_fetch_sub(&job.left[job.feeds[k]], 1) == 1) push(job.feeds[k]);
    atomic_fetch_add(&job.done, 1);
  }
}

static void* worker(void* arg) {
  aither_realtime_worker((int)(long)arg);
  int last = -1;
  for (;;) {
    int g = atomic_load(&gen);
    if ((g & 1) || g == last) {
      double until = now_ns() + SPIN_NS;
      while (atomic_load(&gen) == g && now_ns() < until)
        for (int i = 0; i < 64; i++) relax();
      if (atomic_load(&gen) == g) {
        atomic_fetch_add(&sleepers, 1);
        if (atomic_load(&gen) == g) futex_wait(&gen, g);
        atomic_fetch_sub(&sleepers, 1);
      }
      continue;
    }
    atomic_fetch_add(&inside, 1);
    if (atomic_load(&gen) == g) work();
    last = g;
    atomic_fetch_sub(&inside, 1);
  }
  return NULL;
}

int pool_start(void) {
  static int started;
  if (started) return threads;
  started = 1;
  const char* e = getenv("AITHER_THREADS");
  int want = e && *e ? atoi(e) : aither_cores_online();
  if (want < 1) want = 1;
  if (want > 64) want = 64;
  threads = 1;
  for (int k = 1; k < want; k++) {
    pthread_t th;
    if (pthread_create(&th, NULL, worker, (void*)(long)(k - 1)) != 0) break;
    pthread_detach(th);
    threads++;
  }
  return threads;
}

int pool_threads(void) { return threads; }

void pool_claim(void) { claimed = 1; }

int pool_run(void* s, long nt, void* const* fns, const int* waits, const int* feeds_at, const int* feeds,
             long n, long S, double* B) {
  if (!claimed || threads < 2) return 0;
  int free_ = 0;
  if (!atomic_compare_exchange_strong(&busy, &free_, 1)) return 0;
  if (nt > job.cap) {
    /* nobody is inside between jobs: safe to grow */
    long cap = nt * 2;
    _Atomic int* left = realloc((void*)job.left, cap * sizeof *left);
    _Atomic int* queue = realloc((void*)job.queue, cap * sizeof *queue);
    if (!left || !queue) {
      if (left) job.left = left;
      if (queue) job.queue = queue;
      atomic_store(&busy, 0);
      return 0;
    }
    job.left = left;
    job.queue = queue;
    job.cap = cap;
  }
  job.s = s; job.nt = nt; job.fns = fns; job.feeds_at = feeds_at; job.feeds = feeds;
  job.n = n; job.S = S; job.B = B;
  atomic_store(&job.head, 0);
  atomic_store(&job.tail, 0);
  for (long t = 0; t < nt; t++) {
    atomic_store_explicit(&job.left[t], waits[t], memory_order_relaxed);
    atomic_store_explicit(&job.queue[t], -1, memory_order_relaxed);
  }
  for (long t = 0; t < nt; t++) if (waits[t] == 0) push((int)t);
  atomic_store(&job.done, 0);
  atomic_fetch_add(&gen, 1);                       /* open */
  if (atomic_load(&sleepers)) futex_wake(&gen);
  work();
  atomic_fetch_add(&gen, 1);                       /* closed */
  while (atomic_load(&inside)) relax();
  atomic_store(&busy, 0);
  return 1;
}
