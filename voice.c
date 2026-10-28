/* A voice (voice.h): compile with TCC, allocate, tick, and carry state
 * across a hot reload. */

#define _GNU_SOURCE
#include <libtcc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "voice.h"
#include "imports.h"
#include "dsp.h"

/* What MIDI holds, exported by midi.nim until it is C too. */
extern double n_midi_cc(int);
extern double n_midi_note(int);
extern double n_midi_freq(void);
extern double n_midi_gate(void);
extern double n_midi_vel(void);
extern double n_midi_trig(void*, int);
extern double n_midi_voice_freq(int);
extern double n_midi_voice_gate(int);
extern double n_midi_voice_vel(int);
extern double n_midi_voice_key(int);

/* --------------------------------------------------------------- the clock */

double voice_mono_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double n_now(void) { return voice_mono_now(); }   /* `aither profile`'s clock */

/* A recursion asks every 65 536 steps whether its block is past the
 * deadline. Past it, the recursion gives up, and so does every later one
 * in the block: they see `stuck` before they start. */
static long n_overrun(void* state, const char* where) {
  VoiceHeader* h = state;
  if (h->deadline <= 0.0 || voice_mono_now() < h->deadline) return 0;
  h->stuck = 1;
  h->stuck_at = where;
  return 1;
}

double voice_clock_read_cost(void) {
  /* What one of tick1's timing points costs: `profile` takes it off each
   * slot per sample timed. */
  enum { N = 20000 };
  double (*volatile f)(void) = n_now;
  double sink = 0.0, t0 = voice_mono_now();
  for (int i = 0; i < N; i++) sink += f();
  double r = (voice_mono_now() - t0) / N;
  return sink < 0 ? 0 : r;
}

/* --------------------------------------------------------------------- TCC */

/* libtcc keeps global state, so one thread at a time: the server loading
 * a patch, or the -O2 worker linking one. */
static pthread_mutex_t tcc_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread char tcc_error[1024];

static void tcc_err(void* opaque, const char* msg) {
  (void)opaque;
  snprintf(tcc_error, sizeof tcc_error, "%s", msg);
  fprintf(stderr, "TCC: %s\n", msg);
}

static void register_natives(TCCState* s) {
  char name[96];
  for (const NativeSpec* n = dsp_natives; n->name; n++) {
    snprintf(name, sizeof name, "n_%s", n->name);
    tcc_add_symbol(s, name, n->fn);
  }
  static const struct { const char* name; const void* fn; } extra[] = {
    {"n_overrun", (const void*)n_overrun}, {"n_now", (const void*)n_now},
    {"n_lp1_coef", (const void*)n_lp1_coef}, {"n_lp1_c", (const void*)n_lp1_c},
    {"n_hp1_c", (const void*)n_hp1_c}, {"n_svf_coef", (const void*)n_svf_coef},
    {"n_lpf_c", (const void*)n_lpf_c}, {"n_hpf_c", (const void*)n_hpf_c},
    {"n_bpf_c", (const void*)n_bpf_c}, {"n_notch_c", (const void*)n_notch_c},
    {"n_reverb_coef", (const void*)n_reverb_coef}, {"n_reverb_c", (const void*)n_reverb_c},
    {"n_hilbert_a", (const void*)n_hilbert_a}, {"n_hilbert_b", (const void*)n_hilbert_b},
    {"n_input_l", (const void*)n_input_l}, {"n_input_r", (const void*)n_input_r},
    {"shape_saw", (const void*)shape_saw}, {"shape_tri", (const void*)shape_tri},
    {"shape_sqr", (const void*)shape_sqr},
    {"n_midi_cc", (const void*)n_midi_cc}, {"n_midi_note", (const void*)n_midi_note},
    {"n_midi_freq", (const void*)n_midi_freq}, {"n_midi_gate", (const void*)n_midi_gate},
    {"n_midi_trig", (const void*)n_midi_trig}, {"n_midi_vel", (const void*)n_midi_vel},
    {"n_midi_voice_freq", (const void*)n_midi_voice_freq},
    {"n_midi_voice_gate", (const void*)n_midi_voice_gate},
    {"n_midi_voice_vel", (const void*)n_midi_voice_vel},
    {"n_midi_voice_key", (const void*)n_midi_voice_key},
    {NULL, NULL}};
  for (int i = 0; extra[i].name; i++) tcc_add_symbol(s, extra[i].name, extra[i].fn);
}

