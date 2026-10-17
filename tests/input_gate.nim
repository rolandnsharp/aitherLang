## Offline runs and `audit` read stdin only for a patch that calls
## input(), so one started with an idle pipe on stdin (ssh, a script, an
## editor) doesn't wait forever. callsInput finds the call wherever it is.

import ../parser

doAssert not callsInput(parseProgram("sin(TAU * 220 * t) * 0.1")), "no input()"
doAssert callsInput(parseProgram("input() * 0.5")), "top level"
doAssert callsInput(parseProgram("def fx(g):\n  input() * g\nfx(0.5)")), "in a def body"
doAssert callsInput(parseProgram("sum(2, n => input()[0] / n)")), "in a lambda"
doAssert callsInput(parseProgram("def f(x = input()[0]):\n  x\nf()")), "in a default"
doAssert not callsInput(parseProgram("let input_gain = 0.5\ninput_gain")), "a name, not the call"

echo "input_gate ok"
