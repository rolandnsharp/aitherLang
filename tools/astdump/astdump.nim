## Print a patch's syntax tree as parser.nim reads it (compare with astdump.c).
import std/os
import ../../parser

proc snprintf(buf: cstring; n: csize_t; fmt: cstring): cint {.importc, header: "<stdio.h>", varargs.}

proc num(x: float64): string =
  var b: array[64, char]
  discard snprintf(cast[cstring](addr b[0]), 64, "%.17g", x)
  $cast[cstring](addr b[0])

proc dump(n: Node): string =
  if n == nil: return "nil"
  result = "(" & $n.kind & " " & $n.line & " \"" & n.str & "\" " & num(n.num) &
           (if n.isRec: " rec" else: "")
  if n.params.len > 0 or n.kind in {nkDef, nkLambda, nkPlay}:
    result.add " ["
    for i, p in n.params:
      if i > 0: result.add " "
      result.add p
      if i < n.defaults.len and n.defaults[i] != nil: result.add "=" & dump(n.defaults[i])
    result.add "]"
  for k in n.kids: result.add " " & dump(k)
  result.add ")"

for f in commandLineParams():
  echo "== ", f
  try: echo dump(parseProgram(readFile(f)))
  except CatchableError as e: echo "error: ", e.msg
