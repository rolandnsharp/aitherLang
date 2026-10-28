/* The engine: the voices, mixed to a pipe, with a socket for live coding.
 *
 * One block at a time: read a block of input, run every voice's
 * tick_block, guard it (NaN, runaway, a recursion that didn't return,
 * a send that blew up), meter it and mix it, write the mix. Commands
 * arrive on a UNIX socket, one line each, and are answered with text.
 *
 * The text formatting, the offline render behind `try` and MIDI are
 * still Nim (stage 3 of notes/c-port-2026-10-28.md): the engine hands
 * them the snapshots below through hooks engine.nim provides (declared
 * in engine.c). */

#ifndef AITHER_ENGINE_H
#define AITHER_ENGINE_H

#include "voice.h"

#define ENGINE_ENV_BINS 20     /* sparkline slots; engine_types.nim's EnvBins */

/* The states `list`, `levels` and `check` report, in engine_types.nim's order. */
enum { VS_STOPPED, VS_MUTED, VS_PARKED, VS_FADING_OUT, VS_FADING_IN, VS_PLAYING, VS_RUNAWAY };
enum { PS_SILENT, PS_FADING_OUT, PS_FADING_IN, PS_PLAYING, PS_PARKED };

typedef struct {
  double rms_l, rms_r;         /* linear */
  double peak_l, peak_r;
  long   clips;
  float  env_ring[ENGINE_ENV_BINS];
  int    env_bin_idx;
} StatsSnap;

typedef struct {
  char*  name;
  int    state;                /* PS_* */
  double gain;
  double rms, peak;            /* linear, the louder channel's level */
  int    unheard;              /* the final expression never reaches it */
} PartSnap;

typedef struct {
  char*     name;
  int       state;             /* VS_* */
  double    gain;
  PartSnap* parts; int nparts;
} VoiceSnap;

typedef struct {
  char*     name;              /* "master" for the master mix */
  int       is_master;
  int       state;             /* ignored for the master */
  double    gain;
  StatsSnap stats;
} VoiceStatsSnap;

/* What `check` reports a second after a send. */
typedef struct {
  int            found;
  VoiceStatsSnap voice;
  StatsSnap      master;
  PartSnap*      parts; int nparts;
  int            rolled_back, stuck;
  long           nan_events, runaway_events;
  int            retrying;     /* runaway-muted, will fade back in */
} CheckSnap;

/* Reads the AITHER_* settings; the stdlib's text is kept (not copied). */
void  engine_init(const char* stdlib_src);
/* Run the engine (engine.nim's runEngine): never returns. */
void  engine_run(char** files, int nfiles, double seconds);
/* One command line, its reply (malloc'd, without the newline). */
char* engine_command(const char* line);

/* What the commands do, for the tests: NULL, or the error (malloc'd). */
char* engine_load(const char* path, double fade);
char* engine_stop(const char* name, double fade);
char* engine_silence(const char* name, int on, int park, double fade);
char* engine_solo(const char* name, double fade);
int   engine_mark_stopped(const char* name);
int   engine_active_count(void);
int   engine_voices(VoiceSnap** out);          /* free with engine_free_voices */
void  engine_free_voices(VoiceSnap* v, int n);

#endif
