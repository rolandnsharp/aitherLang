/* The live engine, as a separate process: the control socket, the pipe,
 * start and stop. Each check pins a bug found 2026-10-17 that no other
 * test could see, because the others never start an engine.
 *
 * Runs ./aither (make builds it first). A watchdog fails the test after
 * 60 s rather than let a regression hang it, since several of these
 * bugs were hangs. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "check.h"

#define EXE "./aither"

static char dir[512], sock[600];
static pid_t started[32];
static int nstarted;

static const char* const made[] = {"quiet.aither", "half.aither", "clock.aither", "sw.aither",
                                   "mp.aither", "s.sock"};

/* Whatever happens, leave no engine running and no directory behind.
 * Only async-signal-safe calls: the watchdog runs it too. */
static void cleanup(void) {
  char p[700];
  for (int i = 0; i < nstarted; i++) kill(started[i], SIGKILL);
  for (size_t i = 0; i < sizeof made / sizeof *made; i++) {
    size_t n = strlen(dir), k = strlen(made[i]);
    memcpy(p, dir, n);
    p[n] = '/';
    memcpy(p + n + 1, made[i], k + 1);
    unlink(p);
  }
  rmdir(dir);
}

static void watchdog(int sig) {
  (void)sig;
  static const char msg[] = "engine_live: timed out (a hang is the regression)\n";
  if (write(2, msg, sizeof msg - 1) < 0) {}
  cleanup();
  _exit(1);
}

/* A check the rest can't go on without: fail, clean up, stop. */
#define MUST(cond, ...)                                                         \
  do {                                                                          \
    if (!(cond)) {                                                              \
      CHECK(0, __VA_ARGS__);                                                    \
      cleanup();                                                                \
      _exit(1);                                                                 \
    }                                                                           \
  } while (0)

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static void write_file(const char* name, const char* text) {
  char p[700];
  snprintf(p, sizeof p, "%s/%s", dir, name);
  FILE* f = fopen(p, "w");
  if (f) { fputs(text, f); fclose(f); }
}

/* ----------------------------------------------------------- the socket */

static int connect_sock(void) {
  int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (s < 0) return -1;
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  if (strlen(sock) >= sizeof a.sun_path) { close(s); return -1; }
  memcpy(a.sun_path, sock, strlen(sock) + 1);
  if (connect(s, (struct sockaddr*)&a, sizeof a) != 0) { close(s); return -1; }
  return s;
}

/* One command over the socket, its reply (malloc'd; "" if no engine
 * answers within 5 s). The engine closes the connection after replying. */
static char* ask(const char* cmd) {
  char* r = calloc(1, 65537);
  int s = connect_sock();
  if (s < 0) return r;
  char line[256];
  int n = snprintf(line, sizeof line, "%s\n", cmd);
  if (send(s, line, n, MSG_NOSIGNAL) != n) { close(s); return r; }
  long got = 0;
  double end = now() + 5.0;
  while (got < 65536) {
    int left = (int)((end - now()) * 1000);
    struct pollfd p = {s, POLLIN, 0};
    if (left <= 0 || poll(&p, 1, left) <= 0) { got = 0; break; }   /* timed out: no answer */
    ssize_t k = recv(s, r + got, 65536 - got, 0);
    if (k <= 0) break;
    got += k;
  }
  r[got] = 0;
  close(s);
  return r;
}

static int starts(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

/* ask(), and does the reply start with / contain `want`. */
static int ask_starts(const char* cmd, const char* want) {
  char* r = ask(cmd);
  int ok = starts(r, want);
  free(r);
  return ok;
}

static int ask_has(const char* cmd, const char* want) {
  char* r = ask(cmd);
  int ok = strstr(r, want) != NULL;
  free(r);
  return ok;
}

static void wait_up(void) {
  for (int i = 0; i < 200; i++) {
    if (ask_starts("list", "OK")) return;
    sleep_ms(50);
  }
  MUST(0, "the engine never answered on %s", sock);
}

/* ---------------------------------------------------------- the engines */

typedef struct { pid_t pid; int in, out, err; int done, code; } Proc;

static Proc start(const char* patch, const char* arg) {
  int pi[2], po[2], pe[2];
  MUST(pipe2(pi, O_CLOEXEC) == 0 && pipe2(po, O_CLOEXEC) == 0 && pipe2(pe, O_CLOEXEC) == 0, "pipes");
  char path[700];
  snprintf(path, sizeof path, "%s/%s", dir, patch);
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  MUST(pid >= 0, "fork");
  if (pid == 0) {
    dup2(pi[0], 0);
    dup2(po[1], 1);
    dup2(pe[1], 2);
    signal(SIGPIPE, SIG_DFL);
    execl(EXE, EXE, path, arg, (char*)NULL);
    _exit(127);
  }
  close(pi[0]);
  close(po[1]);
  close(pe[1]);
  if (nstarted < 32) started[nstarted++] = pid;
  return (Proc){pid, pi[1], po[0], pe[0], 0, 0};
}

static void close_in(Proc* p) {
  if (p->in >= 0) close(p->in);
  p->in = -1;
}

static int reaped(Proc* p, int flags) {
  if (p->done) return 1;
  int st;
  if (waitpid(p->pid, &st, flags) != p->pid) return 0;
  p->done = 1;
  p->code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
  return 1;
}

static int wait_exit(Proc* p, int ms) {
  double t0 = now();
  while (!reaped(p, WNOHANG) && now() - t0 < ms / 1000.0) sleep_ms(20);
  MUST(reaped(p, WNOHANG), "the engine didn't exit within %d ms", ms);
  return p->code;
}

static void close_all(Proc* p) {
  close_in(p);
  if (p->out >= 0) close(p->out);
  if (p->err >= 0) close(p->err);
  p->out = p->err = -1;
}

/* Exactly n bytes from fd, or fewer if it ends or stalls for 10 s. */
static long read_n(int fd, char* buf, long n) {
  long got = 0;
  while (got < n) {
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 10000) <= 0) break;
    ssize_t k = read(fd, buf + got, n - got);
    if (k <= 0) break;
    got += k;
  }
  return got;
}

