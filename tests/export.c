/* aither export: an exported patch, built with cc into a program of its
 * own, plays what the engine plays, bit for bit. Three patches go into one
 * program (their names mustn't collide), one of them played with keys,
 * a 14-bit knob and input() from a script, one a field of pairs in
 * arrays (patches/around.aither), and one an MPE instrument played on
 * its own channels (the script's channels in the engine, raw MIDI bytes
 * in the program); each is rendered by ./aither offline and by the
 * program, and the two streams must be the same bytes.
 *
 * Runs ./aither and cc. */

#define _GNU_SOURCE
#include <sys/stat.h>
#include <unistd.h>
#include "check.h"

static char dir[512];

static void put(const char* name, const char* text) {
  char p[700];
  snprintf(p, sizeof p, "%s/%s", dir, name);
  FILE* f = fopen(p, "w");
  if (f) {
    fputs(text, f);
    fclose(f);
  }
}

static int run(const char* cmd) {
  int rc = system(cmd);
  if (rc != 0) fprintf(stderr, "export test: failed (%d): %s\n", rc, cmd);
  return rc;
}

static long same_bytes(const char* a, const char* b, long* size) {
  /* -1 when equal; else the first byte that differs (or the shorter size). */
  FILE* fa = fopen(a, "rb");
  FILE* fb = fopen(b, "rb");
  long at = 0, diff = -2;
  if (fa && fb) {
    for (;; at++) {
      int ca = fgetc(fa), cb = fgetc(fb);
      if (ca != cb) { diff = at; break; }
      if (ca == EOF) { diff = -1; break; }
    }
  }
  *size = at;
  if (fa) fclose(fa);
  if (fb) fclose(fb);
  return diff;
}

