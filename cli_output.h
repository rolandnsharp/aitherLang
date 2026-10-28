/* Every text aither writes to stdout: formatters over the engine's
 * snapshots, a render's part levels and a spectrum. No I/O, no engine
 * state. One place for line widths, dB rounding, alignment, sparklines.
 * Each returns malloc'd text. */

#ifndef AITHER_CLI_OUTPUT_H
#define AITHER_CLI_OUTPUT_H

#include "engine.h"
#include "render.h"
#include "analysis.h"

/* Linear to dB, one decimal, or "  -inf" below 1e-9 (out: 16 bytes). */
void  cli_fmt_db(double v, char* out);

/* `send`'s compact `name  rms=X dB peak=Y dB clips=N env=<bars>`. */
char* cli_meter_line(const char* name, const StatsSnap* s);
char* cli_scope(const char* target, int found, const VoiceStatsSnap* s, int n);
char* cli_parts(const char* voice, int found, const PartSnap* p, int n);
/* What `send` prints a second after the swap. */
char* cli_check(const CheckSnap* q);
char* cli_midi_header(const char* info, int active);   /* "" if never connected */
char* cli_voice_list(const char* midi_info, int midi_active, const VoiceSnap* v, int n);
char* cli_spectrum(const SpectrumSummary* s);
char* cli_audit(const char* path, double seconds, double sr, const SpectrumSummary* s);
/* The per-part table under `audit` and `try`, loudest first. "" if none. */
char* cli_part_levels(const PartLevel* parts, int n);

#endif