/* Everything up to end of file (malloc'd, NUL after), within 10 s of quiet. */
static char* read_all(int fd, long* len) {
  long cap = 65536, n = 0;
  char* b = malloc(cap + 1);
  for (;;) {
    if (n == cap) b = realloc(b, (cap *= 2) + 1);
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 10000) <= 0) break;
    ssize_t k = read(fd, b + n, cap - n);
    if (k <= 0) break;
    n += k;
  }
  b[n] = 0;
  *len = n;
  return b;
}

static float f32_at(const char* buf, long frame, int side) {
  float x;
  memcpy(&x, buf + frame * 8 + side * 4, 4);
  return x;
}

/* ------------------------------------------- 8: spam the socket meanwhile */

static volatile int spam_done;
static int slowest_ms;            /* the slowest reply while the engine ran flat out */

static void* spam(void* arg) {
  (void)arg;
  static const char* const cmds[3] = {"status", "levels", "list"};
  for (int i = 0; i < 400; i++) {
    double t0 = now();
    free(ask(cmds[i % 3]));
    int ms = (int)((now() - t0) * 1000);
    if (ms > slowest_ms) slowest_ms = ms;
  }
  __atomic_store_n(&spam_done, 1, __ATOMIC_SEQ_CST);
  return NULL;
}

/* -------------------------------------------- 11: a stream's last frame */

static Proc* framed;

static void frame(float* l, float* r) {
  char got[512 * 8];
  MUST(read_n(framed->out, got, sizeof got) == (long)sizeof got, "the stream ended early");
  *l = f32_at(got, 511, 0);
  *r = f32_at(got, 511, 1);
}

/* Send `cmd`, then read until the left side settles at `want`. */
static void until(const char* cmd, float want, float* l, float* r) {
  char* reply = ask(cmd);
  MUST(starts(reply, "OK"), "%s: %s", cmd, reply);
  free(reply);
  for (int i = 0; i < 300; i++) {
    frame(l, r);
    if (fabsf(*l - want) < 1e-4f) return;
  }
  MUST(0, "%s: the left side never reached %g (%g)", cmd, want, *l);
}

/* ------------------------------------------------------------------ test */