TEST(export_exact) {
  snprintf(dir, sizeof dir, "%s/aither-export-%d", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
           (int)getpid());
  mkdir(dir, 0700);
  put("mix.aither",
      "let k = midi_cc(3)\n"
      "play keys: midi_keyboard((freq, gate, vel) => additive(freq, warm_shape, 6) * adsr(gate, 0.01, 0.2, 0.6, 0.3) * vel * 0.2)\n"
      "play fx: input() * (0.3 + k) |> delay(0.013, 0.1)\n"
      "play room: tape(mono(keys), 1, t, t - 0.25, 1) * 0.3\n"
      "play turning:\n"
      "  let (x, _) = spin(220) * spin(3)\n"
      "  x * 0.05\n"
      "(keys + fx + room + turning) |> reverb(1.2, 0.2)\n");
  put("moves.txt",
      "0.10 note 60 100\n"
      "0.40 cc 3 50\n"
      "0.40 cc 35 99\n"
      "0.75 note 64 70\n"
      "1.20 note 60 0\n"
      "1.33 cc 35 3\n"
      "1.60 note 64 0\n");
  /* An MPE zone (RPN 6 on channel 1), two notes on their own channels,
   * each bent and pressed and slid on its own, and the master's bend. */
  put("mpe.aither",
      "play voices: poly(4, (freq, gate, vel, key, press, slide) => sin(TAU * phasor(freq)) * swell(gate, 0.01, 0.2) * (0.1 + press * 0.2) * (0.5 + slide))\n"
      "voices * 0.5\n");
  put("mpe.txt",
      "0.00 cc 101 0\n0.00 cc 100 6\n0.00 cc 6 15\n"
      "0.05 bend 9000 2\n0.05 press 40 2\n0.05 note 60 100 2\n"
      "0.30 note 67 90 3\n0.30 cc 74 120 3\n"
      "0.50 bend 4000 3\n0.60 press 127 2\n0.70 bend 12000 1\n"
      "1.20 note 60 0 2\n1.50 note 67 0 3\n");
  /* 2 s of input: a slow chirp, as raw float pairs. */
  {
    char p[700];
    snprintf(p, sizeof p, "%s/in.raw", dir);
    FILE* f = fopen(p, "wb");
    for (long i = 0; f && i < 96000; i++) {
      float x = (float)(0.4 * sin(6.283185307179586 * (200.0 + i * 0.002) * i / 48000.0));
      fwrite(&x, sizeof x, 1, f);
      fwrite(&x, sizeof x, 1, f);
    }
    if (f) fclose(f);
  }
  put("host.c",
      "#include <math.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
      "#include \"aither_rt.h\"\n#include \"mix.h\"\n#include \"around.h\"\n#include \"mpe.h\"\n"
      "int main(void) {\n"
      "  long n = 96000;\n"
      "  float* in = malloc(sizeof(float) * 2 * n);\n"
      "  float* out = malloc(sizeof(float) * 2 * n);\n"
      "  FILE* f = fopen(\"in.raw\", \"rb\");\n"
      "  if (!f || fread(in, sizeof(float), 2 * n, f) != (size_t)(2 * n)) return 1;\n"
      "  fclose(f);\n"
      "  double at[16]; char kind[16][8]; int a[16], b[16], nm = 0;\n"
      "  f = fopen(\"moves.txt\", \"r\");\n"
      "  while (nm < 16 && fscanf(f, \"%lf %7s %d %d\", &at[nm], kind[nm], &a[nm], &b[nm]) == 4) nm++;\n"
      "  fclose(f);\n"
      "  mix* m = mix_new();\n"
      "  long done = 0;\n"
      "  for (int i = 0; i <= nm; i++) {\n"
      "    long due = i < nm ? llround(at[i] * MIX_RATE) : n;\n"
      "    if (due > done) { mix_render(m, out + 2 * done, in + 2 * done, due - done); done = due; }\n"
      "    if (i < nm && kind[i][0] == 'c') aither_cc(a[i], b[i]);\n"
      "    if (i < nm && kind[i][0] == 'n') aither_note_on(a[i], b[i]);\n"
      "  }\n"
      "  f = fopen(\"mix.raw\", \"wb\"); fwrite(out, sizeof(float), 2 * n, f); fclose(f);\n"
      "  mix_free(m);\n"
      "  aither_midi_reset();\n"
      "  around* r = around_new();\n"
      "  for (long i = 0; i < n; i += 1000) around_render(r, out + 2 * i, NULL, n - i < 1000 ? n - i : 1000);\n"
      "  f = fopen(\"around.raw\", \"wb\"); fwrite(out, sizeof(float), 2 * n, f); fclose(f);\n"
      "  around_free(r);\n"
      "  /* mpe: the script's moves as raw MIDI bytes, channels and all. */\n"
      "  aither_midi_reset();\n"
      "  f = fopen(\"mpe.txt\", \"r\");\n"
      "  char line[128]; double mat[32]; unsigned char msg[32][3]; int mlen[32], mn = 0;\n"
      "  while (mn < 32 && fgets(line, sizeof line, f)) {\n"
      "    char k[8]; double x[3] = {0, 0, 0};\n"
      "    int got = sscanf(line, \"%lf %7s %lf %lf %lf\", &mat[mn], k, &x[0], &x[1], &x[2]);\n"
      "    int want = k[0] == 'n' || k[0] == 'c' ? 2 : 1, ch = got - 2 > want ? (int)x[want] - 1 : 0;\n"
      "    if (k[0] == 'n') { msg[mn][0] = (x[1] > 0 ? 0x90 : 0x80) | ch; msg[mn][1] = x[0]; msg[mn][2] = x[1]; mlen[mn] = 3; }\n"
      "    if (k[0] == 'c') { msg[mn][0] = 0xB0 | ch; msg[mn][1] = x[0]; msg[mn][2] = x[1]; mlen[mn] = 3; }\n"
      "    if (k[0] == 'b') { msg[mn][0] = 0xE0 | ch; msg[mn][1] = (int)x[0] & 127; msg[mn][2] = (int)x[0] >> 7; mlen[mn] = 3; }\n"
      "    if (k[0] == 'p') { msg[mn][0] = 0xD0 | ch; msg[mn][1] = x[0]; mlen[mn] = 2; }\n"
      "    mn++;\n"
      "  }\n"
      "  fclose(f);\n"
      "  mpe* q = mpe_new();\n"
      "  done = 0;\n"
      "  for (int i = 0; i <= mn; i++) {\n"
      "    long due = i < mn ? llround(mat[i] * MPE_RATE) : n;\n"
      "    if (due > done) { mpe_render(q, out + 2 * done, NULL, due - done); done = due; }\n"
      "    if (i < mn) aither_midi(msg[i], mlen[i]);\n"
      "  }\n"
      "  f = fopen(\"mpe.raw\", \"wb\"); fwrite(out, sizeof(float), 2 * n, f); fclose(f);\n"
      "  mpe_free(q);\n"
      "  return 0;\n"
      "}\n");

  char cmd[4096];
  snprintf(cmd, sizeof cmd,
           "cd '%s' && AITHER_RATE=48000 '%s/aither' export mix.aither . > /dev/null && "
           "AITHER_RATE=48000 '%s/aither' export '%s/patches/around.aither' . > /dev/null && "
           "AITHER_RATE=48000 '%s/aither' export mpe.aither . > /dev/null && "
           "cc -O2 -ffp-contract=off -w -o host host.c mix.c around.c mpe.c aither_rt.c -lm && ./host",
           dir, getcwd((char[1024]){0}, 1024), getcwd((char[1024]){0}, 1024), getcwd((char[1024]){0}, 1024),
           getcwd((char[1024]){0}, 1024));
  int built = run(cmd) == 0;
  CHECK(built, "export, build and run the host");
  if (built) {
    snprintf(cmd, sizeof cmd,
             "cd '%s' && AITHER_RATE=48000 '%s/aither' mix.aither 2 --play moves.txt < in.raw > mix.engine 2>/dev/null && "
             "AITHER_RATE=48000 '%s/aither' '%s/patches/around.aither' 2 < /dev/null > around.engine 2>/dev/null && "
             "AITHER_RATE=48000 '%s/aither' mpe.aither 2 --play mpe.txt < /dev/null > mpe.engine 2>/dev/null",
             dir, getcwd((char[1024]){0}, 1024), getcwd((char[1024]){0}, 1024), getcwd((char[1024]){0}, 1024),
             getcwd((char[1024]){0}, 1024));
    CHECK(run(cmd) == 0, "the engine's renders");
    const char* names[3] = {"mix", "around", "mpe"};
    for (int i = 0; i < 3; i++) {
      char a[700], b[700];
      snprintf(a, sizeof a, "%s/%s.raw", dir, names[i]);
      snprintf(b, sizeof b, "%s/%s.engine", dir, names[i]);
      long size, at = same_bytes(a, b, &size);
      CHECK(at == -1, "%s: exported and engine differ at byte %ld", names[i], at);
      CHECK(size == 96000 * 8, "%s: %ld bytes, want 768000", names[i], size);
      /* And not two silences agreeing: each is heard. */
      FILE* f = fopen(a, "rb");
      double sq = 0;
      float x;
      long k = 0;
      while (f && fread(&x, sizeof x, 1, f) == 1) sq += (double)x * x, k++;
      if (f) fclose(f);
      double rms = k ? sqrt(sq / k) : 0;
      CHECK(rms > 0.003, "%s is heard: rms %g", names[i], rms);
    }
  }
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  system(cmd);
}