/* The whole allocation: the struct, then the pool its patch needs. */
static const char* SIZE_HELPER =
  "\nint voice_state_size(void) { return sizeof(VoiceState) + voice_pool_slots() * sizeof(double); }\n";

static TCCState* new_tcc(char* err, int errlen) {
  TCCState* s = tcc_new();
  if (!s) { snprintf(err, errlen, "tcc_new failed"); return NULL; }
  tcc_set_error_func(s, NULL, tcc_err);
  if (tcc_set_output_type(s, TCC_OUTPUT_MEMORY) != 0 || tcc_add_library(s, "m") != 0) {
    tcc_delete(s);
    snprintf(err, errlen, "tcc setup failed");
    return NULL;
  }
  register_natives(s);
  return s;
}

/* ----------------------------------------------------------------- strings */

static char* sdup(const char* s) { return strdup(s ? s : ""); }

static char** sdup_list(const char** v, int n) {
  char** r = calloc(n ? n : 1, sizeof(char*));
  for (int i = 0; i < n; i++) r[i] = sdup(v[i]);
  return r;
}

static void free_list(char** v, int n) {
  if (!v) return;
  for (int i = 0; i < n; i++) free(v[i]);
  free(v);
}

static void free_regions(VRegion* r, int n) {
  if (!r) return;
  for (int i = 0; i < n; i++) { free(r[i].type_name); free(r[i].scope); free(r[i].owner); }
  free(r);
}

/* A version's state and layout (not its code: libs live on in old_libs). */
static void free_version(Version* v) {
  free(v->state);
  free_list(v->var_names, v->nvars);
  free_list(v->part_names, v->nparts);
  free_list(v->unheard, v->nunheard);
  free_regions(v->regions, v->nregions);
  free(v->csrc);
  memset(v, 0, sizeof *v);
}

/* ------------------------------------------------------------------ state */

static void place_pool(void* state, long state_size, long slots) {
  VoiceHeader* h = state;
  h->pool_slots = slots;
  h->pool = (double*)((char*)state + state_size - slots * (long)sizeof(double));
}

/* File-level `$vars` sit after the header, then one `inited` byte each. */
static double* var_addr(void* state, int i) {
  return (double*)((char*)state + sizeof(VoiceHeader) + (size_t)i * sizeof(double));
}
static unsigned char* inited_addr(void* state, int nvars, int i) {
  return (unsigned char*)state + sizeof(VoiceHeader) + (size_t)nvars * sizeof(double) + i;
}

Voice* voice_new(void) {
  Voice* v = calloc(1, sizeof(Voice));
  v->prof = calloc(VOICE_MAX_PROF_SLOTS, sizeof(double));
  return v;
}

