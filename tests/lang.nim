## The language's behaviour, written in aither: every tests/lang/*.aither
## holds cases, each a small patch and what it must give.
##
##   # --- what the case shows          a case starts here, named
##   # expect: 7                        sample 0 is 7 on both sides
##   # expect: 0.5, -0.5                left 0.5, right -0.5
##   # expect at 99: 1                  sample 99 (from 0) is 1
##   # expect until 4800: 0             every sample before 4800 is 0
##   # error: unknown identifier        it doesn't compile, saying this
##
## A case may have several expects; one with none need only compile and
## run. Numbers match to 1e-9 of their size. Two forms that must agree
## are one case, `A - B`, expected 0 until some sample.
## Text before the first case describes the file.

import std/[os, strutils, algorithm, math, sequtils]
import ../render

type
  Expect = object
    at: int
    until: bool               # every sample before `at`
    l, r: float64
  Case = object
    name, src, error: string
    line: int
    expects: seq[Expect]

proc parseCases(path: string): seq[Case] =
  var cur: Case
  var inCase = false
  for i, ln in toSeq(readFile(path).splitLines()):
    let s = ln.strip()
    if s.startsWith("# --- "):
      if inCase: result.add cur
      cur = Case(name: s[6 .. ^1], line: i + 1)
      inCase = true
    elif not inCase:
      continue
    elif s.startsWith("# expect"):
      let rest = s[len("# expect") .. ^1]
      let colon = rest.find(':')
      var e = Expect()
      let where = rest[0 ..< colon].strip()
      if where.startsWith("at "): e.at = parseInt(where[3 .. ^1].strip())
      elif where.startsWith("until "):
        e.at = parseInt(where[6 .. ^1].strip()) - 1
        e.until = true
      let vals = rest[colon + 1 .. ^1].split(',')
      e.l = parseFloat(vals[0].strip())
      e.r = if vals.len > 1: parseFloat(vals[1].strip()) else: e.l
      cur.expects.add e
    elif s.startsWith("# error:"):
      cur.error = s[len("# error:") .. ^1].strip()
    else:
      cur.src.add ln & "\n"
  if inCase: result.add cur

proc near(a, b: float64): bool = abs(a - b) <= 1e-9 * max(1.0, abs(b))

var failed, ran = 0
let dir = currentSourcePath().parentDir / "lang"
var files: seq[string]
for f in walkFiles(dir / "*.aither"): files.add f
files.sort()
for f in files:
  for c in parseCases(f):
    inc ran
    let where = f.extractFilename & ":" & $c.line & " (" & c.name & ")"
    var n = 1
    for e in c.expects: n = max(n, e.at + 1)
    try:
      let got = renderPatchSrc(c.src, float64(n + 1) / 48000.0)
      if c.error.len > 0:
        echo "FAIL ", where, ": compiled, expected an error with '", c.error, "'"
        inc failed
        continue
      for e in c.expects:
        for k in (if e.until: 0 else: e.at) .. e.at:
          let l = got.left[k]
          let r = got.right[k]
          if not (near(l, e.l) and near(r, e.r)):
            echo "FAIL ", where, ": sample ", k, " is ", l, ", ", r,
                 "; expected ", e.l, ", ", e.r
            inc failed
            break
    except CatchableError as x:
      if c.error.len == 0 or c.error notin x.msg:
        echo "FAIL ", where, ": ", x.msg
        inc failed

doAssert failed == 0, $failed & " of " & $ran & " language cases failed"
echo "lang ok (", ran, " cases in ", files.len, " files)"
