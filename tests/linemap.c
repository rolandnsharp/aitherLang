/* Item 2 test: generated C contains `#line` directives mapping
 * statements back to the aither source file. */

#include "check.h"

static const char* Src =
  "let trig = impulse(2)\n"
  "let env = discharge(trig, 12)\n"
  "sin(TAU * phasor(50)) * env\n";

TEST(linemap) {
  char err[4096];
  char* csrc = compile_csrc(Src, "patches/fake.aither", err, sizeof err);
  CHECK(csrc, "doesn't compile: %s", err);
  if (!csrc) return;
  /* Expected directives at the three source lines (1, 2, 3). */
  for (int n = 1; n <= 3; n++) {
    char want[64];
    snprintf(want, sizeof want, "#line %d \"patches/fake.aither\"", n);
    CHECK(strstr(csrc, want), "missing %s\n---\n%s", want, csrc);
  }
  int count = 0;
  for (const char* p = strstr(csrc, "#line "); p; p = strstr(p + 6, "#line ")) count++;
  CHECK(count >= 3, "expected >= 3 #line directives, got %d", count);
  printf("  %d directives\n", count);
  free(csrc);
}