Prepared* voice_prepare(const char* stdlib_src, const char* src, const char* tag,
                        const char* patch_path, double sr, int seed_self, int hoist_on,
                        int* kind, char* err, int errlen) {
  Arena* a = arena_new();
  int bkind;
  Node* prog = build_program(a, stdlib_src, src, tag, seed_self, &bkind, err, errlen);
  if (!prog) {
    arena_free(a);
    *kind = bkind == BUILD_IMPORT ? VOICE_IMPORT : VOICE_PARSE;
    return NULL;
  }
  Prepared* p = calloc(1, sizeof(Prepared));
  p->calls_input = calls_input(prog);
  Generated g;
  if (generate_full(a, prog, patch_path, sr, hoist_on, &g, err, errlen) != 0) {
    arena_free(a);
    free(p);
    *kind = VOICE_COMPILE;
    return NULL;
  }
  Version* v = &p->v;
  v->var_names = sdup_list(g.var_names, g.nvars);   v->nvars = g.nvars;
  v->part_names = sdup_list(g.part_names, g.nparts); v->nparts = g.nparts;
  v->unheard = sdup_list(g.unheard, g.nunheard);     v->nunheard = g.nunheard;
  v->regions = calloc(g.nregions ? g.nregions : 1, sizeof(VRegion));
  v->nregions = g.nregions;
  for (int i = 0; i < g.nregions; i++) {
    Region* r = &g.regions[i];
    v->regions[i] = (VRegion){sdup(r->type_name), r->per_type_idx, sdup(r->scope), r->scope_idx,
                              sdup(r->owner), r->offset, r->size};
  }
  size_t cl = strlen(g.csrc), hl = strlen(SIZE_HELPER);
  v->csrc = malloc(cl + hl + 1);
  memcpy(v->csrc, g.csrc, cl);
  memcpy(v->csrc + cl, SIZE_HELPER, hl + 1);
  arena_free(a);

  pthread_mutex_lock(&tcc_lock);
  tcc_error[0] = 0;
  TCCState* s = new_tcc(err, errlen);
  int ok = s && tcc_compile_string(s, v->csrc) >= 0 && tcc_relocate(s) >= 0;
  TickFn tick = ok ? (TickFn)tcc_get_symbol(s, "tick") : NULL;
  int (*size_fn)(void) = ok ? (int (*)(void))tcc_get_symbol(s, "voice_state_size") : NULL;
  int (*slots_fn)(void) = ok ? (int (*)(void))tcc_get_symbol(s, "voice_pool_slots") : NULL;
  pthread_mutex_unlock(&tcc_lock);
  if (!ok || !tick || !size_fn || !slots_fn) {
    if (s && (!ok || !tick)) snprintf(err, errlen, "%s", tcc_error[0] ? tcc_error : "TCC compile failed");
    if (s) tcc_delete(s);
    voice_prepared_free(p);
    *kind = VOICE_TCC;
    return NULL;
  }
  v->lib = s;
  v->tick = tick;
  v->tick_block = (TickBlockFn)tcc_get_symbol(s, "tick_block");
  v->state_size = size_fn();
  v->state = calloc(1, v->state_size);
  place_pool(v->state, v->state_size, slots_fn());
  ((VoiceHeader*)v->state)->sr = sr;
  ((VoiceHeader*)v->state)->dt = 1.0 / sr;
  *kind = VOICE_OK;
  return p;
}

void voice_prepared_free(Prepared* p) {
  if (!p) return;
  if (p->v.lib) tcc_delete(p->v.lib);
  free_version(&p->v);
  free(p);
}

/* -------------------------------------------------------------- poisoning */

/* Defense 1: a region is poisoned if any slot is NaN, ±Inf or past 1e6
 * (a blow-up on its way). Poison never migrates: fixing the patch must
 * help. */
static int poisoned(const double* pool, long off, long size) {
  for (long k = 0; k < size; k++) {
    double x = pool[off + k];
    if (x != x || x > 1e6 || x < -1e6) return 1;
  }
  return 0;
}

/* File-level `$vars` hold timers and counters that grow large, so only
 * NaN or Inf is poison there. */
static int bad_var(double x) { return x != x || x - x != 0.0; }

static int is_user_state(const VRegion* r) {
  return !strcmp(r->type_name, "var") || !strcmp(r->type_name, "pvar");
}

/* Defense 2: zero the stateful helpers' regions (filters, delays,
 * reverbs) after NaN/Inf, so the next tick reads clean state. User state
 * ($vars) is left: its counters and timers drive the arrangement. */
void voice_reset_pool(Voice* v) {
  if (!v->cur.state) return;
  double* pool = ((VoiceHeader*)v->cur.state)->pool;
  for (int i = 0; i < v->cur.nregions; i++) {
    VRegion* r = &v->cur.regions[i];
    if (is_user_state(r)) continue;
    memset(&pool[r->offset], 0, r->size * sizeof(double));
  }
}

/* Clear the poison resetPool leaves: a def's $state past 1e6 or NaN
 * (every one, `hard`), and a file-level $var that is NaN/Inf. No
 * allocation: called from the audio callback. */
void voice_scrub_state(Voice* v, int hard) {
  if (!v->cur.state) return;
  double* pool = ((VoiceHeader*)v->cur.state)->pool;
  for (int i = 0; i < v->cur.nregions; i++) {
    VRegion* r = &v->cur.regions[i];
    if (!is_user_state(r)) continue;
    if (hard || poisoned(pool, r->offset, r->size))
      memset(&pool[r->offset], 0, r->size * sizeof(double));
  }
  for (int i = 0; i < v->cur.nvars; i++)
    if (bad_var(*var_addr(v->cur.state, i))) {
      *var_addr(v->cur.state, i) = 0.0;
      *inited_addr(v->cur.state, v->cur.nvars, i) = 0;
    }
}

/* ------------------------------------------------------------------- parts */

