/* A voice: one patch's compiled code, its state, and its parts.
 *
 * Its code is the C the compiler wrote, compiled in memory by TCC (and
 * later swapped for a cc -O2 build of the same C). Its state is one
 * buffer: the generated VoiceState struct, then the pool its patch
 * needs. A hot reload is two steps: prepare (compile and allocate, slow,
 * outside the audio mutex) and commit (migrate the old state into the
 * new and swap, microseconds, under it). The version before a commit is
 * kept, so a reload that blows up can be rolled back. */

#ifndef AITHER_VOICE_H
#define AITHER_VOICE_H

#include "codegen.h"

typedef void (*TickFn)(void* state, double* outL, double* outR);
typedef void (*TickBlockFn)(void* state, long n, double* outL, double* outR);

/* The fixed head of every generated VoiceState. codegen.c writes the
 * same fields in the same order; change both together. */
typedef struct {
  double* pool;
  long    pool_slots;
  long    idx;
  char    overflow;
  double  sr;
  double  t, dt, sweep_origin;
  double* part_gains;
  double* part_out;
  double* part_park;
  double* part_fade_delta;
  double* part_fade_target;
  unsigned char* part_park_pending;
  double* part_peak;
  double* part_sq;
  double* in_buf;
  double  in_l, in_r;
  double  fade_gain, fade_delta;
  double  time_sec, time_frac;
  double  peak_decay, rms_alpha;
  double  peak_raw;
  long    bad;
  long    n_parts;
  double  deadline;           /* monotonic seconds, 0: none (a recursion's guard) */
  long    stuck;
  const char* stuck_at;
  double* prof;               /* `aither profile`: seconds per slot */
  long    profiling;
  long    prof_ticks;
} VoiceHeader;

/* A region of the pool, owned by the voice (its strings malloc'd). */
typedef struct {
  char* type_name;
  int   per_type_idx;
  char* scope;
  int   scope_idx;
  char* owner;
  long  offset, size;
} VRegion;

/* What a version of a voice is: its code, state and layout. */
typedef struct {
  void*       lib;            /* TCCState* */
  TickFn      tick;
  TickBlockFn tick_block;
  void*       state;
  long        state_size;
  char**      var_names;   int nvars;
  char**      part_names;  int nparts;
  char**      unheard;     int nunheard;
  VRegion*    regions;     int nregions;
  char*       csrc;
  int         optimized;
} Version;

typedef struct Voice {
  Version     cur;
  Version     prev;           /* the version before the last commit; prev.state NULL: none */
  void**      old_libs;  int nold, capold;   /* kept alive: an audio thread may still run them */
  double      sweep_origin;
  long        gen;            /* bumped on every commit, so a stale -O2 build is refused */
  /* Parts, by index into cur.part_names. */
  double*        part_gains;
  double*        part_fade_deltas;
  double*        part_fade_targets;
  double*        part_out;         /* L, R per part, written by tick */
  double*        part_peak;
  double*        part_rms_sq;
  double*        part_parked;      /* 1.0: parked, its body skipped */
  unsigned char* part_park_pending;
  double*        prof;             /* MAX_PROF_SLOTS */
} Voice;

/* A compile, ready to commit. */
typedef struct {
  Version v;
  int     calls_input;
} Prepared;

enum { VOICE_OK, VOICE_PARSE, VOICE_IMPORT, VOICE_COMPILE, VOICE_TCC };

#define VOICE_MAX_PROF_SLOTS 1024

Voice*    voice_new(void);
/* Compile a patch's source (with the stdlib) and allocate its state.
 * NULL on failure, with the kind in *kind and the message in err. */
Prepared* voice_prepare(const char* stdlib_src, const char* src, const char* tag,
                        const char* patch_path, double sr, int seed_self, int hoist_on,
                        int* kind, char* err, int errlen);
void      voice_prepared_free(Prepared* p);   /* one never committed */
void      voice_commit(Voice* v, Prepared* p); /* takes p (frees the shell) */
int       voice_rollback(Voice* v);
/* The same C at -O2 (cc), linked in memory; 0 on success. `background`:
 * at low priority, beside a playing engine. */
int       voice_compile_optimized(const char* csrc, long state_size, int background,
                                  void** lib, TickFn* tick, char* err, int errlen);
int       voice_upgrade(Voice* v, long gen, void* lib, TickFn tick);
void      voice_snapshot_into(Voice* src, Voice* dst, void* buf);
void      voice_dispose(Voice* v);
void      voice_reset_pool(Voice* v);
void      voice_scrub_state(Voice* v, int hard);
void      voice_tick(Voice* v, double t, double* l, double* r);
void      voice_tick_block(Voice* v, long n, double* outL, double* outR);
int       voice_prof_names(Voice* v, const char*** names);
double    voice_clock_read_cost(void);
double    voice_mono_now(void);

#endif
