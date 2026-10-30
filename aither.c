/* aither: the entry point. With no arguments, or patch files and an
 * optional duration, it is the engine. Every named command either talks
 * to a running engine over its socket (send, stop, list, ...) or runs
 * offline (audit). */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "engine.h"
#include "render.h"
#include "analysis.h"
#include "cli_output.h"

extern const char aither_stdlib[];     /* stdlib.c */

static const char* socket_path(void) {
  const char* s = getenv("AITHER_SOCK");
  return s ? s : "/tmp/aither.sock";
}

static int parse_float(const char* s, double* out) {
  if (!s || !*s || strpbrk(s, "xX")) return 0;
  char* end;
  double x = strtod(s, &end);
  if (*end) return 0;
  *out = x;
  return 1;
}

static int ends_with(const char* s, const char* tail) {
  size_t n = strlen(s), k = strlen(tail);
  return n >= k && strcmp(s + n - k, tail) == 0;
}

static char* absolute(const char* p) {
  char cwd[4096];
  char* r;
  if (p[0] == '/' || !getcwd(cwd, sizeof cwd)) return strdup(p);
  if (asprintf(&r, "%s/%s", cwd, p) < 0) return strdup(p);
  return r;
}

/* A patch's voice name: its file name without the extension. */
static char* base_name(const char* p) {
  const char* t = strrchr(p, '/');
  t = t ? t + 1 : p;
  const char* dot = strrchr(t, '.');
  return dot && dot > t ? strndup(t, dot - t) : strdup(t);
}

static void die(const char* msg) {
  fprintf(stderr, "%s\n", msg);
  exit(1);
}

static char* query(const char* cmd) {
  /* One command to the engine, its reply without the margins. */
  char* r;
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", socket_path());
  if (s < 0 || connect(s, (struct sockaddr*)&addr, sizeof addr) != 0) {
    if (s >= 0) close(s);
    if (asprintf(&r, "error: engine not running? (%s)", socket_path()) < 0) r = NULL;
    return r;
  }
  size_t len = strlen(cmd);
  char* line = malloc(len + 2);
  memcpy(line, cmd, len);
  line[len] = '\n';
  for (size_t put = 0; put < len + 1;) {
    ssize_t k = send(s, line + put, len + 1 - put, MSG_NOSIGNAL);
    if (k <= 0) break;
    put += k;
  }
  free(line);
  size_t cap = 4096, n = 0;
  r = malloc(cap);
  for (;;) {
    if (n + 1 >= cap) r = realloc(r, cap *= 2);
    ssize_t k = read(s, r + n, cap - n - 1);
    if (k <= 0) break;
    n += k;
  }
  close(s);
  r[n] = 0;
  const char* ws = " \t\v\r\n\f";
  while (n > 0 && strchr(ws, r[n - 1])) r[--n] = 0;
  size_t lead = strspn(r, ws);
  memmove(r, r + lead, n - lead + 1);
  return r;
}

static void send_cmd(const char* cmd) {
  char* r = query(cmd);
  printf("%s\n", r);
  fflush(stdout);
  free(r);
}

/* The words from `from` on, joined by spaces after `head`. */
static char* joined(const char* head, int argc, char** argv, int from) {
  size_t len = strlen(head) + 1;
  for (int i = from; i < argc; i++) len += strlen(argv[i]) + 1;
  char* s = malloc(len);
  strcpy(s, head);
  for (int i = from; i < argc; i++) {
    if (*s) strcat(s, " ");
    strcat(s, argv[i]);
  }
  return s;
}

/* -------------------------------------------------------------------- audit */

static long read_raw_stdin(double** l, double** r) {
  /* input() for an offline run: raw float pairs on stdin, the format the
   * engine writes, so `aither x.aither 10 > a.raw` and `aither audit
   * fx.aither < a.raw` chain. Nothing when stdin is a terminal. */
  *l = *r = NULL;
  if (isatty(0)) return 0;
  size_t cap = 1 << 16, n = 0;
  float* all = malloc(cap * sizeof(float));
  for (;;) {
    if (n == cap) all = realloc(all, (cap *= 2) * sizeof(float));
    size_t k = fread(all + n, sizeof(float), cap - n, stdin);
    if (k == 0) break;
    n += k;
  }
  long frames = (long)(n / 2);
  *l = malloc((frames ? frames : 1) * sizeof(double));
  *r = malloc((frames ? frames : 1) * sizeof(double));
  for (long i = 0; i < frames; i++) {
    (*l)[i] = all[2 * i];
    (*r)[i] = all[2 * i + 1];
  }
  free(all);
  return frames;
}