/* Re-map the parts' gains, fades and meters onto a new list of names,
 * keeping each surviving part's. No tables: rollback runs it under the
 * audio mutex. */
static void set_parts(Voice* v, char** names, int n, char** old, int nold) {
  int m = n ? n : 1;
  double* gains = calloc(m, sizeof(double));
  double* targets = calloc(m, sizeof(double));
  double* deltas = calloc(m, sizeof(double));
  double* peaks = calloc(m, sizeof(double));
  double* rms = calloc(m, sizeof(double));
  double* parked = calloc(m, sizeof(double));
  unsigned char* pending = calloc(m, 1);
  for (int i = 0; i < n; i++) {
    gains[i] = 1.0;
    targets[i] = 1.0;
    for (int j = 0; j < nold; j++)
      if (!strcmp(old[j], names[i])) {
        gains[i] = v->part_gains[j];
        targets[i] = v->part_fade_targets[j];
        deltas[i] = v->part_fade_deltas[j];
        peaks[i] = v->part_peak[j];
        rms[i] = v->part_rms_sq[j];
        parked[i] = v->part_parked[j];
        pending[i] = v->part_park_pending[j];
        break;
      }
  }
  free(v->part_gains); free(v->part_fade_targets); free(v->part_fade_deltas);
  free(v->part_peak); free(v->part_rms_sq); free(v->part_parked); free(v->part_park_pending);
  free(v->part_out);
  v->part_gains = gains; v->part_fade_targets = targets; v->part_fade_deltas = deltas;
  v->part_peak = peaks; v->part_rms_sq = rms; v->part_parked = parked;
  v->part_park_pending = pending;
  v->part_out = calloc(2 * m, sizeof(double));
}

/* Point the state's header at the voice's part arrays. */
static void point_parts(Voice* v) {
  VoiceHeader* h = v->cur.state;
  int any = v->cur.nparts > 0;
  h->part_gains = any ? v->part_gains : NULL;
  h->part_out = any ? v->part_out : NULL;
  h->part_park = any ? v->part_parked : NULL;
  h->part_fade_delta = any ? v->part_fade_deltas : NULL;
  h->part_fade_target = any ? v->part_fade_targets : NULL;
  h->part_park_pending = any ? v->part_park_pending : NULL;
  h->part_peak = any ? v->part_peak : NULL;
  h->part_sq = any ? v->part_rms_sq : NULL;
  h->prof = v->prof;
  h->n_parts = v->cur.nparts;
  h->peak_decay = 0.99993;     /* ~300 ms (engine_types PeakDecay) */
  h->rms_alpha = 1.04e-4;      /* ~200 ms (engine_types RmsAlpha) */
}

/* --------------------------------------------------------------- migration */

/* A small open-addressing table from a region's key to its index. */
typedef struct { char** keys; int* vals; int cap; } KeyMap;

static unsigned long hash_str(const char* s) {
  unsigned long h = 1469598103934665603UL;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211UL;
  return h;
}

static void km_init(KeyMap* m, int n) {
  m->cap = 16;
  while (m->cap < 2 * n + 1) m->cap *= 2;
  m->keys = calloc(m->cap, sizeof(char*));
  m->vals = calloc(m->cap, sizeof(int));
}

static void km_put(KeyMap* m, char* key, int val) {
  unsigned long i = hash_str(key) & (m->cap - 1);
  while (m->keys[i] && strcmp(m->keys[i], key)) i = (i + 1) & (m->cap - 1);
  if (m->keys[i]) free(m->keys[i]);
  m->keys[i] = key;
  m->vals[i] = val;
}

static int km_get(KeyMap* m, const char* key) {
  unsigned long i = hash_str(key) & (m->cap - 1);
  while (m->keys[i]) {
    if (!strcmp(m->keys[i], key)) return m->vals[i];
    i = (i + 1) & (m->cap - 1);
  }
  return -1;
}

static void km_free(KeyMap* m) {
  for (int i = 0; i < m->cap; i++) free(m->keys[i]);
  free(m->keys);
  free(m->vals);
}

static char* region_key(const VRegion* r) {
  char* k = NULL;
  if (asprintf(&k, "%s\x1f%s\x1f%s\x1f%d", r->scope, r->type_name, r->owner, r->scope_idx) < 0) return NULL;
  return k;
}

