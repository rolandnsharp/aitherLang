## `import "lib/x.aither"` splices a library's defs and constant tables
## into the patch: paths relative to the importing file, nested imports,
## a file imported twice included once, cycles ended, the patch's own
## def winning over the library's, and clear errors for a missing file
## or a library that holds more than defs and tables.

import std/[os, strutils]
import ../render, ../ccompile

let dir = getTempDir() / "aither_imports_" & $getCurrentProcessId()
createDir(dir / "lib")

writeFile(dir / "lib" / "tables.aither", """
let ratios = [1.0, 2.0, 3.0]
let gainK = 0.5
""")
writeFile(dir / "lib" / "voice.aither", """
import "tables.aither"
import "tables.aither"
def third(x): x * ratios[2]
def half(x): x * gainK
""")
writeFile(dir / "lib" / "cycle_a.aither", "import \"cycle_b.aither\"\ndef ca(x): x + 1\n")
writeFile(dir / "lib" / "cycle_b.aither", "import \"cycle_a.aither\"\ndef cb(x): x + 2\n")
writeFile(dir / "patch.aither", """
import "lib/voice.aither"
import "lib/cycle_a.aither"
def half(x): x * 0.25
play a:
  third(1) + half(1) + ca(0) + cb(0)
a
""")

let r = renderPatchFull(dir / "patch.aither", 0.001)
# third(1)=3, the patch's half wins (0.25), ca(0)=1, cb(0)=2.
doAssert r.audio.left[0] == 3.0 + 0.25 + 1.0 + 2.0, $r.audio.left[0]

proc errOf(path: string): string =
  try:
    discard renderPatchFull(path, 0.001)
    ""
  except CatchableError as e: e.msg

writeFile(dir / "missing.aither", "import \"lib/nope.aither\"\n0\n")
let e1 = errOf(dir / "missing.aither")
doAssert "not found" in e1 and "nope.aither" in e1 and "missing.aither:1" in e1, e1

writeFile(dir / "lib" / "noisy.aither", "def ok(x): x\nplay p:\n  1\n")
writeFile(dir / "badlib.aither", "import \"lib/noisy.aither\"\n0\n")
let e2 = errOf(dir / "badlib.aither")
doAssert "defs and constant tables only" in e2 and "play" in e2 and "noisy.aither:2" in e2, e2

writeFile(dir / "inner.aither", "def f(x):\n  import \"lib/tables.aither\"\n  x\nf(1)\n")
let e3 = errOf(dir / "inner.aither")
doAssert "top of the file" in e3, e3

# A compile error inside a library points at the library file.
writeFile(dir / "lib" / "broken.aither", "def bad(x): x + nothing\n")
writeFile(dir / "usesbroken.aither", "import \"lib/broken.aither\"\nbad(1)\n")
let e4 = errOf(dir / "usesbroken.aither")
doAssert "unknown identifier: nothing" in e4 and "broken.aither:1" in e4, e4

# Inline source resolves imports against the current directory. Go
# back afterwards: in the one-binary harness the next test would
# otherwise run from a directory that no longer exists.
let back = getCurrentDir()
setCurrentDir(dir)
let (l, _) = renderPatchSrc("import \"lib/voice.aither\"\nthird(2)", 0.001)
doAssert l[0] == 6.0, $l[0]
setCurrentDir(back)
removeDir(dir)
echo "imports ok"
