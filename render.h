/* Offline render: a voice ticked for N samples into memory, with each
 * play's level. No engine, no audio thread, no disk I/O for the patch.
 *
 * Patches that read MIDI hear whatever midi.c holds (nothing, outside a
 * test). `input()` reads a recording when one is given (raw float pairs,
 * the engine's own format), so a patch that processes input can be
 * worked on against the same material every time. */

#ifndef AITHER_RENDER_H
#define AITHER_RENDER_H

#include "voice.h"

/* One play's level over a render. `low` is the share of its energy
 * below render_low_split_hz(), which laptop speakers can't play: a part
 * that is mostly low measures fine and still isn't heard. */
typedef struct {
  char*  name;
  double rms, peak, low;
  double gain;                 /* its part gain (0: muted) */
  int    unheard;              /* the final expression never reaches it */
} PartLevel;

typedef struct {
  double*    left;
  double*    right;
  long       n;
  PartLevel* parts; int nparts;
  double     seconds;          /* the measured window */
  double     skipped;          /* rendered first, not measured */
} Rendered;

enum { RENDER_NAN = 10 };      /* beside voice.h's VOICE_* kinds */

/* AITHER_LOW_HZ, 200 Hz unset: where small laptop speakers give up. */
double render_low_split_hz(void);

/* cc -O2 builds are possible: cc is on PATH and AITHER_OPT isn't 0. */
int  render_cc_available(void);

/* The voice's cc -O2 build, now (unless AITHER_OPT=0 or no cc); it stays
 * on TCC if that fails, with the same output, only slower. */
void render_optimize(Voice* v);

/* Tick `v` from engine time t0: `skip` seconds run and dropped, then
 * `seconds` kept and measured. The input, if `feed_input`, is n_in
 * frames, then silence; without it, input() reads what the engine's
 * capture holds and the render leaves it alone. 0, or -1 with the time
 * of the first NaN in err. */
int  render_voice(Voice* v, double seconds, int sr, const double* in_l, const double* in_r,
                  long n_in, double skip, double t0, int feed_input, Rendered* out,
                  char* err, int errlen);
void rendered_free(Rendered* r);

/* A patch's source, compiled (with the stdlib) and rendered from t=0.
 * 0, or the error's kind (VOICE_PARSE, VOICE_IMPORT, VOICE_COMPILE,
 * VOICE_TCC, RENDER_NAN) with its message in err. */
int  render_source(const char* stdlib_src, const char* src, const char* tag, double seconds,
                   int sr, const double* in_l, const double* in_r, long n_in, double skip,
                   Rendered* out, char* err, int errlen);

/* Does the patch, with its imports, call input()? Parsed only. 1 or 0,
 * or -1 with the parse or import error in err. */
int  render_calls_input(const char* stdlib_src, const char* src, const char* tag,
                        char* err, int errlen);

#endif