/* Carry the old version's state into the new one: file-level $vars by
 * name, and each pool region to the region in the same statement with
 * the same type, owner and index there (so an edit inside one play or
 * let leaves every other statement's filters, delays and physics alone,
 * and a different def in the same place starts clean). A region of a
 * statement that didn't exist before falls back to the old (type,
 * owner, per-type index), but only against old regions whose statement
 * is gone, so it can't steal live state. Mismatched sizes and poisoned
 * old regions start zeroed. */
static void migrate(Version* old, Version* nw) {
  for (int i = 0; i < old->nvars; i++) {
    double x = *var_addr(old->state, i);
    if (bad_var(x)) continue;                                   /* NaN re-inits */
    if (*inited_addr(old->state, old->nvars, i) == 0) continue; /* reset by scrub */
    for (int j = 0; j < nw->nvars; j++)
      if (!strcmp(nw->var_names[j], old->var_names[i])) {
        *var_addr(nw->state, j) = x;
        *inited_addr(nw->state, nw->nvars, j) = 1;
        break;
      }
  }
  double* op = ((VoiceHeader*)old->state)->pool;
  double* np = ((VoiceHeader*)nw->state)->pool;
  KeyMap by_key, old_scopes, new_scopes;
  km_init(&by_key, old->nregions);
  km_init(&old_scopes, old->nregions);
  km_init(&new_scopes, nw->nregions);
  for (int i = 0; i < old->nregions; i++) {
    km_put(&by_key, region_key(&old->regions[i]), i);
    km_put(&old_scopes, sdup(old->regions[i].scope), 1);
  }
  for (int i = 0; i < nw->nregions; i++) km_put(&new_scopes, sdup(nw->regions[i].scope), 1);
  char* claimed = calloc(old->nregions ? old->nregions : 1, 1);
  int* pending = calloc(nw->nregions ? nw->nregions : 1, sizeof(int));
  int npend = 0;
  for (int ni = 0; ni < nw->nregions; ni++) {
    VRegion* r = &nw->regions[ni];
    char* k = region_key(r);
    int oi = km_get(&by_key, k);
    free(k);
    if (oi >= 0) {
      claimed[oi] = 1;
      VRegion* o = &old->regions[oi];
      if (o->size == r->size && !poisoned(op, o->offset, o->size))
        memcpy(&np[r->offset], &op[o->offset], r->size * sizeof(double));
    } else if (km_get(&old_scopes, r->scope) < 0) {
      pending[npend++] = ni;
    }
  }
  for (int pi = 0; pi < npend; pi++) {
    VRegion* r = &nw->regions[pending[pi]];
    for (int oi = 0; oi < old->nregions; oi++) {
      VRegion* o = &old->regions[oi];
      if (!claimed[oi] && km_get(&new_scopes, o->scope) < 0 && !strcmp(o->type_name, r->type_name) &&
          !strcmp(o->owner, r->owner) && o->per_type_idx == r->per_type_idx && o->size == r->size) {
        claimed[oi] = 1;
        if (!poisoned(op, o->offset, o->size))
          memcpy(&np[r->offset], &op[o->offset], r->size * sizeof(double));
        break;
      }
    }
  }
  free(claimed);
  free(pending);
  km_free(&by_key);
  km_free(&old_scopes);
  km_free(&new_scopes);
}

/* ------------------------------------------------------- commit, rollback */

static void keep_lib(Voice* v, void* lib) {
  if (!lib) return;
  if (v->nold == v->capold) {
    v->capold = v->capold ? v->capold * 2 : 8;
    v->old_libs = realloc(v->old_libs, v->capold * sizeof(void*));
  }
  v->old_libs[v->nold++] = lib;
}

/* Apply a prepared compile. Under the audio mutex: it touches what the
 * audio thread reads. A handful of copies and pointer stores. */
void voice_commit(Voice* v, Prepared* p) {
  Version* nw = &p->v;
  if (v->cur.state) migrate(&v->cur, nw);
  /* The parts, by name; then the swap. */
  set_parts(v, nw->part_names, nw->nparts, v->cur.part_names, v->cur.nparts);
  if (v->cur.state) {
    keep_lib(v, v->prev.lib);       /* code stays alive: a small leak per reload */
    v->prev.lib = NULL;
    free_version(&v->prev);
    v->prev = v->cur;               /* a snapshot of the moment of the swap, owning its code */
  }
  v->cur = *nw;
  nw->state = NULL;
  free(p);
  point_parts(v);
  v->gen++;
  v->cur.optimized = 0;
}

