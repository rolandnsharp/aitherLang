/* The tools a stream goes through (tools/fft, tools/ifft, tools/add) and
 * the plays a stream starts with (--solo, --mute): run as a shell runs
 * them, on raw float pairs in a temporary directory. */

#define _GNU_SOURCE
#include <unistd.h>
#include "check.h"

static char dir[64];

static void in_temp(void) {
  snprintf(dir, sizeof dir, "/tmp/aither-pipes-XXXXXX");
  CHECK(mkdtemp(dir) != NULL, "a temporary directory");
}

static const char* at(const char* name) {
  static char paths[4][128];
  static int k;
  char* p = paths[k++ % 4];
  snprintf(p, 128, "%s/%s", dir, name);
  return p;
}

static int sh(const char* fmt, const char* a, const char* b, const char* c) {
  char cmd[1024];
  snprintf(cmd, sizeof cmd, fmt, a, b, c);
  return system(cmd);
}

static long load(const char* path, float** out) {
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long bytes = ftell(f);
  rewind(f);
  *out = malloc(bytes ? bytes : 1);
  long n = (long)fread(*out, 2 * sizeof(float), bytes / 8, f);
  fclose(f);
  return n;
}

static void save_noise(const char* path, long n) {
  FILE* f = fopen(path, "wb");
  unsigned x = 1;
  for (long i = 0; i < n; i++) {
    x = x * 1664525u + 1013904223u;
    float v = (float)((x >> 8) / 16777216.0 - 0.5);
    float pair[2] = {v, v};
    fwrite(pair, sizeof pair, 1, f);
  }
  fclose(f);
}

static void write_text(const char* path, const char* text) {
  FILE* f = fopen(path, "w");
  fputs(text, f);
  fclose(f);
}

static char* read_text(const char* path) {
  FILE* f = fopen(path, "r");
  char* s = calloc(1, 1 << 16);
  if (f) { fread(s, 1, (1 << 16) - 1, f); fclose(f); }
  return s;
}

/* Every frame size, whole frames or not, gives the signal back N / 2 late. */
TEST(fft_and_ifft_give_the_signal_back) {
  in_temp();
  static const long sizes[][2] = {{4, 1000}, {256, 5000}, {256, 5001}, {1024, 300}};
  for (int t = 0; t < 4; t++) {
    long size = sizes[t][0], n = sizes[t][1], h = size / 2;
    save_noise(at("in"), n);
    char arg[16];
    snprintf(arg, sizeof arg, "%ld", size);
    CHECK(sh("tools/fft %1$s < %2$s | tools/ifft %1$s > %3$s", arg, at("in"), at("out")) == 0, "the pipe ran");
    float *x = NULL, *y = NULL;
    long nx = load(at("in"), &x), ny = load(at("out"), &y);
    CHECK(ny >= nx + h, "all of it came back: %ld of %ld", ny, nx);
    double worst = 0;
    for (long i = 0; i < nx && i + h < ny; i++) worst = fmax(worst, fabs(y[2 * (i + h)] - x[2 * i]));
    CHECK(worst < 1e-6, "frames of %ld: off by %g", size, worst);
    free(x); free(y);
  }
}

/* A sine is the bin it should be, near its level. */
TEST(fft_puts_a_sine_in_its_bin) {
  in_temp();
  FILE* f = fopen(at("sine"), "wb");
  for (long i = 0; i < 4096; i++) {
    float v = (float)(0.5 * sin(2 * M_PI * 8 * i / 256.0)), pair[2] = {v, v};
    fwrite(pair, sizeof pair, 1, f);
  }
  fclose(f);
  CHECK(sh("tools/fft 256 < %s > %s%s", at("sine"), at("bins"), "") == 0, "fft ran");
  float* b = NULL;
  long n = load(at("bins"), &b);
  CHECK(n >= 128 * 8, "frames came: %ld", n);
  const float* frame = b + 2 * 128 * 4;
  int top = 1;
  for (int k = 1; k < 128; k++)
    if (hypot(frame[2 * k], frame[2 * k + 1]) > hypot(frame[2 * top], frame[2 * top + 1])) top = k;
  CHECK(top == 8, "the loudest bin is %d", top);
  NEAR(hypot(frame[16], frame[17]), 0.5 * 2 / M_PI, 0.05, "its level");
  free(b);
}

