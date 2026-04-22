## Item 2 test: generated C contains `#line` directives mapping statements
## back to the aither source file.

import std/[os, strutils]
import ../parser, ../codegen

const Src = """
let trig = impulse(2)
let env = discharge(trig, 12)
sin(TAU * phasor(50)) * env
"""

let prog = parseProgram(Src)
let (csrc, _, _) = generate(prog, "patches/fake.aither")

# Expected directives at the three source lines (1, 2, 3).
for n in [1, 2, 3]:
  let want = "#line " & $n & " \"patches/fake.aither\""
  doAssert want in csrc,
    "missing " & want & "\n---\n" & csrc

# Also force a TCC failure to confirm the directive surfaces in the
# error text. Inject a spurious unresolved symbol by hand-building a
# program whose C output references something TCC can't see. Simplest:
# compile via voice with no native symbols registered (would fail) —
# but that's heavy. Alternative: compile a patch whose generated C
# compiles fine; just verify the #line count by counting occurrences.
let count = csrc.count("#line ")
doAssert count >= 3, "expected ≥3 #line directives, got " & $count
echo "linemap ok (", count, " directives)"