int voice_rollback(Voice* v) {
  /* Put the previous version back: its code, and its state as it was at
   * the swap (scrubbed in case it was already sick). Bumps gen, so a
   * pending -O2 build of the broken code is dropped. */
  if (!v->prev.state) return 0;
  Version broken = v->cur;
  set_parts(v, v->prev.part_names, v->prev.nparts, broken.part_names, broken.nparts);
  keep_lib(v, broken.lib);
  broken.lib = NULL;
  free_version(&broken);
  v->cur = v->prev;
  memset(&v->prev, 0, sizeof v->prev);
  point_parts(v);
  v->gen++;
  voice_scrub_state(v, 0);
  return 1;
}

int voice_upgrade(Voice* v, long gen, void* lib, TickFn tick) {
  /* An -O2 tick() built from generation `gen`'s C: same layout, so only
   * the function pointer changes. Refused if the voice was reloaded. */
  if (v->gen != gen || !v->cur.state) return 0;
  keep_lib(v, v->cur.lib);
  v->cur.lib = lib;
  v->cur.tick = tick;
  v->cur.tick_block = (TickBlockFn)tcc_get_symbol(lib, "tick_block");
  v->cur.optimized = 1;
  return 1;
}

/* ------------------------------------------------------------- -O2 builds */

int voice_compile_optimized(const char* csrc, long state_size, int background,
                            void** lib_out, TickFn* tick_out, char* err, int errlen) {
  char dir[256], base[320], cfile[340], ofile[340], cmd[2048];
  snprintf(dir, sizeof dir, "%s/aither-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
  mkdir(dir, 0700);
  snprintf(base, sizeof base, "%s/opt-%lx", dir, (unsigned long)pthread_self());
  snprintf(cfile, sizeof cfile, "%s.c", base);
  snprintf(ofile, sizeof ofile, "%s.o", base);
  FILE* f = fopen(cfile, "w");
  if (!f) { snprintf(err, errlen, "can't write %s", cfile); return -1; }
  fputs(csrc, f);
  fclose(f);
  /* A live engine builds beside playing voices at nice 19, so the audio
   * wins any core both want. -ffp-contract=off: no fused multiply-adds,
   * so it computes TCC's bits and the swap can't move a voice. */
  snprintf(cmd, sizeof cmd,
           "%scc -O2 -march=native -ffp-contract=off -fno-stack-protector "
           "-fno-asynchronous-unwind-tables -c -o '%s' '%s' 2>&1",
           background && access("/usr/bin/nice", X_OK) == 0 ? "nice -n 19 " : "", ofile, cfile);
  FILE* pp = popen(cmd, "r");
  char out[1024] = "";
  size_t got = pp ? fread(out, 1, sizeof out - 1, pp) : 0;
  out[got] = 0;
  int code = pp ? pclose(pp) : -1;
  unlink(cfile);
  if (code != 0) { unlink(ofile); snprintf(err, errlen, "cc failed: %s", out); return -1; }
  pthread_mutex_lock(&tcc_lock);
  tcc_error[0] = 0;
  TCCState* s = new_tcc(err, errlen);
  int ok = s && tcc_add_file(s, ofile) >= 0 && tcc_relocate(s) >= 0;
  TickFn tick = ok ? (TickFn)tcc_get_symbol(s, "tick") : NULL;
  int (*size_fn)(void) = ok ? (int (*)(void))tcc_get_symbol(s, "voice_state_size") : NULL;
  pthread_mutex_unlock(&tcc_lock);
  unlink(ofile);
  if (!ok || !tick || !size_fn || size_fn() != state_size) {
    if (s) tcc_delete(s);
    snprintf(err, errlen, ok ? "optimized build doesn't match the loaded layout"
                             : "linking optimized object failed: %s", tcc_error);
    return -1;
  }
  *lib_out = s;
  *tick_out = tick;
  return 0;
}

/* ---------------------------------------------------------------- previews */

void voice_snapshot_into(Voice* src, Voice* dst, void* buf) {
  /* `dst` as `src` is right now, for a preview render: its layout and
   * parts, its state copied into `buf` (zeroed, src's size). Copies the
   * struct and only the pool's regions in use, to keep the mutex short.
   * dst gets no code: commit a Prepared to it next. */
  Version* s = &src->cur;
  long slots = ((VoiceHeader*)s->state)->pool_slots;
  memcpy(buf, s->state, s->state_size - slots * sizeof(double));
  place_pool(buf, s->state_size, slots);
  double* sp = ((VoiceHeader*)s->state)->pool;
  double* dp = ((VoiceHeader*)buf)->pool;
  for (int i = 0; i < s->nregions; i++)
    memcpy(&dp[s->regions[i].offset], &sp[s->regions[i].offset], s->regions[i].size * sizeof(double));
  Version* d = &dst->cur;
  d->state = buf;
  d->state_size = s->state_size;
  d->var_names = sdup_list((const char**)s->var_names, s->nvars); d->nvars = s->nvars;
  d->part_names = sdup_list((const char**)s->part_names, s->nparts); d->nparts = s->nparts;
  d->unheard = sdup_list((const char**)s->unheard, s->nunheard); d->nunheard = s->nunheard;
  d->regions = calloc(s->nregions ? s->nregions : 1, sizeof(VRegion));
  d->nregions = s->nregions;
  for (int i = 0; i < s->nregions; i++) {
    VRegion* r = &s->regions[i];
    d->regions[i] = (VRegion){sdup(r->type_name), r->per_type_idx, sdup(r->scope), r->scope_idx,
                              sdup(r->owner), r->offset, r->size};
  }
  set_parts(dst, d->part_names, d->nparts, NULL, 0);
  int n = s->nparts;
  memcpy(dst->part_gains, src->part_gains, n * sizeof(double));
  memcpy(dst->part_fade_targets, src->part_fade_targets, n * sizeof(double));
  memcpy(dst->part_fade_deltas, src->part_fade_deltas, n * sizeof(double));
  memcpy(dst->part_peak, src->part_peak, n * sizeof(double));
  memcpy(dst->part_rms_sq, src->part_rms_sq, n * sizeof(double));
  memcpy(dst->part_parked, src->part_parked, n * sizeof(double));
  memcpy(dst->part_park_pending, src->part_park_pending, n);
  ((VoiceHeader*)buf)->profiling = 0;          /* a preview isn't profiled */
  dst->sweep_origin = src->sweep_origin;
}

void voice_dispose(Voice* v) {
  /* Free a preview voice, which the audio thread never saw: its state,
   * its prev and its code. */
  pthread_mutex_lock(&tcc_lock);
  for (int i = 0; i < v->nold; i++) if (v->old_libs[i]) tcc_delete(v->old_libs[i]);
  if (v->cur.lib) tcc_delete(v->cur.lib);
  if (v->prev.lib) tcc_delete(v->prev.lib);
  pthread_mutex_unlock(&tcc_lock);
  v->cur.lib = v->prev.lib = NULL;
  free_version(&v->prev);
  free_version(&v->cur);
  free(v->old_libs);
  free(v->part_gains); free(v->part_fade_targets); free(v->part_fade_deltas);
  free(v->part_peak); free(v->part_rms_sq); free(v->part_parked); free(v->part_park_pending);
  free(v->part_out);
  free(v->prof);
  free(v);
}

/* ------------------------------------------------------------------ ticks */

void voice_tick(Voice* v, double t, double* l, double* r) {
  /* One sample at engine time t: what render, try and the tests use. */
  VoiceHeader* h = v->cur.state;
  h->t = t;
  h->sweep_origin = v->sweep_origin;
  v->cur.tick(v->cur.state, l, r);
}

void voice_tick_block(Voice* v, long n, double* outL, double* outR) {
  /* n samples from the clock in the header: the engine's path. */
  VoiceHeader* h = v->cur.state;
  h->sweep_origin = v->sweep_origin;
  v->cur.tick_block(v->cur.state, n, outL, outR);
}

int voice_prof_names(Voice* v, const char*** names) {
  /* `aither profile`'s slot names, as the voice's code lists them. */
  if (!v->cur.lib) return 0;
  const char** nm = tcc_get_symbol(v->cur.lib, "prof_names");
  const long* count = tcc_get_symbol(v->cur.lib, "prof_count");
  if (!nm || !count) return 0;
  *names = nm;
  return *count < VOICE_MAX_PROF_SLOTS ? (int)*count : VOICE_MAX_PROF_SLOTS;
}
