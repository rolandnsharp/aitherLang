## The corpus check: does every patch still sound exactly as it did?
##
##   make corpus          compare every tracked .aither file with tests/corpus.golden
##   make corpus-update   re-record the golden file after an intended change
##
## Each file is rendered offline with every MIDI knob at half, in three
## windows, so keyboard and knob driven patches sound too and later
## sections are reached:
##   0-0.5 s    a C-E-G chord held at velocity 100
##   0.5-1.5 s  the same voice on: the chord released, F-A-C struck at
##              velocity 50 (note-offs, release tails, new attacks)
##   150 s      a fresh voice started with its clock at 150 s, the C-E-G
##              chord held, for 0.5 s: timeline pieces whose sections
##              fade in on `t` (cathedral's bells, rhythm, lead) are heard
## All on TCC (AITHER_OPT=0): no compile wait, and the same bits as the
## cc -O2 build (tests/exact_builds). Switching heavy patches to cc was
## tried and was slower: on the A53s a cc compile costs a minute or more
## with the other workers running. Patches are split across one worker
## process per core. Its fingerprint is a hash of every sample's
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

import std/[os, osproc, strutils, math, tables, algorithm, sequtils, exitprocs, streams, times]
import ../render, ../midi, ../voice

const Golden = "tests/corpus.golden"

proc db(x: float64): float64 = 20 * log10(max(x, 1e-12))

const Late = 150.0            # where the third window's fresh voice starts

proc chord(keys: openArray[int]; vel: int) =
  for k in keys: midiNoteOn(k, vel)

proc fingerprint(path: string): string =
  midiResetAll()
  for cc in 0 .. 127: midiCc(cc, 64)
  try:
    let src = readFile(path)
    var windows: seq[Stereo]
    chord([60, 64, 67], 100)
    let v = newVoice(48000.0)
    v.loadSource(Stdlib, src, path, 48000.0)
    windows.add renderVoice(v, 0.5, 48000).audio
    for k in [60, 64, 67]: midiNoteOff(k)
    chord([65, 69, 72], 50)
    windows.add renderVoice(v, 1.0, 48000, t0 = 0.5).audio
    midiResetAll()
    for cc in 0 .. 127: midiCc(cc, 64)
    chord([60, 64, 67], 100)
    let late = newVoice(48000.0)
    late.loadSource(Stdlib, src, path, 48000.0)
    windows.add renderVoice(late, 0.5, 48000, t0 = Late).audio
    var h = 0xcbf29ce484222325'u64          # FNV-1a over the samples' bits
    var sum, peak = 0.0
    var count = 0
    for a in windows:
      for i in 0 ..< a.left.len:
        for x in [a.left[i], a.right[i]]:
          var bits = cast[uint64](x)
          for _ in 0 ..< 8:
            h = (h xor (bits and 0xff)) * 0x100000001b3'u64
            bits = bits shr 8
          sum += x * x
          peak = max(peak, abs(x))
          inc count
    let rms = sqrt(sum / float64(max(1, count)))
    toHex(h) & "  " & formatFloat(db(rms), ffDecimal, 1) & "  " &
      formatFloat(db(peak), ffDecimal, 1)
  except CatchableError as e:
    "ERROR  " & e.msg.splitLines()[0].replace("  ", " ")

proc parse(line: string): tuple[path, rest: string] =
  let i = line.find("  ")
  (line[0 ..< i], line[i + 2 .. ^1])

putEnv("AITHER_OPT", "0")    # TCC throughout: same bits as cc, no compile wait

# A worker: `corpus shard K N DIR FILE...` fingerprints every Nth file
# from the Kth, rendered from the extracted tree DIR, one
# "path<TAB>print" line each. The main process runs one worker per core.
if paramCount() >= 4 and paramStr(1) == "shard":
  let k = parseInt(paramStr(2))
  let n = parseInt(paramStr(3))
  let dir = paramStr(4)
  for i in 5 .. paramCount():
    if (i - 5) mod n == k:
      let f = paramStr(i)
      let t0 = epochTime()
      let print = fingerprint(dir / f)
      stdout.write f & "\t" & print & "\t" & formatFloat(epochTime() - t0, ffDecimal, 1) & "\n"
      stdout.flushFile()
  quit 0

let tracked = execProcess("git ls-files '*.aither'").splitLines().filterIt(it.len > 0)
let files = tracked.filterIt("/lib/" notin it).sorted()
# Every tracked patch, libraries included, as committed at HEAD.
let head = getTempDir() / ("aither-corpus-" & $getCurrentProcessId())
createDir(head)
addExitProc(proc() = removeDir(head))
if execCmd("git archive HEAD -- " & tracked.mapIt(quoteShell(it)).join(" ") &
           " | tar -x -C " & quoteShell(head)) != 0:
  quit "corpus: couldn't extract the committed patches"
# stdlib.aither is compiled into the engine from the working tree, as the
# check intends, so its edits are tested, not ignored.
let dirty = execProcess("git diff --name-only HEAD -- '*.aither'").splitLines()
  .filterIt(it.len > 0 and it != "stdlib.aither")
if dirty.len > 0:
  echo "corpus: rendering the committed version of ", dirty.len,
       " patch(es) with uncommitted edits: ", dirty.join(", ")
# One worker per core, each taking every Nth patch; then in path order.
let workers = max(1, countProcessors())
var procs: seq[Process]
for k in 0 ..< workers:
  procs.add startProcess(getAppFilename(),
    args = @["shard", $k, $workers, head] & files, options = {})
var got = initTable[string, string]()
var took: seq[tuple[secs: float64; path: string]]
for p in procs:
  for line in p.outputStream.readAll().splitLines():
    let f = line.split('\t')
    if f.len == 3:
      got[f[0]] = f[1]
      took.add (parseFloat(f[2]), f[0])
  discard p.waitForExit()
  p.close()
took.sort(proc(a, b: auto): int = cmp(b.secs, a.secs))
stderr.writeLine "corpus: slowest: " &
  took[0 ..< min(5, took.len)].mapIt(it.path & " " & $int(it.secs) & " s").join(", ")
var now = initOrderedTable[string, string]()
for f in files:
  now[f] = got.getOrDefault(f, "ERROR  worker produced no fingerprint")

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
