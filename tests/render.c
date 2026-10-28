/* render.c regression tests: pure offline render produces the expected
 * sample count and energy without depending on the engine. */

#include "check.h"

/* A patch from disk, rendered with its path as the
 * tag, so imports resolve beside it, as `audit` does. -1 with
 * "patch not found" when the file isn't there. */
static int render_file(const char* path, double seconds, int sr, Rendered* out, char* err,
                       int errlen) {
  char* src = slurp(path);
  if (!src) {
    snprintf(err, errlen, "patch not found: %s", path);
    return -1;
  }
  int rc = render_source(aither_stdlib, src, path, seconds, sr, NULL, NULL, 0, 0.0, out, err,
                         errlen);
  free(src);
  return rc;
}

static int rendered(const char* src, double seconds, Rendered* r) {
  char err[4096];
  int rc = render_src(src, seconds, 48000, r, err, sizeof err);
  CHECK(rc == 0, "doesn't render: %s", err);
  return rc == 0;
}

TEST(render) {
  Rendered r;
  char err[4096];

  /* 1. Pure 440 Hz sine for 1s @ 48kHz: 48000 samples per channel,
   * peak ≈ 1.0, no NaN. */
  if (rendered("sin(TAU * phasor(440))", 1.0, &r)) {
    CHECK(r.n == 48000, "%ld samples a side", r.n);
    double peak = 0.0;
    for (long i = 0; i < r.n; i++) {
      if (r.left[i] != r.left[i]) {
        CHECK(0, "NaN in left");
        break;
      }
      peak = fmax(peak, fabs(r.left[i]));
    }
    CHECK(peak > 0.95 && peak <= 1.0, "440 Hz sine peak should be ~1.0, got %g", peak);
    rendered_free(&r);
  }

  /* 2. Mono patch mirrors to both channels (the engine's tick). */
  if (rendered("sin(TAU * phasor(220)) * 0.3", 0.1, &r)) {
    for (long i = 0; i < r.n; i++)
      if (r.left[i] != r.right[i]) {
        CHECK(0, "mono should mirror at sample %ld", i);
        break;
      }
    rendered_free(&r);
  }

  /* 3. Stereo patch returns two distinct channels. */
  if (rendered("let s1 = sin(TAU * phasor(440)) * 0.3\n"
               "let s2 = sin(TAU * phasor(660)) * 0.3\n"
               "[s1, s2]\n", 0.1, &r)) {
    int any_diff = 0;
    for (long i = 0; i < r.n && !any_diff; i++) any_diff = r.left[i] != r.right[i];
    CHECK(any_diff, "stereo channels should differ");
    rendered_free(&r);
  }

  /* 4. Render the real backing.aither for 0.5s: non-zero, no NaN.
   * Confirms the renderer handles the full stdlib + non-trivial patches. */
  int rc = render_file("patches/backing.aither", 0.5, 48000, &r, err, sizeof err);
  CHECK(rc == 0, "backing.aither doesn't render: %s", err);
  if (rc == 0) {
    CHECK(r.n == 24000, "%ld samples", r.n);
    double energy = 0.0;
    for (long i = 0; i < r.n; i++) energy += r.left[i] * r.left[i] + r.right[i] * r.right[i];
    CHECK(energy > 0.001, "backing.aither should produce energy, got %g", energy);
    rendered_free(&r);
  }

  /* 5. Zero seconds: empty buffers (smoke test for "does it parse?"). */
  if (rendered("sin(TAU * phasor(220))", 0.0, &r)) {
    CHECK(r.n == 0, "%ld samples from 0 s", r.n);
    rendered_free(&r);
  }

  /* 6. Missing file: an error, not silence. */
  rc = render_file("/nonexistent.aither", 0.1, 48000, &r, err, sizeof err);
  CHECK(rc != 0 && strstr(err, "not found"), "missing file should fail (rc %d)", rc);
  if (rc == 0) rendered_free(&r);

  /* 7. mono_mix produces (L+R)*0.5. */
  {
    const double l[] = {1.0, 0.5, -0.2}, rr[] = {0.0, 0.5, 0.4};
    double* m = mono_mix(l, rr, 3);
    CHECK(m[0] == 0.5 && m[1] == 0.5 && m[2] == 0.1, "mono_mix: %g %g %g", m[0], m[1], m[2]);
    free(m);
  }
}
