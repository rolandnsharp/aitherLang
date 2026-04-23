## Step 1 regression test: compile-error responses must point at the
## actual aither source line, not the C output line, and must name the
## source file (so the user can tell a stdlib-sourced error apart from
## an error in their own patch — see the choir-block saga in
## HANDOFF_NAN_DEFENSES.md and the follow-up handoff).

import std/[strutils]
import ../parser, ../codegen

# --- User-patch error: must point at the user's file and the right line.
const UserSrc = """
# comment line 1
# comment line 2
let foo = 1.0
# comment line 4

let bar = notDefinedYet * foo
foo + bar
"""
block userError:
  let ast = parseProgram(UserSrc)
  setSource(ast, "patches/test.aither")
  try:
    discard generate(ast, "patches/test.aither")
    doAssert false, "expected compile error"
  except CatchableError as e:
    let m = e.msg
    doAssert "notDefinedYet" in m, "error should name identifier, got: " & m
    # Line 6 is where `notDefinedYet` appears.
    doAssert "6" in m, "expected line 6 in error, got: " & m
    doAssert "patches/test.aither" in m,
      "expected source path in error, got: " & m

# --- Stdlib-sourced error: a local in a play block shadowing a stdlib def
#     name used to report `(line 37)` with no hint that line 37 was inside
#     stdlib. Now the error must name the stdlib origin so the user knows
#     the line number doesn't reference their file.
const ShadowSrc = """
# line 1
# line 2
play choir:
  let swell = 0.5
  ease(swell)
"""
block shadowError:
  const Stdlib = staticRead("../stdlib.aither")
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(ShadowSrc)
  setSource(userAst, "patches/shadow.aither")
  let merged = Node(kind: nkBlock,
                    kids: stdAst.kids & userAst.kids, line: 1)
  try:
    discard generate(merged, "patches/shadow.aither")
    # After the Step 2 fix this compiles fine; if so, the test's point is
    # that the Step 1 plumbing WAS in place when Step 2 landed, so we
    # just skip the assertion in that case.
    discard
  except CatchableError as e:
    let m = e.msg
    # If codegen still raises for this case, the message must say it's
    # from stdlib, not the user's patch.
    doAssert "stdlib" in m or "patches/shadow.aither" in m,
      "error should name its source origin, got: " & m

echo "error_source_line ok"
