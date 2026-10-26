## The C a patch compiles to, and its state layout, from codegen.nim
## (compare with cgdump.c, the C compiler's).
import std/os
import ../../render, ../../codegen

for f in commandLineParams()[1 .. ^1]:
  echo "== ", f
  try:
    let prog = buildProgram(readFile(f), f)
    let g = generateFull(prog, "", 48000.0)
    stdout.write g.csrc
    var line = "-- vars"
    for v in g.varNames: line.add " " & v
    echo line
    line = "-- parts"
    for v in g.partNames: line.add " " & v
    echo line
    line = "-- unheard"
    for v in g.unheard: line.add " " & v
    echo line
    for r in g.regions:
      echo "-- region ", r.typeName, " ", r.perTypeIdx, " [", r.scope, "] ", r.scopeIdx,
           " [", r.owner, "] ", r.offset, " ", r.size
  except CatchableError as e:
    echo "error: ", e.msg
