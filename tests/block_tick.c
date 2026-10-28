/* tick_block is tick, n times, to the bit.
 *
 * The engine runs voices a block at a time (codegen's BlockTemplate):
 * the clock, input(), the part bodies, the fade and part-gain ramps
 * and the meters all advance inside C. Everything render, try and the
 * tests do still goes one sample at a time through tick(). The two
 * must agree exactly, or offline and live would drift apart. */

#include "check.h"

static const char* Patch =
  "let tempo = 2\n"
  "play bass:\n"
  "  saw(TAU * phasor(55 + 10 * sin(TAU * t))) |> lpf(400 + 300 * sin(TAU * 0.5 * t), 0.7)\n"
  "play hit:\n"
  "  noise() * discharge(impulse(tempo), 30) * 0.3\n"
  "play thru:\n"
  "  input() * 0.5\n"
  "play seq:\n"
  "  sin(TAU * phasor(wave(tempo, [220, 330, 440]))) * 0.2\n"
  "\n"
  "(bass + hit) * 0.5 + thru + seq |> tanh\n";

enum { N = 4800, BLOCK = 512 };
static const double SR = 48000.0;

/* One voice, sample by sample, with the engine's clock arithmetic. */
static void single(double* out) {
  Voice* v = must_load(Patch, SR);
  double sec = 0.0, frac = 0.0, dt = 1.0 / SR;
  for (int i = 0; i < N; i++) {
    frac += dt;
    if (frac >= 1.0) {
      sec += 1.0;
      frac -= 1.0;
    }
    dsp_input_l = 0.1 * sin((double)i * 0.01);
    dsp_input_r = -dsp_input_l;
    tick_at(v, sec + frac, &out[2 * i], &out[2 * i + 1]);
  }
  dsp_input_l = dsp_input_r = 0.0;
}

/* The same voice, in blocks, through tick_block. */
static void blocks(double* out) {
  Voice* v = must_load(Patch, SR);
  VoiceHeader* hdr = v->cur.state;
  hdr->time_sec = 0.0;
  hdr->time_frac = 0.0;
  hdr->fade_gain = 1.0;
  hdr->fade_delta = 0.0;
  static double in_buf[2 * BLOCK], outL[BLOCK], outR[BLOCK];
  for (int i = 0; i < N;) {
    int n = N - i < BLOCK ? N - i : BLOCK;
    for (int k = 0; k < n; k++) {
      in_buf[2 * k] = 0.1 * sin((double)(i + k) * 0.01);
      in_buf[2 * k + 1] = -in_buf[2 * k];
    }
    hdr->in_buf = in_buf;
    voice_tick_block(v, n, outL, outR);
    for (int k = 0; k < n; k++) {
      out[2 * (i + k)] = outL[k];
      out[2 * (i + k) + 1] = outR[k];
    }
    i += n;
  }
  CHECK(hdr->bad == 0, "bad samples in the block run: %ld", hdr->bad);
}

TEST(block_tick) {
  static double a[2 * N], b[2 * N];
  single(a);
  blocks(b);
  double peak = 0.0;
  for (int i = 0; i < 2 * N; i++) {
    peak = fmax(peak, fabs(a[i]));
    if (a[i] != b[i]) {
      CHECK(0, "sample %d %s %.17g vs %.17g", i / 2, i % 2 == 0 ? "L" : "R", a[i], b[i]);
      break;
    }
  }
  CHECK(peak > 0.01, "the patch made no sound");
}