TEST(add_sums_and_delays) {
  in_temp();
  save_noise(at("a"), 2000);
  CHECK(sh("tools/add %s --delay 700 %s > %s", at("a"), at("a"), at("sum")) == 0, "add ran");
  float *x = NULL, *y = NULL;
  long nx = load(at("a"), &x), ny = load(at("sum"), &y);
  CHECK(ny == nx + 700, "the longer stream sets the length: %ld", ny);
  double worst = 0;
  for (long i = 0; i < ny; i++) {
    double want = (i < nx ? x[2 * i] : 0.0) + (i >= 700 ? x[2 * (i - 700)] : 0.0);
    worst = fmax(worst, fabs(y[2 * i] - want));
  }
  CHECK(worst < 1e-6, "off by %g", worst);
  free(x); free(y);
}

/* --solo parks a play it leaves out when nothing else reads it, and runs
 * one on silent when a heard play does: the solo is the same either way,
 * bit for bit, as muting the rest. */
TEST(solo_parks_only_what_nothing_hears) {
  in_temp();
  write_text(at("three.aither"),
             "$depth = 0\n"
             "play wobble:\n  $depth = 0.5 + 0.5 * sin(TAU * phasor(3))\n  sin(TAU * phasor(5000)) * 0.1\n"
             "play hiss:\n  noise() * 0.05\n"
             "play tone:\n  sin(TAU * phasor(220)) * 0.3 * $depth\n"
             "wobble + hiss + tone\n");
  const char* run = "AITHER_RT=0 AITHER_RATE=48000 ./aither %1$s 0.25 %2$s </dev/null 2>%3$s.err > %3$s";
  CHECK(sh(run, at("three.aither"), "--solo tone", at("solo")) == 0, "--solo");
  CHECK(sh(run, at("three.aither"), "--mute wobble,hiss", at("mute")) == 0, "--mute");
  char* said = read_text(at("solo.err"));
  CHECK(strstr(said, "1 plays of three parked; running silent, as a heard play reads them: wobble") != NULL,
        "it says what it parked: %s", said);
  float *a = NULL, *b = NULL;
  long n = load(at("solo"), &a);
  CHECK(n == 12000 && load(at("mute"), &b) == n, "the same length");
  CHECK(memcmp(a, b, 2 * n * sizeof(float)) == 0, "the solo is the same bits as muting the rest");
  double some = 0;
  for (long i = 0; i < 2 * n; i++) some = fmax(some, fabs(a[i]));
  CHECK(some > 0.1, "the solo sounds, wobbling: %g", some);
  free(a); free(b); free(said);
}

/* --solo and --mute hold from the first sample, and split a patch in two
 * that add back to it. */
TEST(solo_and_mute_start_with_the_stream) {
  in_temp();
  write_text(at("two.aither"), "play low:\n  sin(TAU * phasor(220)) * 0.3\nplay high:\n  sin(TAU * phasor(3000)) * 0.2\nlow + high\n");
  const char* run = "AITHER_RT=0 AITHER_RATE=48000 ./aither %s 0.25 %s </dev/null 2>/dev/null > %s";
  CHECK(sh(run, at("two.aither"), "", at("full")) == 0, "the whole patch");
  CHECK(sh(run, at("two.aither"), "--solo low", at("low")) == 0, "--solo");
  CHECK(sh(run, at("two.aither"), "--mute low", at("high")) == 0, "--mute");
  CHECK(sh(run, at("two.aither"), "--solo nobody", at("none")) != 0, "a name that isn't a play stops it");
  float *f = NULL, *l = NULL, *h = NULL;
  long n = load(at("full"), &f);
  CHECK(n == 12000, "a quarter of a second: %ld", n);
  CHECK(load(at("low"), &l) == n && load(at("high"), &h) == n, "the same length");
  double worst = 0, some = 0;
  for (long i = 0; i < 2 * n; i++) {
    worst = fmax(worst, fabs(f[i] - l[i] - h[i]));
    some = fmax(some, fabs(l[i]));
  }
  CHECK(worst < 1e-6, "they add back: off by %g", worst);
  CHECK(some > 0.25, "the solo sounds: %g", some);
  free(f); free(l); free(h);
}
