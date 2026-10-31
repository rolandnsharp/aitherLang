/* How a program splits into tasks (`aither profile`'s prediction, and
 * later the tasks themselves), on the shapes music is written in: a
 * band of plays, instruments as lets in one play, a conductor read by
 * all, a part read before its writer, two parts that need each other,
 * two writers of one name. Each unit is found by the start of its
 * profile name; the test reads the tables the code carries. */

#include "check.h"

typedef struct { const char* names[1024]; int task[1024]; int n; int needs[4096][2]; int nneeds; } Split;

static void split_of(const char* src, Split* sp) {
  char err[4096];
  char* c = compile_csrc(src, "<inline>", err, sizeof err);
  memset(sp, 0, sizeof *sp);
  CHECK(c, "doesn't compile: %s", err);
  if (!c) return;
  const char* p = strstr(c, "prof_names[] = {");
  for (p = p ? strchr(p, '{') + 1 : NULL; p && *p == '"' && sp->n < 1024; ) {
    const char* e = strchr(p + 1, '"');
    sp->names[sp->n++] = strndup(p + 1, e - p - 1);
    p = e + 1;
    if (*p == ',') p += 2;
  }
  p = strstr(c, "task_of[] = {");
  for (int i = 0; p && i < sp->n; i++) {
    p = strpbrk(p, "{,") + 1;
    sp->task[i] = atoi(p);
  }
  p = strstr(c, "task_needs[] = {");
  const char* nn = strstr(c, "task_nneeds = ");
  sp->nneeds = nn ? atoi(nn + 14) : 0;
  for (int i = 0; p && i < 2 * sp->nneeds; i++) {
    p = strpbrk(p, "{,") + 1;
    sp->needs[i / 2][i % 2] = atoi(p);
  }
  free(c);
}

static int task(Split* sp, const char* name) {
  for (int i = 0; i < sp->n; i++)
    if (!strncmp(sp->names[i], name, strlen(name))) return sp->task[i];
  CHECK(0, "no unit '%s'", name);
  return -1;
}

static int needs(Split* sp, const char* a, const char* b) {
  int ta = task(sp, a), tb = task(sp, b);
  for (int i = 0; i < sp->nneeds; i++)
    if (sp->needs[i][0] == ta && sp->needs[i][1] == tb) return 1;
  return 0;
}

static Split sp;

TEST(tasks_band_of_plays) {
  split_of("play a:\n  sin(TAU * phasor(110)) * 0.1\n"
           "play b:\n  saw(TAU * phasor(55)) * 0.1\n"
           "play c:\n  noise() * 0.01\n", &sp);
  CHECK(task(&sp, "play a") != task(&sp, "play b"), "two plays are two tasks");
  CHECK(task(&sp, "play b") != task(&sp, "play c"), "two plays are two tasks");
  CHECK(!needs(&sp, "play a", "play b") && !needs(&sp, "play b", "play a"), "and neither waits");
}

TEST(tasks_instruments_in_one_play) {
  split_of("play pads:\n"
           "  let p1 = sin(TAU * phasor(220)) * 0.1\n"
           "  let p2 = sin(TAU * phasor(330)) * 0.1\n"
           "  p1 + p2\n", &sp);
  CHECK(task(&sp, "play pads/let p1") != task(&sp, "play pads/let p2"), "a play's lets split");
  CHECK(needs(&sp, "play pads (", "play pads/let p1") && needs(&sp, "play pads (", "play pads/let p2"),
        "its output waits for both");
}

TEST(tasks_conductor) {
  split_of("$pos = 0\n"
           "$pos = $pos + 1 / 48000\n"
           "play a:\n  sin(TAU * phasor(110 + $pos)) * 0.1\n"
           "play b:\n  sin(TAU * phasor(220 + $pos)) * 0.1\n", &sp);
  CHECK(task(&sp, "play a") != task(&sp, "play b"), "readers of one state stay apart");
  CHECK(task(&sp, "play a") != task(&sp, "set pos"), "and apart from its writer");
  CHECK(needs(&sp, "play a", "set pos") && needs(&sp, "play b", "set pos"), "each waits for it");
}

TEST(tasks_read_before_writer) {
  split_of("play a:\n  sin(TAU * phasor(110 + $late)) * 0.1\n"
           "$late = 0\n"
           "$late = $late + 1 / 48000\n", &sp);
  CHECK(task(&sp, "play a") != task(&sp, "set late"), "a sample-late read is a need, not a merge");
  CHECK(needs(&sp, "play a", "set late"), "the writer runs its block first");
}

TEST(tasks_coupled) {
  split_of("$x = 1\n$y = 0\n"
           "$x = $x + 0.01 * $y\n"
           "$y = $y - 0.01 * $x\n"
           "play a:\n  $x * 0.1\n", &sp);
  CHECK(task(&sp, "set x") == task(&sp, "set y"), "two parts that need each other are one task");
  CHECK(task(&sp, "play a") != task(&sp, "set x"), "a reader of the pair is not");
}

TEST(tasks_two_writers) {
  split_of("$acc = 0\n"
           "play a:\n  $acc = $acc + 1\n  sin(TAU * phasor(110)) * 0.1\n"
           "play b:\n  $acc = $acc * 0.5\n  sin(TAU * phasor(220)) * 0.1\n", &sp);
  CHECK(task(&sp, "play a/") == task(&sp, "play b/"), "two writers of one name are one task");
  CHECK(task(&sp, "play a (") != task(&sp, "play b ("), "the rest of the plays stay apart");
}
