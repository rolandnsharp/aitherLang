/* Transparent signal helpers (lpf, hpf, bpf, lp1, hp1, drive, fold,
 * delay, fbdelay, slew, discharge, pluck, swell, adsr, gain, prev,
 * tremolo) accept either mono or stereo input and preserve the shape:
 * stereo input gives stereo output with TWO independent state
 * allocations, one per channel.
 *
 * What this test pins:
 * 1. Region count: stereo input doubles the per-helper regions, mono
 *    stays at one. Captures hot-reload migration's invariant that
 *    structurally matching call sites keep their state.
 * 2. Per-channel state independence: feeding [impulse, 0] through a
 *    stereo lpf rings on L only; R stays silent. Proves the L and R
 *    filters are truly independent, not shared.
 * 3. Pipe chains (stereo |> lpf |> drive |> fold) compile and run.
 * 4. Mono-input helpers stay single-region (no regression).
 * 5. pan with stereo input raises a clear, actionable error.
 * 6. A stateful scalar arg (e.g. lfo modulator) is shared across the
 *    two channel emissions instead of registering twice: a bug-fix
 *    that surfaces once stereo splits routinely include native
 *    stateful helpers. */

#include "check.h"

static void must_compile(const char* src) {
  Generated g;
  char err[4096];
  CHECK(compile_full(src, &g, err, sizeof err) == 0, "doesn't compile: %s\n%s", err, src);
}

static int count_regions(const char* src, const char* type_name) {
  Generated g;
  char err[4096];
  int n = 0;
  if (compile_full(src, &g, err, sizeof err) != 0) CHECK(0, "doesn't compile: %s\n%s", err, src);
  else
    for (int i = 0; i < g.nregions; i++) n += strcmp(g.regions[i].type_name, type_name) == 0;
  return n;
}

static void regions_are(const char* src, const char* type_name, int want) {
  int got = count_regions(src, type_name);
  CHECK(got == want, "%d %s regions, wanted %d, in\n%s", got, type_name, want, src);
}

/* A compile error (not a parse error) saying `expected`. */
static void should_fail(const char* src, const char* expected) {
  Generated g;
  char err[4096] = "";
  int rc = compile_full(src, &g, err, sizeof err);
  if (rc == 0) CHECK(0, "expected error containing '%s', got success", expected);
  else
    CHECK(rc == -2 && strstr(err, expected), "expected a compile error containing '%s', got: %s",
          expected, err);
}

