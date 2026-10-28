/* Pin the byte format of every CLI line that callers grep, parse, or
 * mirror. The Phase 0 refactor moved these formatters out of engine
 * into cli_output; if a future cleanup drifts the spacing or rounding,
 * this test fails loudly instead of silently breaking external scripts. */

#include "check.h"

/* A StatsSnap with chosen RMS/peak/clips values. env_ring left at zeros
 * so the sparkline section is constant noise. */
static StatsSnap snap(double rms_l, double rms_r, double peak_l, double peak_r, long clips) {
  return (StatsSnap){.rms_l = rms_l, .rms_r = rms_r, .peak_l = peak_l, .peak_r = peak_r,
                     .clips = clips, .env_bin_idx = 0};
}

static int starts(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

/* Formatted text equals `want`, then freed. */
#define TEXT_IS(expr, want, what)                                               \
  do {                                                                          \
    char* got_ = (expr);                                                        \
    CHECK(!strcmp(got_, want), "%s: got\n%s\nwanted\n%s", what, got_, want);    \
    free(got_);                                                                 \
  } while (0)

/* 1. cli_fmt_db rounds to 1 decimal, gives "  -inf" for ~0. */
TEST(cli_output_fmt_db) {
  char b[16];
  cli_fmt_db(1.0, b);   CHECK(!strcmp(b, "0.0"), "fmt_db(1.0) = '%s'", b);
  cli_fmt_db(0.5, b);   CHECK(!strcmp(b, "-6.0"), "fmt_db(0.5) = '%s'", b);
  cli_fmt_db(0.0, b);   CHECK(!strcmp(b, "  -inf"), "fmt_db(0.0) = '%s'", b);
  cli_fmt_db(1e-10, b); CHECK(!strcmp(b, "  -inf"), "fmt_db(1e-10) = '%s'", b);
}

/* 2. cli_meter_line: name + rms + peak + clips + 20-char env spark. */
TEST(cli_output_meter_line) {
  StatsSnap s = snap(0.5, 0.5, 0.7, 0.7, 3);
  char* line = cli_meter_line("foo", &s);
  CHECK(starts(line, "foo  rms=-6.0dB peak=-3.1dB clips=3 env="), "meter line preamble: %s", line);
  /* The sparkline is ENGINE_ENV_BINS glyphs (multi-byte, so ≥ that many bytes). */
  const char* spark = strstr(line, "env=");
  size_t len = spark ? strlen(spark + 4) : 0;
  CHECK(len >= ENGINE_ENV_BINS, "spark expected >= %d bytes, got %zu: %s", ENGINE_ENV_BINS, len,
        spark ? spark + 4 : "");
  free(line);
}

/* 3. cli_scope with not-found gives the historic message. */
TEST(cli_output_scope_not_found) {
  TEXT_IS(cli_scope("nope", 0, NULL, 0), "not found: nope", "scope not found");
}

/* 4. cli_scope master block — header is literal "master". */
TEST(cli_output_scope_master) {
  VoiceStatsSnap q = {.name = "master", .is_master = 1, .stats = snap(0.1, 0.1, 0.2, 0.2, 0)};
  char* s = cli_scope("master", 1, &q, 1);
  CHECK(starts(s, "master\n"), "master block must lead with 'master\\n': %s", s);
  CHECK(strstr(s, "RMS   L"), "scope must include 'RMS   L' aligned column: %s", s);
  CHECK(strstr(s, "peak  L"), "scope must include 'peak  L' aligned column: %s", s);
  CHECK(strstr(s, "clips="), "scope must include clips=: %s", s);
  free(s);
}

/* 5. cli_scope per-voice block has state + gain in header. */
TEST(cli_output_scope_voice) {
  VoiceStatsSnap q = {.name = "bass", .is_master = 0, .state = VS_PLAYING, .gain = 0.75,
                      .stats = snap(0.3, 0.3, 0.5, 0.5, 1)};
  char* s = cli_scope("bass", 1, &q, 1);
  CHECK(starts(s, "bass  playing  gain=0.75\n"), "voice header form: %s", s);
  free(s);
}

/* 6. cli_parts: not-found, no-parts, normal cases. */
TEST(cli_output_parts) {
  TEXT_IS(cli_parts("x", 0, NULL, 0), "not found: x", "parts not found");
  TEXT_IS(cli_parts("y", 1, NULL, 0), "(no parts)", "no parts");
  PartSnap p[] = {
    {.name = "kick", .state = PS_PLAYING, .gain = 1.0, .rms = 0.5, .peak = 1.0},
    {.name = "snare", .state = PS_SILENT, .gain = 0.0},
  };
  TEXT_IS(cli_parts("drums", 1, p, 2),
          "drums\n  kick   [playing]  gain=1.00  rms=-6.0dB peak=0.0dB\n"
          "  snare  [silent]  gain=0.00  rms=-infdB peak=-infdB",
          "parts format");
}

/* 7. cli_midi_header: empty input → empty string; populated → tagged. */
TEST(cli_output_midi_header) {
  TEXT_IS(cli_midi_header("", 0), "", "never connected");
  TEXT_IS(cli_midi_header("Minilab3 (28:0)", 1), "MIDI: Minilab3 (28:0) [active]", "active");
  TEXT_IS(cli_midi_header("Minilab3 (28:0)", 0), "MIDI: Minilab3 (28:0) [DROPPED]", "dropped");
}

/* 8. cli_voice_list: empty → "(no voices)"; populated → state + gain
 *    per voice + indented per-part lines (with " (muted)" for gain≤0). */
TEST(cli_output_voice_list) {
  TEXT_IS(cli_voice_list("", 0, NULL, 0), "(no voices)", "no voices");
  PartSnap parts[] = {
    {.name = "low", .state = PS_PLAYING, .gain = 1.0},
    {.name = "high", .state = PS_SILENT, .gain = 0.0},
  };
  VoiceSnap v = {.name = "bass", .state = VS_PLAYING, .gain = 1.0, .parts = parts, .nparts = 2};
  TEXT_IS(cli_voice_list("", 0, &v, 1),
          "bass [playing] gain=1.00\n"
          "  low      gain=1.00\n"
          "  high     gain=0.00 (muted)",
          "voice list");
  /* The MIDI header leads when present. */
  char* with_midi = cli_voice_list("Minilab (28:0)", 1, &v, 1);
  CHECK(starts(with_midi, "MIDI: Minilab (28:0) [active]\n"), "midi header should lead: %s", with_midi);
  free(with_midi);
}
