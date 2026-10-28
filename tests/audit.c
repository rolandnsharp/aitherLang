/* End-to-end audit pipeline: render an inline patch, run analysis,
 * format. The CLI's `audit` subcommand calls these same functions. */

#include "check.h"

/* A patch rendered, mono-mixed, analysed and formatted as `audit` does.
 * NULL (and a failed check) if it doesn't render. */
static char* audit_of(const char* src, double seconds) {
  Rendered r;
  char err[4096];
  if (render_src(src, seconds, 48000, &r, err, sizeof err) != 0) {
    CHECK(0, "doesn't render: %s", err);
    return NULL;
  }
  double* mono = mono_mix(r.left, r.right, r.n);
  SpectrumSummary s;
  analyze(mono, r.n, 48000.0, 8, &s);
  char* txt = cli_audit("<inline>", seconds, 48000.0, &s);
  free(mono);
  rendered_free(&r);
  return txt;
}

TEST(audit) {
  /* 1. Pure 440 Hz sine: formatted output mentions 440 in the top
   * peaks AND the fundamental. */
  char* txt = audit_of("sin(TAU * phasor(440)) * 0.3", 1.0);
  if (txt) {
    CHECK(strstr(txt, "audit: <inline>"), "header missing: %s", txt);
    CHECK(strstr(txt, "440"), "no 440 in output: %s", txt);
    CHECK(strstr(txt, "RMS:"), "no RMS: %s", txt);
    CHECK(strstr(txt, "Top peaks:"), "no Top peaks: %s", txt);
    free(txt);
  }

  /* 2. Two-tone: both freqs surface in formatted text. */
  txt = audit_of("let s1 = sin(TAU * phasor(440)) * 0.3\n"
                 "let s2 = sin(TAU * phasor(880)) * 0.3\n"
                 "s1 + s2\n", 0.5);
  if (txt) {
    CHECK(strstr(txt, "440"), "440 missing: %s", txt);
    CHECK(strstr(txt, "880"), "880 missing: %s", txt);
    free(txt);
  }

  /* 3. Silent input: all -inf (or -240) numbers, no top peaks. */
  {
    double* buf = calloc(24000, sizeof *buf);
    SpectrumSummary s;
    analyze(buf, 24000, 48000.0, 8, &s);
    txt = cli_audit("<silent>", 0.5, 48000.0, &s);
    CHECK(strstr(txt, "non-tonal"), "silent should show 'non-tonal': %s", txt);
    CHECK(s.npeaks == 0, "silent input should have no peaks, has %d", s.npeaks);
    free(txt);
    free(buf);
  }

  /* 4. Header format pinned: `audit: <path> (<sec>s @ <sr> Hz)`. */
  {
    SpectrumSummary s = {.sr = 48000.0};
    txt = cli_audit("foo.aither", 2.0, 48000.0, &s);
    const char* want = "audit: foo.aither (2.0s @ 48000 Hz)\n";
    CHECK(strncmp(txt, want, strlen(want)) == 0, "header byte-format: %.*s",
          (int)strcspn(txt, "\n"), txt);
    free(txt);
  }
}
