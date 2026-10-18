## The corpus check: does every patch still sound exactly as it did?
##
##   make corpus          compare every tracked .aither file with tests/corpus.golden
##   make corpus-update   re-record the golden file after an intended change
##
## Each file is rendered offline for one second with every MIDI knob at
## half and a C-E-G chord held at velocity 100, so keyboard and knob
## driven patches sound too. Its fingerprint is a hash of every sample's
## exact bits, plus RMS and peak to make a change readable. A change to
## the language, the stdlib or the engine that alters a patch it wasn't
## meant to touch fails here; one that was meant to shows up as a diff
## of tests/corpus.golden in the commit.
##
## Patches are rendered as committed at HEAD (extracted to a temporary
## tree, so imports resolve as usual), so uncommitted edits to a patch
## neither fail the check nor get recorded by an update. The engine and
## stdlib are the working tree's: those are what the check is testing.
## Noise and all other state start fresh in each render, so the result
## doesn't depend on file order.

import std/[os, osproc, strutils, math, tables, algorithm, sequtils, exitprocs]
import ../render, ../midi

const Golden = "tests/corpus.golden"
const Seconds = 1.0

proc db(x: float64): float64 = 20 * log10(max(x, 1e-12))

proc fingerprint(path: string): string =
  midiResetAll()
  for cc in 0 .. 127: midiCc(cc, 64)
  for key in [60, 64, 67]: midiNoteOn(key, 100)
  try:
    let a = renderPatch(path, Seconds)
    var h = 0xcbf29ce484222325'u64          # FNV-1a over the samples' bits
    var sum, peak = 0.0
    for i in 0 ..< a.left.len:
      for x in [a.left[i], a.right[i]]:
        var bits = cast[uint64](x)
        for _ in 0 ..< 8:
          h = (h xor (bits and 0xff)) * 0x100000001b3'u64
          bits = bits shr 8
        sum += x * x
        peak = max(peak, abs(x))
    let rms = sqrt(sum / float64(max(1, 2 * a.left.len)))
    toHex(h) & "  " & formatFloat(db(rms), ffDecimal, 1) & "  " &
      formatFloat(db(peak), ffDecimal, 1)
  except CatchableError as e:
    "ERROR  " & e.msg.splitLines()[0].replace("  ", " ")

proc parse(line: string): tuple[path, rest: string] =
  let i = line.find("  ")
  (line[0 ..< i], line[i + 2 .. ^1])

let tracked = execProcess("git ls-files '*.aither'").splitLines().filterIt(it.len > 0)
let files = tracked.filterIt("/lib/" notin it).sorted()
# Every tracked patch, libraries included, as committed at HEAD.
let head = getTempDir() / ("aither-corpus-" & $getCurrentProcessId())
createDir(head)
addExitProc(proc() = removeDir(head))
if execCmd("git archive HEAD -- " & tracked.mapIt(quoteShell(it)).join(" ") &
           " | tar -x -C " & quoteShell(head)) != 0:
  quit "corpus: couldn't extract the committed patches"
let dirty = execProcess("git diff --name-only HEAD -- '*.aither'").splitLines()
  .filterIt(it.len > 0)
if dirty.len > 0:
  echo "corpus: rendering the committed version of ", dirty.len,
       " patch(es) with uncommitted edits: ", dirty.join(", ")
var now = initOrderedTable[string, string]()
for f in files: now[f] = fingerprint(head / f)

if paramCount() >= 1 and paramStr(1) == "update":
  var o = ""
  for f, p in now: o.add f & "  " & p & "\n"
  writeFile(Golden, o)
  echo "corpus: recorded ", now.len, " files in ", Golden
  quit 0

var old = initTable[string, string]()
if fileExists(Golden):
  for line in readFile(Golden).splitLines():
    if line.len > 0:
      let (p, rest) = parse(line)
      old[p] = rest
var bad = 0
for f, p in now:
  if f notin old:
    echo "  new      ", f, "  ", p; inc bad
  elif old[f] != p:
    let o = old[f].splitWhitespace()
    let n = p.splitWhitespace()
    let lvl = if o.len == 3 and n.len == 3: "  rms " & o[1] & " -> " & n[1] & " dB"
              else: "  " & old[f] & "  ->  " & p
    echo "  changed  ", f, lvl; inc bad
for f in old.keys:
  if f notin now: echo "  gone     ", f; inc bad
if bad > 0:
  echo "corpus: ", bad, " of ", now.len, " files differ from ", Golden,
       ". If intended: make corpus-update, and commit the golden file."
  quit 1
echo "corpus ok (", now.len, " files)"
quit 0          # through the exit procs, which remove the extracted tree