TEST(engine_live) {
  snprintf(dir, sizeof dir, "%s/aither-live-%d", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
           (int)getpid());
  mkdir(dir, 0700);
  snprintf(sock, sizeof sock, "%s/s.sock", dir);
  setenv("AITHER_SOCK", sock, 1);
  setenv("AITHER_RATE", "48000", 1);      /* not the shell's (44100 on the uConsole) */
  setenv("AITHER_BUFFER", "512", 1);
  write_file("quiet.aither", "0\n");
  write_file("half.aither", "input() * 0.5\n");
  signal(SIGPIPE, SIG_IGN);
  signal(SIGALRM, watchdog);
  alarm(60);

  /* --- 1. A live engine answers. (Its stdin is closed: input() is silence,
   * and it runs until its stdout pipe fills, then waits on it.) */
  Proc a = start("quiet.aither", NULL);
  close_in(&a);
  wait_up();

  /* --- 2. A client that connects and hangs up without a command, and one
   * that hangs up before reading its reply. Nim's send with SafeDisconn
   * retried a closed peer forever and froze the command loop. */
  {
    int s = connect_sock();
    if (s >= 0) close(s);
    CHECK(ask_starts("list", "OK"), "after a silent hang-up");
    int s2 = connect_sock();
    if (s2 >= 0) {
      if (send(s2, "list\n", 5, MSG_NOSIGNAL) != 5) {}
      close(s2);
    }
    sleep_ms(200);
    CHECK(ask_starts("list", "OK"), "after a hang-up mid-reply");
  }

  /* --- 3. A second engine on the same socket says one is running and stops. */
  {
    Proc b = start("quiet.aither", NULL);
    int code = wait_exit(&b, 10000);
    long n;
    char* err = read_all(b.err, &n);
    CHECK(code != 0 && strstr(err, "already running"), "second engine: %d %s", code, err);
    free(err);
    close_all(&b);
    CHECK(ask_starts("list", "OK"), "the first engine is untouched");
  }

  /* --- 4. An engine killed outright leaves its socket file; the next one
   * clears it. fileExists is false for a socket, so it never did. */
  kill(a.pid, SIGKILL);
  reaped(&a, 0);
  close_all(&a);
  {
    char* r = ask("list");
    CHECK(r[0] == 0, "the killed engine is gone: %s", r);
    free(r);
  }
  Proc c = start("quiet.aither", NULL);
  close_in(&c);
  wait_up();

  /* --- 5. `kill` ends it; "bye" goes to stderr, and stdout is whole float
   * pairs to the last byte (it used to end in the text "bye\n"). */
  CHECK(ask_has("kill", "OK"), "kill");
  wait_exit(&c, 10000);
  {
    long nout, nerr;
    char* out = read_all(c.out, &nout);
    char* err = read_all(c.err, &nerr);
    close_all(&c);
    CHECK(nout % 8 == 0, "stdout ends mid-pair: %ld bytes", nout);
    CHECK(!memmem(out, nout, "bye", 3), "bye leaked into the float stream");
    CHECK(strstr(err, "bye"), "bye belongs on stderr: %s", err);
    free(out);
    free(err);
  }

  /* --- 6. Lockstep: one block out for each block in, nothing held back.
   * stdio read ahead on stdin, so it answered only every second block and
   * tools/card deadlocked. */
  {
    Proc d = start("half.aither", NULL);
    wait_up();
    enum { Frames = 512 };
    static char block_in[Frames * 8], got[Frames * 8];
    for (int i = 0; i < Frames; i++) {
      float l = 0.8f, r = -0.4f;
      memcpy(block_in + i * 8, &l, 4);
      memcpy(block_in + i * 8 + 4, &r, 4);
    }
    for (int n = 1; n <= 5; n++) {
      MUST(write(d.in, block_in, sizeof block_in) == (ssize_t)sizeof block_in, "block %d: write", n);
      long k = read_n(d.out, got, sizeof got);     /* stalls if not lockstep */
      MUST(k == Frames * 8, "block %d: %ld bytes", n, k);
      float l = f32_at(got, Frames - 1, 0), r = f32_at(got, Frames - 1, 1);
      CHECK(fabsf(l - 0.4f) < 1e-6f && fabsf(r + 0.2f) < 1e-6f, "block %d is input() * 0.5: %g %g", n, l, r);
    }
    CHECK(ask_has("kill", "OK"), "kill");
    close_in(&d);
    wait_exit(&d, 10000);
    close_all(&d);
  }

  /* --- 7. Offline, a patch that doesn't call input() doesn't read stdin, so
   * an idle pipe there (ssh, a script) can't make it wait. */
  {
    Proc e = start("quiet.aither", "1");           /* stdin: an open, idle pipe */
    double t0 = now();
    long n;
    char* bytes = read_all(e.out, &n);              /* until it exits (or the watchdog) */
    int code = wait_exit(&e, 10000);
    close_all(&e);
    CHECK(code == 0 && now() - t0 < 10, "offline render waited");
    CHECK(n == 48000 * 8, "one second of pairs: %ld", n);
    free(bytes);
  }

  /* --- 8. The stream is gapless: every sample written is one tick of t,
   * however busy the control socket is. mixBlock used to tryAcquire the
   * lock and, when a command held it, write a block of silence without
   * advancing the clock; tv counts pixels by samples received and the
   * patch by t, so one such block sheared the picture for good. */
  {
    write_file("clock.aither", "[((t * sr) mod 65536) / 65536, 0]\n");
    Proc g = start("clock.aither", NULL);
    close_in(&g);
    wait_up();
    pthread_t th;
    pthread_create(&th, NULL, spam, NULL);
    enum { Chunk = 4096 };
    static char got[Chunk * 8];
    long prev = -1, samples = 0, gaps = 0;
    while (!__atomic_load_n(&spam_done, __ATOMIC_SEQ_CST) || samples < 48000 * 4) {
      MUST(read_n(g.out, got, sizeof got) == (long)sizeof got, "the stream ended early");
      for (int i = 0; i < Chunk; i++) {
        long tick = lround(f32_at(got, i, 0) * 65536.0);
        if (prev >= 0 && (tick - prev + 65536) % 65536 != 1) gaps++;
        prev = tick;
      }
      samples += Chunk;
    }
    pthread_join(th, NULL);
    CHECK(gaps == 0, "%ld gaps in %ld samples while the socket was busy", gaps, samples);
    /* --- 9. ...and commands still get in. This test reads as fast as it
     * can, so the engine runs flat out, like an overloaded set: the audio
     * thread took the lock back the moment it let go, and a command could
     * wait seconds (lockForCommand lets it in first). */
    CHECK(slowest_ms < 500, "a command waited %d ms for the engine", slowest_ms);
    CHECK(ask_has("kill", "OK"), "kill");
    wait_exit(&g, 10000);
    close_all(&g);
  }

  /* --- 10. `sweep`: a voice's own time, read and set over the socket. The
   * patch plays its sweep (in thousands of seconds) as its left side. */
  {
    write_file("sw.aither", "[sweep / 1000, 0]\n");
    Proc w = start("sw.aither", NULL);
    close_in(&w);
    wait_up();
    framed = &w;
    float v, r;
    frame(&v, &r);
    char* reply = ask("sweep sw");
    CHECK(starts(reply, "OK"), "sweep sw: %s", reply);
    char* nl = strchr(reply, '\n');
    double at = nl ? atof(nl + 1) : -1;
    CHECK(at >= 0 && at < 30, "a new voice's sweep starts near 0: %s", reply);
    free(reply);
    CHECK(ask_starts("sweep sw 500", "OK"), "sweep sw 500");
    v = 0;
    for (int i = 0; i < 200; i++) {               /* blocks already in the pipe come first */
      frame(&v, &r);
      if (v > 0.49f) break;
    }
    CHECK(v > 0.499f && v < 0.52f, "after sweep 500 the patch reads about 500 s: %g", v);
    CHECK(ask_starts("sweep nobody", "ERR"), "an unknown voice");
    CHECK(ask_starts("sweep sw soon", "ERR"), "seconds must be a number");
    CHECK(ask_has("kill", "OK"), "kill");
    wait_exit(&w, 10000);
    close_all(&w);
  }

  /* --- 11. mute, park and solo name a voice or one of its plays
   * (`v`, `v.a`), with a fade. The patch plays two parts of 0.25 on the
   * left and its sample count on the right, so the right side shows
   * whether the voice is running: a mute runs on, a park freezes. */
  {
    write_file("mp.aither", "$n = 0\n$n = $n + 1\nplay a: 0.25\nplay b: 0.25\n[(a + b)[0], $n / 1000000]\n");
    Proc m = start("mp.aither", NULL);
    close_in(&m);
    wait_up();
    framed = &m;
    /* 40 blocks silenced: a mute that ran on has counted them, a park
     * hasn't (its count picks up where it stopped). */
    const double held = 40 * 512 / 1000000.0;
    float l, r, n0, x, y;
    until("list", 0.5f, &l, &n0);
    until("mute mp 0.01", 0, &x, &y);
    CHECK(ask_has("list", "[muted]"), "list shows [muted]");
    for (int i = 0; i < 40; i++) frame(&x, &y);
    until("unmute mp", 0.5f, &l, &r);
    CHECK(r - n0 > held, "a muted voice runs on: %g then %g", n0, r);
    n0 = r;
    until("park mp 0.01", 0, &x, &y);
    CHECK(ask_has("list", "[parked]"), "list shows [parked]");
    for (int i = 0; i < 40; i++) frame(&x, &y);
    until("unpark mp", 0.5f, &l, &r);
    CHECK(r - n0 < held / 2, "a parked voice is frozen: %g then %g", n0, r);
    until("mute mp.a 0.01", 0.25f, &x, &y);
    until("park mp.b", 0, &x, &y);
    until("unpark mp.b", 0.25f, &x, &y);
    until("unmute mp.a", 0.5f, &x, &y);
    until("solo mp.b", 0.25f, &x, &y);
    until("unmute mp.a", 0.5f, &x, &y);
    CHECK(ask_starts("mute mp.zz", "ERR"), "no such play");
    CHECK(ask_starts("mute nobody", "ERR"), "no such voice");
    CHECK(ask_starts("mute mp a", "ERR"), "the old two-word form is gone");
    CHECK(ask_has("kill", "OK"), "kill");
    wait_exit(&m, 10000);
    close_all(&m);
  }

  alarm(0);
  cleanup();
  for (int i = 0; i < nstarted; i++) waitpid(started[i], NULL, WNOHANG);
}
