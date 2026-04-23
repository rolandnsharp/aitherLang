## Step 3c regression test — array literals may span multiple lines.
##
## Live-jam pain: a 64-element pipeNotes array that wouldn't fit on one
## line had to be written as one ugly string, because any internal
## newline produced "parse error: unexpected token at 52:77 (got
## tkNewline '')". Inside [ ], the tokenizer should treat newlines
## (and their indent/dedent side effects) as whitespace, same as Python
## list literals.

import ../parser, ../codegen

const Multiline = """
let notes = [
  110.0,
  146.83,
  164.81,
  220.0,
]

play t:
  let f = wave(1.0, notes)
  sin(TAU * phasor(f)) * 0.3
"""
let prog = parseProgram(Multiline)
discard generate(prog, "multi.aither")

# Empty array across lines — also fine.
const EmptyMulti = """
let empty = [
]
let x = 0.0
x
"""
discard parseProgram(EmptyMulti)

# Nested literals + comments inside brackets still work.
const WithComments = """
let notes = [
  110.0,   # A2
  146.83,  # D3
  164.81,  # E3
  220.0
]

play t:
  wave(1.0, notes)
"""
let p2 = parseProgram(WithComments)
discard generate(p2, "multi.aither")

# A trailing newline directly before the close bracket is fine too.
const NlBeforeClose = """
let notes = [110.0, 220.0
]
play t: wave(1.0, notes)
"""
discard parseProgram(NlBeforeClose)

echo "array_multiline ok"
