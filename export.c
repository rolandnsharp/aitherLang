/* aither export (export.h): a patch as C for any program to build in.
 *
 * The engine already turns every patch into C and links it in memory.
 * Export writes that C to a file instead, with its names made the
 * patch's own and a small interface after it, and writes beside it the
 * runtime the C calls: the engine's own natives (dsp.c, midi_state.c),
 * as text, the same code. So an exported patch is the patch, not a copy
 * of it: built with -ffp-contract=off it plays the engine's samples bit
 * for bit (tests/export.c checks). */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "export.h"
#include "voice.h"

extern const char aither_stdlib[];     /* stdlib.c */
/* The runtime's sources, in the binary (export_text.c). */
extern const char export_rt_h[], export_dsp_h[], export_midi_h[], export_dsp_c[], export_midi_state_c[],
    export_glue_c[];

/* The names the generated C defines for the engine to find; export
 * prefixes them, so two patches can be built into one program. */
static const char* const generated_names[] = {
  "tick", "tick_block", "voice_pool_slots", "voice_state_size", "prof_names", "prof_count",
  "task_of", "task_count", "task_needs", "task_nneeds", "run_of", "run_count", "unit_cost",
  "task_glue", "task_glue_count", NULL};

static int write_text(const char* path, const char* text, char* err, int errlen) {
  FILE* f = fopen(path, "w");
  if (!f) {
    snprintf(err, errlen, "can't write %s: %s", path, strerror(errno));
    return -1;
  }
  int ok = fputs(text, f) >= 0;
  ok = fclose(f) == 0 && ok;
  if (!ok) snprintf(err, errlen, "can't write %s", path);
  return ok ? 0 : -1;
}

/* A source file as the runtime carries it: its own `#include "..."` lines
 * dropped, since the headers they name are already above it. */
static void put_source(FILE* f, const char* name, const char* text) {
  fprintf(f, "\n/* ------------------------------------------------------------ %s */\n\n", name);
  for (const char* line = text; *line;) {
    const char* end = strchr(line, '\n');
    size_t n = end ? (size_t)(end - line + 1) : strlen(line);
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "#include \"", 10) != 0) fwrite(line, 1, n, f);
    line += n;
  }
}

static int write_runtime(const char* path, char* err, int errlen) {
  FILE* f = fopen(path, "w");
  if (!f) {
    snprintf(err, errlen, "can't write %s: %s", path, strerror(errno));
    return -1;
  }
  fputs("/* aither_rt.c: the runtime of patches exported by aither (aither_rt.h\n"
        " * says how to use it). Written by `aither export` from the engine's own\n"
        " * sources: the same filters, delays, reverb and MIDI state. Build it\n"
        " * once per program, with -ffp-contract=off. */\n",
        f);
  put_source(f, "aither_rt.h", export_rt_h);
  put_source(f, "dsp.h", export_dsp_h);
  put_source(f, "midi.h", export_midi_h);
  put_source(f, "dsp.c", export_dsp_c);
  put_source(f, "midi_state.c", export_midi_state_c);
  put_source(f, "glue.c", export_glue_c);
  int ok = fclose(f) == 0;
  if (!ok) snprintf(err, errlen, "can't write %s", path);
  return ok ? 0 : -1;
}

void export_name_of(const char* path, char* out, int outlen) {
  const char* base = strrchr(path, '/');
  base = base ? base + 1 : path;
  int n = 0;
  if (isdigit((unsigned char)base[0]) && n < outlen - 1) out[n++] = 'p';
  for (const char* c = base; *c && n < outlen - 1; c++) {
    if (!strcmp(c, ".aither")) break;
    out[n++] = isalnum((unsigned char)*c) ? *c : '_';
  }
  out[n] = 0;
}

static char* read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  rewind(f);
  char* s = malloc(n + 1);
  if (s && fread(s, 1, n, f) == (size_t)n) s[n] = 0;
  else {
    free(s);
    s = NULL;
  }
  fclose(f);
  return s;
}

static void upper(const char* s, char* out) {
  for (; *s; s++) *out++ = (char)toupper((unsigned char)*s);
  *out = 0;
}

