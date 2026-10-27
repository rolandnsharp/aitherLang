## Offline runs and `audit` read stdin only for a patch that calls
## input(), so one started with an idle pipe on stdin (ssh, a script, an
## editor) doesn't wait forever. callsInput finds the call wherever it is.

import ../ccompile

doAssert not callsInput("sin(TAU * 220 * t) * 0.1"), "no input()"
doAssert callsInput("input() * 0.5"), "top level"
doAssert callsInput("def fx(g):\n  input() * g\nfx(0.5)"), "in a def body"
doAssert callsInput("sum(2, n => input()[0] / n)"), "in a lambda"
doAssert callsInput("def f(x = input()[0]):\n  x\nf()"), "in a default"
doAssert not callsInput("let input_gain = 0.5\ninput_gain"), "a name, not the call"

echo "input_gate ok"
