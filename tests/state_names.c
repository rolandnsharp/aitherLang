/* `$x` and `x` are one name to the compiler, so state and a parameter or
 * let of the same name would be one value: a compile error names both
 * lines (SPEC.md, "State"). Each frame is checked against the frames it
 * is written in, up to the def or top level that bounds it: a def's
 * parameter doesn't clash with the file's state, which it never sees. */

#include "check.h"

static int refused(const char* src, const char* name) {
  Generated g;
  char err[1024] = "";
  int rc = compile_full(src, &g, err, sizeof err);
  char want[64];
  snprintf(want, sizeof want, "'%s' is both", name);
  return rc != 0 && strstr(err, want) != NULL;
}

static int compiles(const char* src) {
  Generated g;
  char err[1024] = "";
  int rc = compile_full(src, &g, err, sizeof err);
  if (rc) fprintf(stderr, "    %s\n", err);
  return rc == 0;
}

TEST(state_names) {
  CHECK(refused("def f(x):\n  $x = 0\n  $x = $x + 1\n  x\nf(0.25)\n", "x"), "a def's parameter and its state");
  CHECK(refused("def f(a):\n  let y = a\n  $y = 0\n  y\nf(1)\n", "y"), "a def's let and its state");
  CHECK(refused("let x = 0.25\n$x = 0\n$x = $x + 1\nx\n", "x"), "a top-level let and the file's state");
  CHECK(refused("play a:\n  let n = 2\n  n\n$n = 1\na\n", "n"), "a play's let and the file's state");
  CHECK(refused("play a:\n  $m = 1\n  $m\nlet m = 2\na + m\n", "m"), "a play's state is the file's, by name");
  CHECK(refused("$k = 0\nsum(4, k => k * 0.1)\n", "k"), "a lambda's parameter and the state it is written beside");
  CHECK(compiles("def g(a):\n  sum(2, i =>\n    $a = 0\n    $a = $a + a\n    $a)\ng(1)\n"),
        "state inside a lambda, its def's parameter outside: shadowing, $a is the lambda's");
  CHECK(compiles("$x = 0\n$x = $x + 1\ndef f(x): x * 2\nf($x) * 0\n"), "a def's parameter and the file's state: a def never sees it");
  CHECK(compiles("$z = array(4, [0, 0])\nlet z = $z[1]\n$z[1] = z + [0.001, 0]\nz[0]\n"),
        "a state array and a let of one of its elements: read by index, never one value");
  CHECK(compiles("def f(a):\n  $peak = 0\n  $peak = max($peak, a)\n  $peak\nlet level = 0.5\nf(level)\n"),
        "different names are fine");
}