int export_patch(const char* path, const char* dir, const char* name, double rate, char* err, int errlen) {
  char* src = read_file(path);
  if (!src) {
    snprintf(err, errlen, "can't read %s: %s", path, strerror(errno));
    return -1;
  }
  char abs[4096];
  if (!realpath(path, abs)) snprintf(abs, sizeof abs, "%s", path);
  int kind;
  Prepared* p = voice_prepare(aither_stdlib, src, abs, abs, rate, 1, 1, &kind, err, errlen);
  free(src);
  if (!p) return -1;
  Version* v = &p->v;
  mkdir(dir, 0755);
  char N[256], file[4600];
  upper(name, N);
  int rc = 0;

  /* The header: the interface. */
  snprintf(file, sizeof file, "%s/%s.h", dir, name);
  FILE* h = fopen(file, "w");
  if (!h) {
    snprintf(err, errlen, "can't write %s: %s", file, strerror(errno));
    voice_prepared_free(p);
    return -1;
  }
  fprintf(h,
          "/* %s.h: %s, exported by aither at %g Hz.\n"
          " * Build %s.c with aither_rt.c (aither_rt.h says how; that is also where\n"
          " * its keys and knobs go). The code is fixed to its rate: export again for\n"
          " * another. */\n"
          "#ifndef %s_H\n#define %s_H\n\n"
          "#define %s_RATE %g\n"
          "#define %s_PARTS %d\n\n"
          "typedef struct %s %s;\n\n"
          "/* A new instance, at t = 0, its state fresh; NULL if out of memory.\n"
          " * Instances are independent but read the same keys and knobs. */\n"
          "%s* %s_new(void);\n"
          "void %s_free(%s* v);\n\n"
          "/* The next `frames` samples, as interleaved float pairs (left, right),\n"
          " * the engine's own format; `in` is what input() hears, the same shape,\n"
          " * or NULL for silence. The mix is not limited: a patch that wants a\n"
          " * limiter ends with |> tanh. */\n"
          "void %s_render(%s* v, float* out, const float* in, long frames);\n\n"
          "/* The plays (parts), by index 0 .. %s_PARTS - 1, and back; -1: none. */\n"
          "const char* %s_part_name(int part);\n"
          "int %s_part(const char* name);\n"
          "/* Fade a part to `gain` (0 silent, 1 as written) over `seconds` (0: at\n"
          " * once). It keeps running while silent, as a muted play does. */\n"
          "void %s_gain(%s* v, int part, double gain, double seconds);\n\n"
          "/* The engine's two clocks: t, seconds since the instance began, and\n"
          " * its sweep, moved to `seconds` (0: from the top). */\n"
          "double %s_time(const %s* v);\n"
          "void %s_sweep(%s* v, double seconds);\n\n"
          "#endif\n",
          name, abs, rate, name, N, N, N, rate, N, v->nparts, name, name, name, name, name, name, name, name,
          N, name, name, name, name, name, name, name, name);
  rc = fclose(h) == 0 ? 0 : -1;

  /* The patch: its generated C, renamed, then the interface. */
  snprintf(file, sizeof file, "%s/%s.c", dir, name);
  FILE* c = rc == 0 ? fopen(file, "w") : NULL;
  if (!c) {
    if (rc == 0) snprintf(err, errlen, "can't write %s: %s", file, strerror(errno));
    voice_prepared_free(p);
    return -1;
  }
  fprintf(c,
          "/* %s.c: %s, exported by aither at %g Hz (%s.h is its interface).\n"
          " * The C below is what the engine compiles the patch to, its names made\n"
          " * this patch's own. Build with aither_rt.c and -ffp-contract=off. */\n"
          "#include <stdlib.h>\n#include <string.h>\n#include \"%s.h\"\n\n",
          name, abs, rate, name, name);
  for (int i = 0; generated_names[i]; i++) fprintf(c, "#define %s %s__%s\n", generated_names[i], name, generated_names[i]);
  fputs("\n", c);
  fputs(v->csrc, c);
  fprintf(c, "\n/* ------------------------------------------------------- the interface */\n\n");
  fprintf(c, "static const char* const %s__parts[] = {", name);
  for (int i = 0; i < v->nparts; i++) fprintf(c, "\"%s\", ", v->part_names[i]);
  fprintf(c, "NULL};\n\n");
  fprintf(c,
          "enum { %s__BLOCK = 512 };\n\n"
          "struct %s {\n"
          "  VoiceState* s;\n"
          "  double* parts;                 /* gains, fades, parked flags, meters: 6 a part */\n"
          "  unsigned char* pending;\n"
          "  double l[%s__BLOCK], r[%s__BLOCK], in[2 * %s__BLOCK];\n"
          "};\n\n",
          name, name, name, name, name);
  fprintf(c,
          "%s* %s_new(void) {\n"
          "  %s* v = calloc(1, sizeof *v);\n"
          "  long size = voice_state_size(), slots = voice_pool_slots(), np = %s_PARTS;\n"
          "  if (v) v->s = calloc(1, size);\n"
          "  if (v) v->parts = calloc(6 * (np ? np : 1), sizeof(double));\n"
          "  if (v) v->pending = calloc(np ? np : 1, 1);\n"
          "  if (!v || !v->s || !v->parts || !v->pending) {\n"
          "    %s_free(v);\n"
          "    return NULL;\n"
          "  }\n"
          "  VoiceState* s = v->s;\n"
          "  s->pool_slots = slots;\n"
          "  s->pool = (double*)((char*)s + size - slots * (long)sizeof(double));\n"
          "  s->sr = %s_RATE;\n"
          "  s->dt = 1.0 / %s_RATE;\n"
          "  s->fade_gain = 1.0;\n"
          "  s->n_parts = np;\n"
          "  s->peak_decay = 0.99993;\n"
          "  s->rms_alpha = 1.04e-4;\n"
          "  if (np) {\n"
          "    s->part_gains = v->parts;\n"
          "    s->part_fade_delta = v->parts + np;\n"
          "    s->part_fade_target = v->parts + 2 * np;\n"
          "    s->part_park = v->parts + 3 * np;\n"
          "    s->part_peak = v->parts + 4 * np;\n"
          "    s->part_sq = v->parts + 5 * np;\n"
          "    s->part_park_pending = v->pending;\n"
          "    for (long p = 0; p < np; p++) s->part_gains[p] = s->part_fade_target[p] = 1.0;\n"
          "  }\n"
          "  return v;\n"
          "}\n\n",
          name, name, name, N, name, N, N);
  fprintf(c,
          "void %s_free(%s* v) {\n"
          "  if (!v) return;\n"
          "  free(v->s);\n"
          "  free(v->parts);\n"
          "  free(v->pending);\n"
          "  free(v);\n"
          "}\n\n",
          name, name);
  fprintf(c,
          "void %s_render(%s* v, float* out, const float* in, long frames) {\n"
          "  for (long done = 0; done < frames;) {\n"
          "    long n = frames - done < %s__BLOCK ? frames - done : %s__BLOCK;\n"
          "    if (in) {\n"
          "      for (long i = 0; i < 2 * n; i++) v->in[i] = in[2 * done + i];\n"
          "      v->s->in_buf = v->in;\n"
          "    } else v->s->in_buf = NULL;\n"
          "    tick_block(v->s, n, v->l, v->r);\n"
          "    for (long i = 0; i < n; i++) {\n"
          "      out[2 * (done + i)] = (float)v->l[i];\n"
          "      out[2 * (done + i) + 1] = (float)v->r[i];\n"
          "    }\n"
          "    done += n;\n"
          "  }\n"
          "}\n\n",
          name, name, name, name);
  fprintf(c,
          "const char* %s_part_name(int part) { return part >= 0 && part < %s_PARTS ? %s__parts[part] : NULL; }\n\n"
          "int %s_part(const char* name) {\n"
          "  for (int p = 0; p < %s_PARTS; p++)\n"
          "    if (!strcmp(%s__parts[p], name)) return p;\n"
          "  return -1;\n"
          "}\n\n",
          name, N, name, name, N, name);
  fprintf(c,
          "void %s_gain(%s* v, int part, double gain, double seconds) {\n"
          "  if (part < 0 || part >= %s_PARTS) return;\n"
          "  VoiceState* s = v->s;\n"
          "  gain = gain < 0.0 ? 0.0 : gain > 1.0 ? 1.0 : gain;\n"
          "  s->part_fade_target[part] = gain;\n"
          "  if (seconds > 0.0 && gain != s->part_gains[part])\n"
          "    s->part_fade_delta[part] = (gain - s->part_gains[part]) / (seconds * %s_RATE);\n"
          "  else {\n"
          "    s->part_gains[part] = gain;\n"
          "    s->part_fade_delta[part] = 0.0;\n"
          "  }\n"
          "}\n\n",
          name, name, N, N);
  fprintf(c,
          "double %s_time(const %s* v) { return v->s->step / %s_RATE; }\n\n"
          "void %s_sweep(%s* v, double seconds) { v->s->sweep_origin = v->s->step - nearbyint(seconds * %s_RATE); }\n",
          name, name, N, name, name, N);
  rc = fclose(c) == 0 ? 0 : -1;
  if (rc) snprintf(err, errlen, "can't write %s", file);

  /* The runtime and its header. */
  if (rc == 0) {
    snprintf(file, sizeof file, "%s/aither_rt.h", dir);
    rc = write_text(file, export_rt_h, err, errlen);
  }
  if (rc == 0) {
    snprintf(file, sizeof file, "%s/aither_rt.c", dir);
    rc = write_runtime(file, err, errlen);
  }
  voice_prepared_free(p);
  return rc;
}