static char* read_file(const char* p) {
  FILE* f = fopen(p, "rb");
  if (!f) return NULL;
  size_t cap = 8192, n = 0;
  char* s = malloc(cap);
  size_t k;
  while ((k = fread(s + n, 1, cap - n - 1, f)) > 0) {
    n += k;
    if (n + 1 >= cap) s = realloc(s, cap *= 2);
  }
  fclose(f);
  s[n] = 0;
  return s;
}

static void first_line(char* s) { s[strcspn(s, "\r\n")] = 0; }

static void audit(const char* path, double seconds, double skip) {
  /* An offline render, its spectrum and its parts' levels. Needs no
   * engine. The rate is AITHER_RATE's, as for the engine (44.1 kHz on
   * the uConsole), so a check hears what the speaker plays; 48 kHz
   * unset. With raw pairs on stdin, input() reads them, and the render
   * runs their whole length unless a duration was given. `skip` seconds
   * run first and aren't measured, to check a later section. */
  const char* rv = getenv("AITHER_RATE");
  double rate_f = 48000.0;
  if (rv && *rv && !parse_float(rv, &rate_f)) rate_f = 48000.0;
  int rate = (int)rate_f;
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    fprintf(stderr, "audit: file not found: %s\n", path);
    exit(1);
  }
  char* src = read_file(path);
  char err[4096] = "";
  double *in_l = NULL, *in_r = NULL;
  long n_in = 0;
  int calls = render_calls_input(aither_stdlib, src, path, err, sizeof err);
  if (calls < 0) {
    first_line(err);
    fprintf(stderr, "audit: %s\n", err);
    exit(1);
  }
  if (calls) n_in = read_raw_stdin(&in_l, &in_r);
  if (n_in > 0 && seconds <= 0.0) seconds = (double)n_in / (double)rate - skip;
  if (seconds <= 0.0) seconds = 1.0;
  Rendered r;
  if (render_source(aither_stdlib, src, path, seconds, rate, in_l, in_r, n_in, skip, &r, err, sizeof err) != 0) {
    first_line(err);
    fprintf(stderr, "audit: %s\n", err);
    exit(1);
  }
  double* mono = mono_mix(r.left, r.right, r.n);
  SpectrumSummary sum;
  analyze(mono, r.n, (double)rate, 8, &sum);
  char* at = NULL;
  if (skip > 0.0) { if (asprintf(&at, "%s from %.1fs", path, skip) < 0) at = NULL; }
  char* text = cli_audit(at ? at : path, seconds, (double)rate, &sum);
  printf("%s\n", text);
  char* parts = cli_part_levels(r.parts, r.nparts);
  if (*parts) printf("%s\n", parts);
  free(parts);
  free(text);
  free(at);
  free(mono);
  rendered_free(&r);
  free(in_l);
  free(in_r);
  free(src);
}

/* --------------------------------------------------------------------- send */

static int send_patch(const char* path, const char* extra) {
  /* `send`, then listen a moment (AITHER_SEND_WAIT seconds, default 1, 0
   * to skip) and report what happened: level, quiet parts, rollback or
   * guard trips, the master. The OK line's meter alone is read the
   * instant the swap lands, before any new sound has played. */
  char* abs = absolute(path);
  char* cmd;
  if (asprintf(&cmd, "send %s%s", abs, extra) < 0) return 0;
  char* resp = query(cmd);
  free(cmd);
  free(abs);
  if (strncmp(resp, "OK", 2) != 0) {
    printf("%s\n", resp);
    free(resp);
    return 0;
  }
  free(resp);
  printf("OK\n");
  fflush(stdout);
  double wait = 1.0;
  const char* w = getenv("AITHER_SEND_WAIT");
  if (w && !parse_float(w, &wait)) wait = 1.0;
  if (wait > 0.0) {
    long ms = (long)(wait * 1000);
    struct timespec ts = {ms / 1000, ms % 1000 * 1000000L};
    nanosleep(&ts, NULL);
    char* name = base_name(path);
    if (asprintf(&cmd, "check %s", name) < 0) cmd = NULL;
    char* check = query(cmd);
    printf("%s\n", strncmp(check, "OK\n", 3) == 0 ? check + 3 : check);
    free(check);
    free(cmd);
    free(name);
  }
  return 1;
}