TEST(transparent_helpers_polymorphic) {
  /* 1. Region count for each transparent helper. Each native helper
   * that accepts stereo registers two regions of its own type. Mono
   * stays at one. The (type_name, per_type_idx, size) tuple is what
   * voice_commit uses for hot-reload migration; one region per channel
   * keeps that path intact. */
  static const char* native_cases[][3] = {
    {"lpf",       "let s = [sin(t), cos(t)]\ns |> lpf(800, 0.5)\n",       "lpf"},
    {"hpf",       "let s = [sin(t), cos(t)]\ns |> hpf(200, 0.5)\n",       "hpf"},
    {"bpf",       "let s = [sin(t), cos(t)]\ns |> bpf(800, 0.5)\n",       "bpf"},
    {"lp1",       "let s = [sin(t), cos(t)]\ns |> lp1(800)\n",            "lp1"},
    {"hp1",       "let s = [sin(t), cos(t)]\ns |> hp1(200)\n",            "hp1"},
    {"delay",     "let s = [sin(t), cos(t)]\ns |> delay(0.01, 0.05)\n",   "delay"},
    {"fbdelay",   "let s = [sin(t), cos(t)]\ns |> fbdelay(0.01, 0.05, 0.3)\n", "fbdelay"},
    {"slew",      "let s = [sin(t), cos(t)]\ns |> slew(0.01)\n",          "lp1"},
    {"discharge", "let s = [sin(t), cos(t)]\ns |> discharge(20)\n",       "discharge"},
    {"tremolo",   "let s = [sin(t), cos(t)]\ns |> tremolo(5, 0.5)\n",     "turner"},  /* a steady LFO turns */
  };
  for (size_t k = 0; k < sizeof native_cases / sizeof *native_cases; k++) {
    int got = count_regions(native_cases[k][1], native_cases[k][2]);
    CHECK(got == 2, "%s stereo: expected 2 %s regions, got %d", native_cases[k][0],
          native_cases[k][2], got);
  }

  /* Mono input still allocates one region only. Hot-reload migration
   * requires the structural identity to be preserved across re-emits. */
  regions_are("let s = sin(TAU * phasor(440))\ns |> lpf(800, 0.5)\n", "lpf", 1);

  /* 2. Aither-defined stateful helpers. pluck wraps `discharge`; the
   * inlined body claims a discharge region per call site. swell uses
   * `$level` (one var region). adsr uses `$level` + `$peaked` (two vars
   * per call). prev uses `$last` (one var). Doubling the calls via
   * stereo input therefore doubles the inlined region claims: exactly
   * the per-channel state separation we want. */
  regions_are("let s = [sin(t), cos(t)]\ns |> pluck(0.1)\n", "discharge", 2);
  regions_are("let s = [sin(t), cos(t)]\ns |> swell(0.1, 0.5)\n", "var", 2);
  regions_are("let s = [sin(t), cos(t)]\ns |> adsr(0.01, 0.3, 0.7, 0.4)\n", "var", 4);
  regions_are("let s = [sin(t), cos(t)]\ns |> prev\n", "var", 2);

  /* Pure aither defs (drive, fold, gain) have no state, but they're in
   * the allowlist so the path is uniform. Just verify they compile in
   * stereo context; region count is irrelevant. */
  must_compile("let s = [sin(t), cos(t)]\ns |> drive(1.5)\n");
  must_compile("let s = [sin(t), cos(t)]\ns |> fold(1.5)\n");
  must_compile("let s = [sin(t), cos(t)]\ns |> gain(0.5)\n");

  /* 3. Pipe chain from the task definition-of-done. */
  must_compile("let s = [sin(t), cos(t)]\ns |> lpf(800, 0.5) |> drive(1.2) |> fold(0.7)\n");
  regions_are("let s = [sin(t), cos(t)]\ns |> lpf(800, 0.5) |> drive(1.2) |> fold(0.7)\n", "lpf",
              2);

  /* 4. Per-channel state independence (audible). Feed an asymmetric
   * stereo signal through lpf: only the L channel carries a 110 Hz
   * tone, R is silent. After the filter, L should still carry a
   * (filtered) tone and R should remain near-silent. If the L and R
   * filter slots were aliased, R's accumulator would pick up energy
   * from L's input. */
  {
    Voice* v = must_load("let trig = if t < 0.001 then 1 else 0\n"
                         "let asymStereo = [sin(TAU * phasor(110)) * 0.5, 0]\n"
                         "asymStereo |> lpf(2000, 0.3)\n", 48000.0);
    if (v) {
      double sum_l = 0.0, sum_r = 0.0;
      for (int i = 0; i < 24000; i++) {
        double l, r;
        tick_at(v, (double)i / 48000.0, &l, &r);
        sum_l += l * l;
        sum_r += r * r;
      }
      double rms_l = sum_l / 24000.0, rms_r = sum_r / 24000.0;
      CHECK(rms_l > 0.01, "L channel should carry a filtered tone, rmsL² = %g", rms_l);
      CHECK(rms_r < 1e-10, "R channel should stay silent (independent state), rmsR² = %g", rms_r);
    }
  }

  /* A second variant: each channel carries a different tone through the
   * same delay. After the delay, each channel should be a phase-shifted
   * version of its own input, not a mix. */
  {
    Voice* v = must_load("let s = [sin(TAU * phasor(220)), sin(TAU * phasor(440))]\n"
                         "s |> delay(0.001, 0.05)\n", 48000.0);
    if (v) {
      double l, r;
      /* Skip past the delay buffer fill so we observe the steady state,
       * then accumulate channel energy. */
      for (int i = 0; i < 1024; i++) tick_at(v, (double)i / 48000.0, &l, &r);
      double sum_l = 0.0, sum_r = 0.0;
      for (int i = 1024; i < 1024 + 24000; i++) {
        tick_at(v, (double)i / 48000.0, &l, &r);
        sum_l += l * l;
        sum_r += r * r;
      }
      /* Both channels should carry their own tone (RMS² ≈ 0.5 for unit
       * sine): if delay slots were aliased, the two tones would mix and
       * one or both RMS values would deviate substantially. */
      double rms_l = sum_l / 24000.0, rms_r = sum_r / 24000.0;
      CHECK(fabs(rms_l - 0.5) < 0.05, "L channel rms² should be ≈ 0.5 (sine through delay), got %g",
            rms_l);
      CHECK(fabs(rms_r - 0.5) < 0.05, "R channel rms² should be ≈ 0.5, got %g", rms_r);
    }
  }

  /* 5. Stateful scalar arg shared across channels.
   * `lpf(stereo, modulated_cutoff, q)` should NOT register the
   * modulator's state twice. That used to happen via the original-AST-
   * node duplication in the split path; the fix binds via a fresh ident
   * per scalar arg. Region count: 2 stereo source oscillators + 1 lfo =
   * 3 total (each a turning point, its frequency steady). */
  const char* modulated = "let s = [sin(TAU*phasor(440)), sin(TAU*phasor(660))]\n"
                          "s |> lpf(800 + sin(TAU*phasor(2)) * 100, 0.5)\n";
  regions_are(modulated, "turner", 3);
  regions_are(modulated, "lpf", 2);

  /* 6. pan refuses stereo input with a clear error. (haas takes a pair
   * since 2026-10-12: see tests/stereo_haas_reverb.) */
  should_fail("let s = [sin(t), cos(t)]\ns |> pan(0)\n", "a pair of pairs");

  /* 7. Migration identity preserved across mono to mono reload. A patch
   * that uses a transparent helper on mono input must still match its
   * own (type_name, per_type_idx, size) on a no-op reload. */
  {
    const char* patch = "let s = sin(TAU * phasor(220))\ns |> lpf(800, 0.5)\n";
    Generated g1, g2;
    char err[4096];
    if (compile_full(patch, &g1, err, sizeof err) != 0 ||
        compile_full(patch, &g2, err, sizeof err) != 0) {
      CHECK(0, "doesn't compile: %s", err);
    } else {
      CHECK(g1.nregions == g2.nregions, "%d regions, then %d", g1.nregions, g2.nregions);
      for (int i = 0; i < g1.nregions && i < g2.nregions; i++) {
        Region* x = &g1.regions[i];
        Region* y = &g2.regions[i];
        CHECK(strcmp(x->type_name, y->type_name) == 0 && x->per_type_idx == y->per_type_idx &&
              x->size == y->size, "region %d: %s#%d (%ld) then %s#%d (%ld)", i, x->type_name,
              x->per_type_idx, x->size, y->type_name, y->per_type_idx, y->size);
      }
    }
  }
}
