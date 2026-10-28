/* State arrays across a hot reload, and the tube written with arrays
 * checked against the generated one. (What arrays compute is in
 * tests/lang/arrays.aither.) */

#include "check.h"

static double* mono(const char* src, long n) {
  Rendered r;
  char err[4096];
  if (render_src(src, (double)n / 48000.0, 48000, &r, err, sizeof err) != 0) {
    CHECK(0, "doesn't render: %s", err);
    return calloc(n, sizeof(double));
  }
  double* l = r.left;
  r.left = NULL;
  rendered_free(&r);
  return l;
}

/* The size of state array `$name` as the compiler lays the patch out,
 * or -1 if it has none. */
static long sarr_size(const char* src, const char* name) {
  char err[4096], owner[128];
  Generated g;
  long size = -1;
  if (compile_full(src, &g, err, sizeof err) != 0) {
    CHECK(0, "doesn't compile: %s", err);
  } else {
    snprintf(owner, sizeof owner, "/$%s", name);
    for (int i = 0; i < g.nregions && size < 0; i++)
      if (strcmp(g.regions[i].type_name, "sarr") == 0 && strcmp(g.regions[i].owner, owner) == 0)
        size = g.regions[i].size;
    CHECK(size >= 0, "no state array %s", name);
  }
  return size;
}

/* `s` with every `from` replaced by `to` (malloc'd). */
static char* replaced(const char* s, const char* from, const char* to) {
  size_t fl = strlen(from), tl = strlen(to), n = 0;
  for (const char* p = s; (p = strstr(p, from)); p += fl) n++;
  char* out = malloc(strlen(s) + n * (tl > fl ? tl - fl : 0) + 1);
  char* o = out;
  for (const char* p = s;;) {
    const char* q = strstr(p, from);
    if (!q) {
      strcpy(o, p);
      break;
    }
    memcpy(o, p, q - p);
    o += q - p;
    memcpy(o, to, tl);
    o += tl;
    p = q + fl;
  }
  return out;
}

static const char drive[] =
  "let ph = phasor(110)\n"
  "let flow = pow(max(0, sin(TAU * ph)), 2) * 0.3\n"
  "let hiss = noise() * 0.02\n";
static const char ah[] =
  "flow, hiss, 5, 0.3, 7.5, 0.2, 3.6, 3.53, 3.36, 3.13, 2.92, 2.72, 2.56, 2.49";

TEST(state_arrays) {
  /* Hot reload: same shape keeps the elements; a new size starts fresh. */
  Voice* v = must_load("$h = array(3)\n$h[1] = $h[1] + 1\n$h[1]", 48000.0);
  if (v) {
    double l, r;
    for (int i = 0; i < 10; i++) tick_at(v, (double)i / 48000.0, &l, &r);
    reload(v, "$h = array(3)\n$h[1] = $h[1] + 1\n$h[1] * 2", 48000.0);
    tick_at(v, 0.0, &l, &r);
    CHECK(l == 22.0, "state array should survive a reload: %g", l);
    long size = sarr_size("$h = array(3)\n$h[1]", "h");
    CHECK(size == 4, "$h's region is %ld slots", size);
    reload(v, "$h = array(5)\n$h[1] = $h[1] + 1\n$h[1]", 48000.0);
    tick_at(v, 0.0, &l, &r);
    CHECK(l == 1.0, "a resized state array should start fresh: %g", l);
  }

  /* The tube: 24 one-sample sections, written with arrays. Same physics
   * as gen_tube24.py's def (one forward and one backward wave per
   * section, Kelly-Lochbaum scattering at each junction, area junctions
   * at the glottis and the lips), compared sample by sample. The
   * generated version rounds its interpolation weights to 4 decimals,
   * hence the tolerance. The array version is the library file
   * patches/lib/tube.aither (import "lib/tube.aither"). */
  char* tube_gen = slurp("notes/voice-eval/tube24.aither");
  char* tube_lib = slurp("patches/lib/tube.aither");
  CHECK(tube_gen && tube_lib, "the tubes' sources (run from the repository's root)");
  if (!tube_gen || !tube_lib) return;
  char* tube_arr = replaced(tube_lib, "def tube24(", "def tube24a(");
  size_t cap = strlen(tube_gen) + strlen(tube_arr) + sizeof drive + sizeof ah + 64;
  char* gen_src = malloc(cap);
  char* arr_src = malloc(cap);
  snprintf(gen_src, cap, "%s%stube24(%s)\n", tube_gen, drive, ah);
  snprintf(arr_src, cap, "%s%stube24a(%s)\n", tube_arr, drive, ah);
  long n = 9600;
  double* gen_out = mono(gen_src, n);
  double* arr_out = mono(arr_src, n);
  /* Both use one noise() at file level, so they match sample for sample. */
  double max_diff = 0.0, peak = 0.0;
  for (long i = 0; i < n; i++) {
    max_diff = fmax(max_diff, fabs(gen_out[i] - arr_out[i]));
    peak = fmax(peak, fabs(gen_out[i]));
  }
  printf("  tube24 generated vs arrays: peak %.4f, max diff %.2e\n", peak, max_diff);
  CHECK(peak > 0.01, "the tube should sound: peak %g", peak);
  CHECK(max_diff < peak * 0.02, "array tube differs from the generated one: %g", max_diff);
  free(gen_out);
  free(arr_out);
  free(gen_src);
  free(arr_src);
  free(tube_arr);
  free(tube_gen);
  free(tube_lib);
}