static void usage(void) {
  puts("usage: aither [patch.aither ...] [seconds]      the engine: float pairs on stdout");
  puts("       aither <send|stop|mute|unmute|solo|park|unpark|clear|list|status|levels|"
       "parts|check|sweep|midi|audit|try|spectrum|kill> [args]");
  puts("");
  puts("  aither | aplay -f FLOAT_LE -c 2 -r 48000      play; send patches to it");
  puts("  aither set.aither | tools/play                 same, with a first voice loaded");
  puts("  aither set.aither 30 > take.raw                offline: 30 s from t=0, then exit");
  puts("  aither set.aither 30 --play moves.txt > take.raw   offline, its keys and knobs played from a script");
  puts("  AITHER_RATE=192000 aither fm.aither | sdr-tx   the rate is yours; the reader agrees");
  puts("  arecord -f FLOAT_LE -c 2 -r 48000 | aither fx.aither | tools/play");
  puts("                                                 input() reads stdin when it is a pipe");
  puts("");
  puts("  send <file> [fade]          load patch (instant or fade-in seconds)");
  puts("  stop <name> [fade]          fade out & remove voice");
  puts("  mute <target> [fade]        fade to silence; it runs on, in step");
  puts("  unmute <target> [fade]      fade back in");
  puts("  solo <target> [fade]        mute the others: other voices, or the voice's other plays");
  puts("  park <target> [fade]        fade out, then stop running it: no CPU, state frozen");
  puts("  unpark <target> [fade]      run it again from where it stopped");
  puts("                              a target is a voice (bass) or one of its plays (bass.drone)");
  puts("  clear [fade]                stop all voices");
  puts("  list                        show active voices");
  puts("  status                      voices, parts and the master level");
  puts("  levels [name]               per-voice RMS/peak/clips/envelope");
  puts("  sweep <name> [seconds]      where the voice's sweep is; with seconds, put it there (0: from the top)");
  puts("  parts <voice>               list parts (play blocks): state, gain, rms, peak");
  puts("  check <voice>               level, quiet parts, rollbacks and guard trips");
  puts("  profile <voice> [seconds]   CPU per part, top-level code and final expression (2 s)");
  puts("  midi list                   show ALSA seq ports");
  puts("  midi connect <spec>         subscribe to a specific port (e.g. '28:0')");
  puts("  midi disconnect             drop MIDI and re-open the sequencer");
  puts("  audit <patch> [seconds] [--from S]");
  puts("                              offline render from t=0: spectrum + per-part levels");
  puts("                              (default 1s; --from runs S seconds first, unmeasured;");
  puts("                              raw pairs on stdin feed input() and set the length)");
  puts("  try <patch> [seconds] [--from S]");
  puts("                              render the patch as a send would land it NOW: the");
  puts("                              live voice's state and time, without touching the");
  puts("                              sound (default 2s). Catches NaN/runaways first");
  puts("  spectrum [voice]            spectral summary of the engine's recent buffer");
  puts("  kill                        shut down engine");
}

/* ---------------------------------------------------------------------- main */

