## Stateful natives nested in another native's args. Each native call
## points s->idx at its own pool region; if that happened before the
## args ran, an inner call (`x |> bpf(..) |> hpf(..)`) moved s->idx and
## the outer filter ran on the wrong slots and blew up. A chain must
## sound exactly like the same chain split over `let`s.

import ../parser, ../voice, ../codegen

const Stdlib = staticRead("../stdlib.aither")
const SR = 48000

proc load(src, name: string): NativeVoice =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(src)
  setSource(userAst, name)
  let prog = Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)
  result = newVoice(float64(SR))
  result.load(prog, float64(SR))

proc same(name, chained, split: string) =
  let a = load(chained, name & "_chained")
  let b = load(split, name & "_split")
  for i in 0 ..< SR:                       # one second
    let t = float64(i) / float64(SR)
    let x = a.tick(t)
    let y = b.tick(t)
    doAssert abs(x.l) < 8.0, name & ": chain blew up at sample " & $i
    doAssert x.l == y.l and x.r == y.r,
      name & ": chain differs from split at sample " & $i
  echo "ok ", name

# noise() would differ between the two voices, so use a deterministic source.
same("two filters",
  "osc(saw, 110) |> bpf(1000, 0.6) |> hpf(500, 0.2)",
  "let a = osc(saw, 110) |> bpf(1000, 0.6)\na |> hpf(500, 0.2)")

same("three filters",
  "osc(saw, 110) |> lpf(2000, 0.3) |> hpf(200, 0.2) |> lpf(800, 0.5)",
  "let a = osc(saw, 110) |> lpf(2000, 0.3)\nlet b = a |> hpf(200, 0.2)\nb |> lpf(800, 0.5)")

same("def with a filter, piped",
  "def src(f):\n  osc(saw, f) |> lpf(2000, 0.3)\n\nlet b = src(110) |> lpf(500, 0.3)\nb",
  "def src(f):\n  osc(saw, f) |> lpf(2000, 0.3)\n\nlet a = src(110)\nlet b = a |> lpf(500, 0.3)\nb")

same("native in cutoff arg",
  "osc(saw, 110) |> lpf(200 + discharge(impulse(4), 8) * 2000, 0.5)",
  "let c = 200 + discharge(impulse(4), 8) * 2000\nosc(saw, 110) |> lpf(c, 0.5)")
