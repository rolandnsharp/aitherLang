/* Offline runs and `audit` read stdin only for a patch that calls
 * input(), so one started with an idle pipe on stdin (ssh, a script, an
 * editor) doesn't wait forever. render_calls_input finds the call
 * wherever it is. */

#include "check.h"

static void calls_input_is(const char* src, int want, const char* where) {
  char err[4096] = "";
  int got = render_calls_input(aither_stdlib, src, "<inline>", err, sizeof err);
  CHECK(got == want, "%s: calls input %d, wanted %d %s", where, got, want, err);
}

TEST(input_gate) {
  calls_input_is("sin(TAU * 220 * t) * 0.1", 0, "no input()");
  calls_input_is("input() * 0.5", 1, "top level");
  calls_input_is("def fx(g):\n  input() * g\nfx(0.5)", 1, "in a def body");
  calls_input_is("sum(2, n => input()[0] / n)", 1, "in a lambda");
  calls_input_is("def f(x = input()[0]):\n  x\nf()", 1, "in a default");
  calls_input_is("let input_gain = 0.5\ninput_gain", 0, "a name, not the call");
}