int main(int argc, char** argv) {
  engine_init(aither_stdlib);
  int n = argc - 1;
  char** a = argv + 1;
  double x;

  /* The engine: no arguments, or patch files and an optional duration. */
  if (n == 0 || ends_with(a[0], ".aither") || parse_float(a[0], &x)) {
    char** files = calloc(n + 1, sizeof *files);
    int nf = 0;
    double seconds = 0.0;
    const char* script = NULL;
    for (int i = 0; i < n; i++) {
      if (ends_with(a[i], ".aither")) files[nf++] = a[i];
      else if (parse_float(a[i], &x)) seconds = x;
      else if (strcmp(a[i], "--play") == 0 && i + 1 < n) script = a[++i];
      else {
        usage();
        return 1;
      }
    }
    if (script) {
      if (seconds <= 0.0) {
        fprintf(stderr, "aither: --play is for an offline run: give a duration (aither patch.aither 10 --play moves.txt)\n");
        return 1;
      }
      char* err = engine_play_script(script);
      if (err) {
        fprintf(stderr, "aither: %s\n", err);
        return 1;
      }
    }
    engine_run(files, nf, seconds);
    return 0;
  }

  const char* cmd = a[0];
  char extra[1024] = "";
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
    usage();
  } else if (strcmp(cmd, "send") == 0) {
    if (n < 2) die("usage: aither send <file> [fade]");
    if (n >= 3) snprintf(extra, sizeof extra, " %s", a[2]);
    if (!send_patch(a[1], extra)) return 1;
  } else if (strcmp(cmd, "stop") == 0) {
    if (n < 2) die("usage: aither stop <name> [fade]");
    char* c = joined("stop", n < 3 ? 2 : 3, a, 1);
    send_cmd(c);
    free(c);
  } else if (!strcmp(cmd, "mute") || !strcmp(cmd, "unmute") || !strcmp(cmd, "solo") ||
             !strcmp(cmd, "park") || !strcmp(cmd, "unpark")) {
    if (n < 2 || n > 3) {
      fprintf(stderr, "usage: aither %s <voice>[.<play>] [fade]\n", cmd);
      return 1;
    }
    char* c = joined("", n, a, 0);
    send_cmd(c);
    free(c);
  } else if (strcmp(cmd, "clear") == 0) {
    char* c = joined("clear", n < 2 ? 1 : 2, a, 1);
    send_cmd(c);
    free(c);
  } else if (strcmp(cmd, "list") == 0) {
    send_cmd("list");
  } else if (strcmp(cmd, "status") == 0) {
    /* What people type: the voices and their parts, then the master. */
    send_cmd("list");
    send_cmd("levels master");
  } else if (strcmp(cmd, "levels") == 0) {
    char* c = joined("levels", n < 2 ? 1 : 2, a, 1);
    send_cmd(c);
    free(c);
  } else if (strcmp(cmd, "sweep") == 0) {
    if (n < 2) die("usage: aither sweep <name> [seconds]");
    char* c = joined("sweep", n < 3 ? 2 : 3, a, 1);
    send_cmd(c);
    free(c);
  } else if (strcmp(cmd, "parts") == 0) {
    if (n < 2) die("usage: aither parts <voice>");
    char* c = joined("parts", 2, a, 1);
    send_cmd(c);
    free(c);
  } else if (strcmp(cmd, "profile") == 0) {
    if (n < 2) die("usage: aither profile <voice> [seconds]");
    char* c = joined("profile", n < 3 ? 2 : 3, a, 1);
    send_cmd(c);
    free(c);
  } else if (strcmp(cmd, "check") == 0) {
    if (n < 2) die("usage: aither check <voice>");
    char* name = base_name(a[1]);
    char* c;
    if (asprintf(&c, "check %s", name) >= 0) {
      send_cmd(c);
      free(c);
    }
    free(name);
  } else if (strcmp(cmd, "midi") == 0) {
    if (n < 2) die("usage: aither midi list|connect <spec>|disconnect");
    char* c = joined("midi", n, a, 1);
    send_cmd(c);
    free(c);
  } else if (strcmp(cmd, "audit") == 0 || strcmp(cmd, "try") == 0) {
    char use[128];
    snprintf(use, sizeof use, "usage: aither %s <patch> [seconds] [--from S]", cmd);
    if (n < 2) die(use);
    double seconds = 0.0, skip = 0.0;
    for (int i = 2; i < n; i++) {
      if (strcmp(a[i], "--from") == 0 && i + 1 < n) {
        if (!parse_float(a[i + 1], &skip)) die(use);
        i++;
      } else if (!parse_float(a[i], &seconds)) {
        die(use);
      }
    }
    if (strcmp(cmd, "audit") == 0) {
      audit(a[1], seconds, skip);
    } else {
      char* abs = absolute(a[1]);
      char* c;
      if (asprintf(&c, "try %s %.17g %.17g", abs, seconds > 0.0 ? seconds : 2.0, skip) < 0) return 1;
      char* resp = query(c);
      printf("%s\n", strncmp(resp, "OK\n", 3) == 0 ? resp + 3 : resp);
      int ok = strncmp(resp, "OK", 2) == 0;
      free(resp);
      free(c);
      free(abs);
      if (!ok) return 1;
    }
  } else if (strcmp(cmd, "spectrum") == 0) {
    char* c = joined("spectrum", n < 2 ? 1 : 2, a, 1);
    send_cmd(c);
    free(c);
  } else if (strcmp(cmd, "kill") == 0) {
    send_cmd("kill");
  } else {
    printf("unknown command: %s\n", cmd);
    usage();
    return 1;
  }
  return 0;
}
