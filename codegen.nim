## aither → C source transpiler. Consumes parser.Node AST (stdlib
## prepended), produces a C translation unit TCC can compile.
##
## Contract:
##   emitC(program) -> (cSource, varNames)
## `varNames` lists top-level `var`s in definition order so the engine
## can migrate state field-by-field across hot reloads.
##
## Shape of the emitted C:
##   #include <math.h>
##   extern double n_<fn>(...);                    (one per dsp.nim primitive)
##   typedef struct { pool/idx/overflow/sr    ← DspState prefix (binary-
##                    t,dt,start_t,           ←  compatible cast to
##                    <top-level vars>,       ←  DspState*)
##                    inited[...]; } VoiceState;
##   void tick(VoiceState* s, double* outL, double* outR) { ... }
##
## Always emits a stereo tick. If the final expression is scalar, both
## outputs are set equal; if it's [L,R], they're set independently.

import std/[strutils, tables, sets, strformat, sequtils, os]
import parser

const PoolSize* = 524288

type
  # A per-helper-type state region. Each stateful call site owns one
  # region, keyed by (typeName, perTypeIdx). Across a hot reload, a new
  # region with a matching (typeName, perTypeIdx, size) inherits the old
  # region's slots via copyMem — so inserting a new helper type in a
  # patch doesn't shift existing state (filters, delays, reverb tails
  # survive structural edits).
  Region* = object
    typeName*: string
    perTypeIdx*: int
    # Stable key: the statement the call sits in (`play drums/let k`,
    # `let bass`, `out`, ...) and its index among same-type calls in that
    # statement. Migration matches on this first, so a stateful call
    # added to one play or let can't shift the state of any other.
    scope*: string
    scopeIdx*: int
    # Which def the call sits in (the chain of inlined defs, `didge3>formant`;
    # "" at file level), and for `$state` the state's name (`bar/$bv`).
    # Part of both migration keys, so replacing one physical model by
    # another of the same shape (`stick` with $x/$dx x3 -> `bar` with
    # $bv/$bx x3) starts the new one clean instead of feeding it the old
    # one's displacement (2026-10-11: +24 dBFS).
    owner*: string
    offset*: int            # absolute start offset into pool (in slots)
    size*: int              # number of float64 slots
    # Optional payload written into the first `init.len` slots when the
    # voice's pool is first allocated. Used by `array_make` to seed the
    # length and capacity header (slots 0 and 1) without paying a
    # per-tick init branch. Across hot reload, region migration may
    # overwrite these — that's the desired behaviour: a same-shape array
    # carries its data forward; a resized array gets its fresh init.
    init*: seq[float64]

  Scope = ref object
    parent: Scope
    # name -> C expression text (for params, inlined let bindings)
    # and name -> "pool[<n>]" for inlined vars
    names: Table[string, string]
    # name -> aither function name (for fn-valued params like `shape`)
    fns: Table[string, string]
    # name -> nkLambda node. Lambda-valued params: a def can take a
    # lambda as an argument, and the param name resolves to an inline
    # lambda call site at use. Distinct from `fns` because lambdas have
    # no callable name — they're substituted body-and-all at the call.
    # Powers `def poly(n_voices, voice_fn): sum(n_voices, n => voice_fn(...))`.
    lambdas: Table[string, Node]
    # name -> (lExpr, rExpr). Stereo-valued bindings (params, lets).
    stereoNames: Table[string, tuple[l, r: string]]
    # name -> (C symbol, length). Play-local numeric-literal arrays that
    # were auto-hoisted to static const data; lookup walks the chain
    # (parent scopes inherit top-level arrays via the topArrays fallback
    # used only if we bottom out here).
    arrays: Table[string, tuple[sym: string; length: int]]
    # name -> (C expression of the region's first slot, length). State
    # arrays declared in this scope with `$f = array(N)`.
    sarrays: Table[string, tuple[base: string; n: int; wo: int]]
    # name -> (C array symbol, length). Per-sample local arrays:
    # `let a = [p, q, r]` with runtime values, or `let a = array(N)`.
    larrays: Table[string, tuple[sym: string; n: int]]
    # name -> compile-time numeric literal value. Populated alongside the
    # usual `names` binding whenever a def-param or let is initialised
    # from an nkNum. Consumed by builtins that need compile-time-constant
    # integer args (currently `sum(N, fn)`). Keeping this parallel to
    # `names` preserves the normal scalar-double binding for every other
    # use site — the literal is an *additional* fact, not a replacement.
    numLits: Table[string, float64]
    # name -> (expression, its scope) for a name bound to an expression
    # that can't change within a block (see isInvariant): a knob, or
    # arithmetic over knobs and numbers. Like numLits, an additional
    # fact beside the `names` binding; it lets hoisting see through
    # `let morph = midi_cc(20)` and a def param given one.
    invs: Table[string, tuple[node: Node; sc: Scope]]

  Ctx = ref object
    userDefs: Table[string, Node]
    topVarNames*: seq[string]   # top-level vars in definition order
    topVarSet: HashSet[string]
    topLets: HashSet[string]    # top-level let names (scoped to tick())
    letsDone: HashSet[string]   # top-level lets emitted so far
    letLines: Table[string, int] # top-level let name -> its line
    playNames*: seq[string]     # play blocks in definition order
    unheard*: seq[string]       # play blocks the final expression never reaches
    # Hoisting: coefficients that can't change from sample to sample are
    # computed once in hoist_init (see isInvariant). Each entry is a
    # static's declaration and its init line; hoistSeen dedupes by text.
    hoistOn: bool
    hoists: seq[tuple[decl, init: string]]
    hoistSeen: Table[string, string]
    inHoist: bool              # emitting a hoist's init: hoist nothing inside
    assigned: HashSet[string]  # names assigned somewhere: never invariant
    playSet: HashSet[string]
    stereoLets: HashSet[string] # top-level let names whose RHS is stereo
    # name -> (C array symbol, length). Populated when a top-level let
    # binds an nkArr literal of numeric constants.
    topArrays: Table[string, (string, int)]
    arrayDecls: string          # accumulates `static const double arr_N[...]`
    tmpCounter: int
    patchPath: string           # source path used in `#line` directives
    # Per-helper-type state layout. `regions` is filled during emission;
    # offsets are finalised after the walk by assigning per-type base
    # offsets in first-seen order.
    sr*: float64
    regions*: seq[Region]
    typeCounter: Table[string, int]     # name -> next perTypeIdx
    typeCumSize: Table[string, int]     # name -> cumulative size so far
    typeOrder: seq[string]              # types in first-seen order
    scope: string                       # statement being emitted (Region.scope)
    scopeCounter: Table[string, int]    # scope & type & owner -> next scopeIdx
    defStack: seq[string]               # defs being inlined (Region.owner)
    # File-level state declared with a pair (`$z = [0, 0]`). Each one is
    # two struct fields, `v_<name>__0` and `v_<name>__1`, listed in
    # topVarNames so they migrate across hot reloads by name like any
    # other state.
    pairVars: HashSet[string]
    # Every file-level `$x = init` declaration (top level and play
    # bodies), in source order, for the lazy-init block at tick() start.
    varDecls: seq[Node]
    # def name -> which params its body reads as a pair (see pairParams).
    pairParamCache: Table[string, seq[bool]]
    # File-level state arrays (`$f = array(N)` at top level or in a play)
    # and file-level local arrays (`let a = [p, q, r]`), by name. The
    # state array's slots live in the pool; `base` is its offset
    # placeholder, so any later statement can read or store elements.
    topSarrays: Table[string, tuple[base: string; n: int; wo: int]]
    topLarrays: Table[string, tuple[sym: string; n: int]]
    topSarrayNames: HashSet[string]
    # Inside `each(N, i => ...)`: element stores to state arrays are held
    # here (as C statements) and committed when the each ends, so every
    # body reads the array as it was before the step.
    eachDepth: int
    deferred: string
    # Recursion (notes/tail-recursion-2026-10-16.md): a def that calls
    # itself compiles to a loop at each call site. `recursive` holds
    # those defs; `tailChecked` the ones whose calls to themselves are
    # all repeats. `stereoSeen` / `pureSeen` stop the def walks of
    # isStereoDef / isPureForStereo from following a def into itself.
    recursive: HashSet[string]
    tailChecked: HashSet[string]
    stereoSeen: HashSet[string]
    pureSeen: HashSet[string]

# Native dsp.nim primitives callable from generated C.
# Name -> arity (not counting the state pointer).
const NativeArities = {
  "lp1": 2, "hp1": 2, "lpf": 3, "hpf": 3, "bpf": 3, "notch": 3,
  "delay": 3, "fbdelay": 4, "reverb": 3,
  "impulse": 1, "resonator": 3, "discharge": 2,
  "dho": 3, "dho_v": 3,
  "tremolo": 3, "slew": 2,
  "wave": -1,                      # special: freq + array
}.toTable

# Libm 1-arg pass-throughs.
const Libm1 = ["sin", "cos", "tan", "tanh", "exp", "log", "log2",
               "floor", "ceil", "sqrt"]

# The two entry points, appended after every patch's tick1 (the
# per-sample body above):
#
#   tick(s, outL, outR)        one sample, with s->t set by the caller and
#                              input() from the dsp globals: what render,
#                              try and the tests use, unchanged.
#   tick_block(s, n, outL, outR)   n samples: the engine's path. The whole
#                              per-sample story lives here in C: the
#                              clock (the same second/fraction split the
#                              engine used, so t has the same bits), the
#                              block input, the voice fade and part gain
#                              ramps, park, the part meters, the output.
#                              Non-finite samples are counted and zeroed;
#                              the engine decides per block what to do.
const BlockTemplate = """
extern double n_input_l(void);
extern double n_input_r(void);

void tick(VoiceState* s, double* outL, double* outR) {
  hoist_init(s);
  s->in_l = n_input_l(); s->in_r = n_input_r();
  tick1(s, outL, outR);
}

void tick_block(VoiceState* s, long n, double* outL, double* outR) {
  const long np = s->n_parts;
  hoist_init(s);
  s->bad = 0; s->peak_raw = 0.0; s->stuck = 0;
  for (long i = 0; i < n; i++) {
    s->time_frac += s->dt;
    if (s->time_frac >= 1.0) { s->time_sec += 1.0; s->time_frac -= 1.0; }
    s->t = s->time_sec + s->time_frac;
    if (s->in_buf) { s->in_l = s->in_buf[2*i]; s->in_r = s->in_buf[2*i + 1]; }
    else { s->in_l = 0.0; s->in_r = 0.0; }
    double l, r;
    tick1(s, &l, &r);
    if (!(l == l) || !(r == r) || l > 1e6 || l < -1e6 || r > 1e6 || r < -1e6) {
      s->bad++; l = 0.0; r = 0.0;
    }
    double al = fabs(l), ar = fabs(r);
    if (al > s->peak_raw) s->peak_raw = al;
    if (ar > s->peak_raw) s->peak_raw = ar;
    /* the voice's fade, then the parts' gain ramps: for the next sample */
    s->fade_gain += s->fade_delta;
    if (s->fade_gain > 1.0) s->fade_gain = 1.0;
    if (s->fade_gain < 0.0) s->fade_gain = 0.0;
    if (s->fade_gain >= 1.0 && s->fade_delta > 0.0) s->fade_delta = 0.0;
    for (long p = 0; p < np; p++) {
      double d = s->part_fade_delta[p];
      if (d != 0.0) {
        double target = s->part_fade_target[p];
        double g = s->part_gains[p] + d;
        if ((d > 0.0 && g >= target) || (d < 0.0 && g <= target)) {
          s->part_gains[p] = target; s->part_fade_delta[p] = 0.0;
          if (s->part_park_pending[p] && target <= 0.0) {
            s->part_park[p] = 1.0; s->part_park_pending[p] = 0;
          }
        } else {
          s->part_gains[p] = g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);
        }
      }
    }
    const double g = s->fade_gain;
    outL[i] = l * g; outR[i] = r * g;
    /* per-part meters, after the voice fade like the voice's own */
    for (long p = 0; p < np; p++) {
      double a0 = fabs(s->part_out[2*p]), a1 = fabs(s->part_out[2*p + 1]);
      double a = (a0 > a1 ? a0 : a1) * g;
      double pk = s->part_peak[p] * s->peak_decay;
      s->part_peak[p] = pk > a ? pk : a;
      s->part_sq[p] = s->part_sq[p] * (1.0 - s->rms_alpha) + a * a * s->rms_alpha;
    }
  }
}
"""

# ---- small helpers -----------------------------------------------------

proc newCtx*(program: Node): Ctx =
  result = Ctx(sr: 48000.0)
  # Gather user defs so call sites can inline them.
  for n in program.kids:
    if n.kind == nkDef:
      result.userDefs[n.str] = n
  result.hoistOn = getEnv("AITHER_HOIST", "1") != "0"
  result.hoistSeen = initTable[string, string]()
  proc collectAssigned(n: Node; into: var HashSet[string]) =
    if n == nil: return
    if n.kind in {nkAssign, nkIdxAssign} and n.str.len > 0: into.incl n.str
    for k in n.kids: collectAssigned(k, into)
  collectAssigned(program, result.assigned)

# Fixed-width placeholder token so simple `replace` back-patching stays
# unambiguous regardless of id length (avoids "__OFF_1__" matching a
# prefix of "__OFF_10__").
proc offsetPlaceholder(rid: int): string =
  "__OFF_" & align($rid, 6, '0') & "__"

# Register one stateful call site. Returns a placeholder token to embed
# in the emitted C — substituted with the absolute pool offset after
# the walk, once per-type bases are known.
proc registerRegionInit(c: Ctx; typeName: string; size: int;
                        init: seq[float64] = @[]; stateName = ""): string =
  var owner = c.defStack.join(">")
  if stateName.len > 0: owner.add "/$" & stateName
  if typeName notin c.typeCounter:
    c.typeCounter[typeName] = 0
    c.typeCumSize[typeName] = 0
    c.typeOrder.add typeName
  let perTypeIdx = c.typeCounter[typeName]
  let intraOffset = c.typeCumSize[typeName]
  c.typeCounter[typeName] = perTypeIdx + 1
  c.typeCumSize[typeName] = intraOffset + size
  let rid = c.regions.len
  let scopeKey = c.scope & " | " & typeName & " | " & owner
  let scopeIdx = c.scopeCounter.getOrDefault(scopeKey, 0)
  c.scopeCounter[scopeKey] = scopeIdx + 1
  # `offset` holds the intra-region offset here; `finaliseLayout` rewrites
  # it to an absolute pool slot once region bases are assigned.
  c.regions.add Region(typeName: typeName, perTypeIdx: perTypeIdx,
                       scope: c.scope, scopeIdx: scopeIdx, owner: owner,
                       offset: intraOffset, size: size, init: init)
  offsetPlaceholder(rid)

proc registerRegion(c: Ctx; typeName: string; size: int): string =
  registerRegionInit(c, typeName, size, @[])

proc fresh(c: Ctx; prefix: string): string =
  inc c.tmpCounter
  prefix & "_" & $c.tmpCounter

proc push(parent: Scope): Scope =
  Scope(parent: parent,
        names: initTable[string, string](),
        fns: initTable[string, string](),
        lambdas: initTable[string, Node](),
        stereoNames: initTable[string, tuple[l, r: string]](),
        arrays: initTable[string, tuple[sym: string; length: int]](),
        sarrays: initTable[string, tuple[base: string; n: int; wo: int]](),
        larrays: initTable[string, tuple[sym: string; n: int]](),
        numLits: initTable[string, float64](),
        invs: initTable[string, tuple[node: Node; sc: Scope]]())

proc lookupInv(sc: Scope; name: string): tuple[node: Node; sc: Scope; ok: bool] =
  ## The block-invariant expression `name` is bound to, if the nearest
  ## binding of that name is one (shadowing as in lookupNumLit).
  var s = sc
  while s != nil:
    if name in s.invs: return (s.invs[name].node, s.invs[name].sc, true)
    if name in s.numLits or name in s.names or name in s.stereoNames or
       name in s.fns or name in s.lambdas or name in s.arrays or
       name in s.sarrays or name in s.larrays:
      return (nil, nil, false)
    s = s.parent
  (nil, nil, false)

proc lookupNumLit(sc: Scope; name: string): tuple[val: float64; ok: bool] =
  ## The compile-time number `name` is bound to, if the nearest binding
  ## of that name is one. A nearer scope that binds the name to anything
  ## else (a runtime let, a pair, an array, a function) shadows an outer
  ## literal: `let hz = 800` outside, `let hz = 800 + lfo` inside is not
  ## a constant, however the outer one is.
  var s = sc
  while s != nil:
    if name in s.numLits: return (s.numLits[name], true)
    if name in s.names or name in s.stereoNames or name in s.fns or
       name in s.lambdas or name in s.arrays or name in s.sarrays or
       name in s.larrays:
      return (0.0, false)
    s = s.parent
  (0.0, false)

proc lookupArray(c: Ctx; sc: Scope; name: string):
    tuple[sym: string; length: int; ok: bool] =
  var s = sc
  while s != nil:
    if name in s.arrays:
      let a = s.arrays[name]
      return (a.sym, a.length, true)
    s = s.parent
  if name in c.topArrays:
    let (sym, length) = c.topArrays[name]
    return (sym, length, true)
  ("", 0, false)

proc lookupSarray(c: Ctx; sc: Scope; name: string):
    tuple[base: string; n: int; wo: int; ok: bool] =
  ## `wo` is where stores go past the elements reads see: 0, or for a
  ## stepped array (`every = N`) n, its next generation.
  var s = sc
  while s != nil:
    if name in s.sarrays:
      let a = s.sarrays[name]
      return (a.base, a.n, a.wo, true)
    s = s.parent
  if name in c.topSarrays:
    let a = c.topSarrays[name]
    return (a.base, a.n, a.wo, true)
  ("", 0, 0, false)

proc lookupLarray(c: Ctx; sc: Scope; name: string):
    tuple[sym: string; n: int; ok: bool] =
  var s = sc
  while s != nil:
    if name in s.larrays:
      let a = s.larrays[name]
      return (a.sym, a.n, true)
    s = s.parent
  if name in c.topLarrays:
    let a = c.topLarrays[name]
    return (a.sym, a.n, true)
  ("", 0, false)

# A compile-time number: a literal, a name bound to one, `len(array)`,
# or arithmetic over those. What `sum`, `each`, `array(N)` and the
# literal propagation through lets and params accept, so a def can say
# `let n = len(areas)` and size its state by its argument.
proc constValue(c: Ctx; sc: Scope; n: Node): tuple[val: float64; ok: bool] =
  if n == nil: return (0.0, false)
  case n.kind
  of nkNum: (n.num, true)
  of nkIdent:
    if sc != nil:
      let v = sc.lookupNumLit(n.str)
      if v.ok: return (v.val, true)
    (0.0, false)
  of nkCall:
    if n.str == "len" and n.kids.len == 1 and n.kids[0].kind == nkIdent:
      let a = n.kids[0].str
      let sa = c.lookupSarray(sc, a)
      if sa.ok: return (float64(sa.n), true)
      let la = c.lookupLarray(sc, a)
      if la.ok: return (float64(la.n), true)
      let ca = c.lookupArray(sc, a)
      if ca.ok: return (float64(ca.length), true)
    (0.0, false)
  of nkBinOp:
    let a = c.constValue(sc, n.kids[0])
    let b = c.constValue(sc, n.kids[1])
    if not (a.ok and b.ok): return (0.0, false)
    case n.str
    of "+": (a.val + b.val, true)
    of "-": (a.val - b.val, true)
    of "*": (a.val * b.val, true)
    of "/": (if b.val != 0.0: (a.val / b.val, true) else: (0.0, false))
    else: (0.0, false)
  of nkUnary:
    if n.str == "-":
      let a = c.constValue(sc, n.kids[0])
      if a.ok: return (-a.val, true)
    (0.0, false)
  else: (0.0, false)

# `array(N)` / `array(N, init)`: the right side of a state or local
# array declaration.
proc arrayArgs(call: Node): tuple[pos: seq[Node]; every: Node] =
  ## `array(n, init, every)`: size and init, and `every` (nil when not
  ## given). Named arguments were made positional by normalizeCalls.
  for i, k in call.kids:
    if i < 2: result.pos.add k else: result.every = k

proc isArrayMake(n: Node): bool =
  n != nil and n.kind == nkCall and n.str == "array" and n.kids.len in {1, 2, 3}

proc lookupStereo(sc: Scope; name: string): tuple[l, r: string, ok: bool] =
  var s = sc
  while s != nil:
    if name in s.stereoNames:
      let p = s.stereoNames[name]
      return (p.l, p.r, true)
    s = s.parent
  ("", "", false)

proc lookupFn(sc: Scope; name: string): string =
  var s = sc
  while s != nil:
    if name in s.fns: return s.fns[name]
    s = s.parent
  ""

proc lookupLambda(sc: Scope; name: string): Node =
  ## Walk the scope chain for a lambda binding. Returns nil if the name
  ## isn't bound to a lambda — caller falls through to other resolution
  ## paths (named def, builtin, scalar local).
  var s = sc
  while s != nil:
    if name in s.lambdas: return s.lambdas[name]
    s = s.parent
  nil

proc lookup(sc: Scope; name: string): string =
  var s = sc
  while s != nil:
    if name in s.names: return s.names[name]
    s = s.parent
  ""

# True when the nearest binding of `name` in the scope chain is a plain
# number: a def param or local let that shadows a file-level pair of
# the same name (`let s = [l, r]` at the top, `adsr(gate, a, d, s, r)`
# in stdlib). File-level names are consulted only when no scope binds it.
proc scalarShadows(sc: Scope; name: string): bool =
  var s = sc
  while s != nil:
    if name in s.stereoNames: return false
    if name in s.names: return true
    s = s.parent
  false

# Builtins callable by name from user code that aren't keywords or ops:
# libm1, shape primitives, stateful inlined builtins, math helpers, MIDI.
const MaxProfSlots* = 1024   # `aither profile`: top-level statements timed, at most

const BuiltinFns = ["saw", "tri", "sqr", "phasor", "noise", "abs", "pow",
                    "midi_cc", "midi_note", "midi_freq", "midi_gate",
                    "midi_trig", "midi_voice_freq", "midi_voice_gate",
                    "midi_voice_vel", "midi_voice_key", "midi_vel",
                    "magnitude", "phase", "freq_shift",
                    "array_make", "array_get", "array_set", "array_len",
                    "array_push", "array_pop", "array_resize"]

# Pair-returning builtins. Each emits two C expressions — one for the
# real component, one for the imaginary. They piggy-back on the stereo
# machinery (refsStereo / emitStereo) because aither already represents
# two-valued returns as L/R-style pairs; "stereo" here means
# "two-component", not literally "speaker channels". Composers index with
# `out[0]` (real) / `out[1]` (imag), the same way they index a pan output
# for L/R. Don't add type-system surgery; the value stays flat.
#
# Operands may be pairs or plain numbers in any mix — `cmul(p, q)` is
# `cmul(p[0], p[1], q[0], q[1])` — see pairOperands. The operation, not
# the value, decides that two cells are a complex number.
const PairCalls = ["cmul", "cdiv", "cscale", "rotate", "analytic",
                   "phasor_pair", "conj", "input"]

# Scalar reductions of a pair: one pair (or its two numbers) in, one
# number out.
const PairReaders = ["magnitude", "phase"]

# Whether argument j of a pair builtin (called with n arguments) is a
# pair: the angle of rotate, the factor of cscale, analytic's signal and
# phasor_pair's frequency are numbers, so a def parameter passed there
# stays one (`def turn(th): rotate([1, 0], th)`).
proc takesPairAt(name: string; j, n: int): bool =
  case name
  of "rotate": not (n == 2 and j == 1)
  of "cscale": not (n == 2 and j == 0)
  of "analytic", "phasor_pair", "input": false
  else: true

# Transparent helpers — process a signal without intentionally creating
# L/R difference. Stereo input maps to stereo output by re-emitting the
# call once per channel; each emission registers its own state region(s)
# so the two channels filter / delay / envelope independently. Mono input
# maps to mono output with a single state allocation (current behavior).
#
# Why an allowlist and not "any stateful call": pan / haas / phasor /
# noise / dho / freq_shift / etc. either create stereo from mono, are
# semantically scalar-only, or have call-site identity that doubles
# pool usage in surprising ways. Listing the helpers whose lift IS
# semantically transparent keeps the rest of the language unchanged.
#
# Mix of natives (lpf/hpf/.../delay/...) and pure-aither defs (drive/
# fold/gain/pluck/swell/adsr/prev) — codegen treats them uniformly via
# the call-split path: each channel call recurses through emitExpr,
# which registers fresh regions for natives and re-inlines stdlib defs
# (whose own state slots are claimed per call site).
const TransparentHelpers = ["lpf", "hpf", "bpf", "lp1", "hp1",
                            "drive", "fold",
                            "delay", "fbdelay", "reverb",
                            "slew", "discharge",
                            "pluck", "swell", "adsr",
                            "gain", "prev", "tremolo"]

# Stereo-returning defs that semantically REQUIRE mono input — pan takes
# a mono signal and places it. Calling it with stereo input would
# otherwise descend into the body and surface a misleading "unknown
# identifier: signal" because emitStereo's nkArr branch emits each
# component via emitExpr (scalar), which can't resolve a stereo-bound
# param. Diagnose at the call site instead. (haas takes a pair now: it
# delays one side of whatever it is given.)
const MonoOnlyInputDefs = ["pan"]

# True when `name` resolves to a value (scalar local, stereo local, top
# lets/vars/arrays, play block, stereo let). Values shadow function
# names, so this short-circuits isFnName below.
proc isValueName(c: Ctx; sc: Scope; name: string): bool =
  if sc.lookup(name).len > 0: return true
  if sc.lookupStereo(name).ok: return true
  if c.lookupArray(sc, name).ok: return true
  if c.lookupSarray(sc, name).ok or c.lookupLarray(sc, name).ok: return true
  if name in c.topLets: return true
  if name in c.topVarSet: return true
  if name in c.pairVars: return true
  if name in c.playSet: return true
  if name in c.stereoLets: return true
  false

# True when `name` is a function-valued identifier (for function-valued
# args to user defs, and for spotting function-idents during stereo
# emission). Unifies what used to be three near-identical predicates.
# Shadowing: a local/top value with the same name wins, so isFnName
# returns false if `name` already resolves to a value — `let swell =
# 0.5` must not be mistaken for a ref to stdlib's `def swell`.
proc isFnName(c: Ctx; sc: Scope; name: string): bool =
  if isValueName(c, sc, name): return false
  name in Libm1 or name in BuiltinFns or
  name in c.userDefs or NativeArities.hasKey(name) or
  sc.lookupFn(name).len > 0

# Reference to one channel of a play block or stereo let, as a C lvalue.
proc stereoRef(c: Ctx; name, chan: string): string =
  let prefix = if name in c.playSet: "p_" else: "l_"
  prefix & name & "_" & chan

# Struct field holding one side (k = 0 or 1) of file-level pair state.
proc pairVarField(name: string; k: int): string = name & "__" & $k
proc pairVarRef(name: string; k: int): string = "s->v_" & pairVarField(name, k)

# A def takes a param as a pair when its body reads it as one: indexes
# it with a constant `p[0]` / `p[1]`, hands it straight to a pair
# operation (`cmul(p, q)`, `magnitude(p)`), passes it on to another
# def that takes a pair there, or does arithmetic with it and a pair
# (`cmul(z, z) + c`). A `def rec` that passes a pair to itself takes a
# pair there. Such a param is bound whole, so `def mono(s): (s[0] +
# s[1]) * 0.5` sees both channels instead of being run once per
# channel. A plain number passed to a pair param fills both sides.
proc pairParams(c: Ctx; def: Node): seq[bool] =
  if def.str in c.pairParamCache: return c.pairParamCache[def.str]
  var res = newSeq[bool](def.params.len)
  c.pairParamCache[def.str] = res          # guard against call cycles
  # A pair, as far as the body shows so far: a param marked one, a
  # literal pair, a pair operation, or arithmetic on one of those.
  proc isPair(res: seq[bool]; params: seq[string]; n: Node): bool =
    if n == nil: return false
    case n.kind
    of nkIdent:
      let k = params.find(n.str)
      k >= 0 and res[k]
    of nkArr: n.kids.len == 2
    of nkCall: n.str in PairCalls
    of nkBinOp:
      n.str in ["+", "-", "*", "/"] and n.kids.anyIt(isPair(res, params, it))
    of nkUnary: n.str == "-" and isPair(res, params, n.kids[0])
    else: false
  proc mark(c: Ctx; res: var seq[bool]; params: seq[string]; self: string;
            n: Node) =
    if n == nil: return
    case n.kind
    of nkIdx:
      let a = n.kids[0]; let i = n.kids[1]
      if a.kind == nkIdent and i.kind == nkNum and int(i.num) in {0, 1}:
        let k = params.find(a.str)
        if k >= 0: res[k] = true
    of nkBinOp:
      if n.str in ["+", "-", "*", "/"] and n.kids.anyIt(isPair(res, params, it)):
        for x in n.kids:
          if x.kind == nkIdent:
            let k = params.find(x.str)
            if k >= 0: res[k] = true
    of nkCall:
      let callee =
        if n.str in c.userDefs and n.str notin PairCalls and
           n.str notin PairReaders: c.pairParams(c.userDefs[n.str])
        else: @[]
      for j, arg in n.kids:
        if n.str == self and j < res.len and isPair(res, params, arg):
          res[j] = true
        if arg.kind != nkIdent: continue
        let k = params.find(arg.str)
        if k < 0: continue
        if ((n.str in PairCalls or n.str in PairReaders) and takesPairAt(n.str, j, n.kids.len)) or
           (j < callee.len and callee[j]):
          res[k] = true
    else: discard
    for k in n.kids: mark(c, res, params, self, k)
  # A mark can make more arithmetic a pair, so walk until nothing changes.
  while true:
    let before = res
    mark(c, res, def.params, def.str, def.kids[0])
    c.pairParamCache[def.str] = res
    if res == before: break
  res

proc hasPairParams(c: Ctx; name: string): bool =
  name in c.userDefs and c.pairParams(c.userDefs[name]).anyIt(it)

proc errLoc(n: Node): string =
  # Format the node's source location for an error message. Prefers
  # "<source>:<line>" when the source tag is set (stdlib / patch path),
  # falling back to "line <n>" for nodes that pre-date the source-tagging
  # plumbing or were synthesised without a file origin.
  if n.source.len > 0 and n.line > 0: "(" & n.source & ":" & $n.line & ")"
  elif n.line > 0:                    "(line " & $n.line & ")"
  else:                               ""

proc numLit(v: float64): string =
  # Emit with enough digits that round-trip is exact; append dot if
  # integer-valued so the literal type is double.
  let s = formatFloat(v, ffDefault, 17)
  if '.' in s or 'e' in s or 'E' in s: s else: s & ".0"

# ---- expression codegen ------------------------------------------------

proc emitExpr(c: Ctx; sc: Scope; n: Node): string
proc emitBlockExpr(c: Ctx; sc: Scope; blk: Node): string
proc refsStereo(c: Ctx; sc: Scope; n: Node): bool
proc emitIdxAssign(c: Ctx; sc: Scope; st: Node): string
proc declareStateArray(c: Ctx; sc: Scope; st: Node; pre: var string;
                       topLevel = false)
proc declareLocalArray(c: Ctx; sc: Scope; name: string; rhs: Node;
                       pre: var string; topLevel = false)

# Stereo-aware emission. Returns (l, r, stereo) where stereo=false means
# l == r (the caller may collapse to a single emit). `pre` accumulates
# any C statements that must execute before l/r are referenced (used
# when a stereo-returning user def must be inlined once and its two
# channels captured into fresh temps — the inlining can't fit inside a
# single expression).
type StereoVal = tuple[l, r: string, stereo: bool]
proc emitStereo(c: Ctx; sc: Scope; n: Node; pre: var string): StereoVal

# Does this expression structurally return a stereo value? True when the
# final expression (all branches of nested ifs / blocks) is an nkArr of
# length 2 or a call to another stereo-returning def.
proc isStereoReturn(c: Ctx; n: Node): bool

# Inside a def body, `names` are the names known to hold a pair: its
# pair params (see pairParams) and the lets bound to a pair so far. A
# result that is one of them, or arithmetic on one, is a pair too, so
# `def norm(z): z / magnitude(z)` and a recursion's `then z` return
# pairs.
proc stereoIn(c: Ctx; n: Node; names: HashSet[string]): bool =
  if n == nil: return false
  case n.kind
  of nkIdent: n.str in names
  of nkBinOp, nkUnary: n.kids.anyIt(c.stereoIn(it, names))
  of nkBlock:
    var names = names
    for i, k in n.kids:
      if i == n.kids.len - 1: return c.stereoIn(k, names)
      if k.kind == nkLet:
        if c.stereoIn(k.kids[0], names): names.incl k.str
        else: names.excl k.str
    false
  of nkIf:
    c.stereoIn(n.kids[1], names) or c.stereoIn(n.kids[2], names)
  else: c.isStereoReturn(n)

# A def that calls itself is walked once: its repeat says nothing about
# the result, its other branches decide.
proc isStereoDef(c: Ctx; def: Node): bool =
  if def.str in c.stereoSeen: return false
  c.stereoSeen.incl def.str
  defer: c.stereoSeen.excl def.str
  var names = initHashSet[string]()
  let pairs = c.pairParams(def)
  for i, p in def.params:
    if pairs[i]: names.incl p
  c.stereoIn(def.kids[0], names)

proc isStereoReturn(c: Ctx; n: Node): bool =
  if n == nil: return false
  case n.kind
  of nkArr: n.kids.len == 2
  of nkBlock:
    if n.kids.len == 0: false
    else: isStereoReturn(c, n.kids[^1])
  of nkIf:
    isStereoReturn(c, n.kids[1]) or
      (n.kids[2] != nil and isStereoReturn(c, n.kids[2]))
  of nkCall:
    if n.str in PairCalls: true
    else: n.str in c.userDefs and isStereoDef(c, c.userDefs[n.str])
  of nkLambda: false      # lambdas are scalar-valued in v1
  else: false

# Walk a user def body to decide whether it can be safely inlined twice
# (once per channel) in stereo context. Stateful calls (phasor, noise,
# native dsp) and `var` bindings make the def unsafe to duplicate.
proc isPureForStereo(c: Ctx; n: Node): bool =
  if n == nil: return true
  case n.kind
  of nkVar: return false
  of nkCall:
    # sum() unrolls at codegen into N textual call sites; if the lambda
    # body contains stateful primitives, duplicating the whole sum() for
    # both channels would claim twice the pool slots and desync L/R
    # state. Treat sum() as stateful for stereo-inlining purposes.
    if n.str in ["sum", "each", "array"]: return false
    if n.str in ["phasor", "phasor_pair", "noise", "midi_trig"]: return false
    if NativeArities.hasKey(n.str): return false
    if n.str in c.userDefs and n.str notin c.pureSeen:
      c.pureSeen.incl n.str
      let pure = isPureForStereo(c, c.userDefs[n.str].kids[0])
      c.pureSeen.excl n.str
      if not pure: return false
  of nkLambda: return false      # conservative: lambda bodies may be stateful
  else: discard
  for k in n.kids:
    if k != nil and not isPureForStereo(c, k): return false
  true

proc callArgs(c: Ctx; sc: Scope; kids: openArray[Node]): string =
  var parts: seq[string] = @[]
  for k in kids: parts.add c.emitExpr(sc, k)
  parts.join(", ")

# Call a stateful native at its call site's pool region. The args are
# evaluated into locals FIRST: an arg can hold another native call
# (`x |> bpf(..) |> hpf(..)`) that moves s->idx, so s->idx is pointed
# at this site's region only after they have all run. `call` is the
# native call with `$1`, `$2`, ... standing for the evaluated args.
# ---- hoisting ----------------------------------------------------------
# An expression that can't change from one sample to the next: a
# number, PI/TAU/sr, a name bound to a compile-time number, a knob
# (`midi_cc(k)`: hoist_init runs at the start of every block, so a knob
# is read once a block, and a knob moved mid-block lands at the next,
# within 11 ms at 512 frames), a name bound to any of these, arithmetic
# and pure math over those. A native whose coefficient arguments are
# invariant gets its coefficients computed once, in hoist_init, and
# calls the `_c` step per sample (lp1's exp, the SVF's tan, reverb's
# pows, phasor's division). Same arithmetic once instead of every
# sample, so the same bits. AITHER_HOIST=0 turns it off.
const PureMath = ["sin", "cos", "tan", "tanh", "exp", "log", "log2",
                  "floor", "ceil", "sqrt", "pow", "min", "max", "clamp",
                  "abs", "int"]

proc isInvariant(c: Ctx; sc: Scope; n: Node): bool =
  if n == nil: return false
  case n.kind
  of nkNum: true
  of nkIdent:
    n.str in ["PI", "TAU", "sr"] or
      (sc != nil and (sc.lookupNumLit(n.str).ok or
                      (n.str notin c.assigned and sc.lookupInv(n.str).ok)))
  of nkBinOp, nkUnary:
    n.kids.allIt(c.isInvariant(sc, it))
  of nkIf:
    n.kids.len == 3 and n.kids.allIt(c.isInvariant(sc, it))
  of nkCall:
    (n.str in PureMath or n.str == "midi_cc") and n.kids.len > 0 and
      n.kids.allIt(c.isInvariant(sc, it))
  else: false

proc noteInvariant(c: Ctx; target: Scope; name: string; expr: Node; exprSc: Scope) =
  ## Record that `name` is bound to a block-invariant expression, unless
  ## it is a plain number (numLits has that) or hoisting is off.
  proc mentions(n: Node): bool =
    n != nil and ((n.kind == nkIdent and n.str == name) or n.kids.anyIt(mentions(it)))
  # `let x = x * 2` reads an outer x; spelling it out would loop. Only a
  # let can do that: a parameter's argument comes from the caller's scope
  # (`def f(u)` called as `f(u)` is fine).
  if c.hoistOn and expr != nil and expr.kind != nkNum and
     not (target == exprSc and mentions(expr)) and
     c.isInvariant(exprSc, expr) and not c.constValue(exprSc, expr).ok:
    target.invs[name] = (expr, exprSc)

# Emit an invariant expression for hoist_init, where no per-sample local
# exists: every name bound to a compile-time number is spelled as that
# number. Reuses emitExpr so operators mean the same.
proc emitInvariant(c: Ctx; sc: Scope; n: Node): string =
  # A copy of `n` in which every name is spelled out: a compile-time
  # number as that number, a name bound to an invariant expression as
  # that expression (from its own scope, recursively). What is left is
  # numbers, PI/TAU/sr and midi_cc, which mean the same in hoist_init.
  proc spell(sc: Scope; n: Node): Node =
    if n == nil: return nil
    if n.kind == nkIdent and n.str notin ["PI", "TAU", "sr"] and sc != nil:
      let v = sc.lookupNumLit(n.str)
      if v.ok: return Node(kind: nkNum, num: v.val, line: n.line, source: n.source)
      let inv = sc.lookupInv(n.str)
      if inv.ok: return spell(inv.sc, inv.node)
    result = Node(kind: n.kind, str: n.str, num: n.num, line: n.line,
                  source: n.source, params: n.params, defaults: n.defaults)
    for k in n.kids: result.kids.add spell(sc, k)
  let was = c.inHoist
  c.inHoist = true
  defer: c.inHoist = was
  c.emitExpr(push(nil), spell(sc, n))

proc hoist(c: Ctx; cexpr: string): string =
  ## A static double computed once from `cexpr`; its name.
  if cexpr in c.hoistSeen: return c.hoistSeen[cexpr]
  let name = "h_" & $c.hoists.len
  c.hoists.add (decl: "static double " & name & ";\n",
                init: "  " & name & " = (" & cexpr & ");\n")
  c.hoistSeen[cexpr] = name
  name

proc hoistArr(c: Ctx; n: int; initCall: string): string =
  ## A static array of n doubles filled once by `initCall`, in which $H
  ## stands for the array; its name.
  if initCall in c.hoistSeen: return c.hoistSeen[initCall]
  let name = "h_" & $c.hoists.len
  c.hoists.add (decl: "static double " & name & "[" & $n & "];\n",
                init: "  " & initCall.replace("$H", name) & ";\n")
  c.hoistSeen[initCall] = name
  name

proc nativeAt(c: Ctx; off: string; args: openArray[string]; call: string): string =
  var pre = ""
  var names: seq[string] = @[]
  for a in args:
    let t = c.fresh("na")
    pre.add &"double {t} = ({a}); "
    names.add t
  "(({ " & pre & "s->idx = " & off & "; " & call % names & "; }))"

# An argument bound at compile time rather than evaluated each sample:
# a lambda, a function, or an array. Binds `p` in `inner` and says
# so; false leaves `a` to be evaluated as a value.
proc bindByName(c: Ctx; sc, inner: Scope; p: string; a: Node;
                preface: var string): bool =
  # Lambda-valued arg: either a literal `(a, b) => ...` at the call
  # site, or an ident that already resolves to a lambda in scope (the
  # def-of-def case where one def passes its own lambda param to
  # another). Bind in inner.lambdas so callees of `p(...)` inline the
  # body. Checked before isFnName because a lambda doesn't satisfy
  # isFnName but must take the function-arg slot all the same.
  if a.kind == nkLambda:
    inner.lambdas[p] = a
    return true
  if a.kind == nkIdent:
    let lam = sc.lookupLambda(a.str)
    if lam != nil:
      inner.lambdas[p] = lam
      return true
  # Function-valued arg: a bare ident naming a known builtin/def. Bind
  # in inner.fns so calls to `p(...)` resolve to the concrete function.
  # isFnName respects shadowing — a local `let saw = ...` (or any other
  # value binding) wins over a same-named stdlib def, so this branch
  # won't steal a scalar arg from the caller.
  if a.kind == nkIdent and isFnName(c, sc, a.str):
    let resolved = if sc.lookupFn(a.str).len > 0: sc.lookupFn(a.str)
                   else: a.str
    inner.fns[p] = resolved
    return true
  # Array arg: bound by name, never copied. A def that indexes its
  # param or takes len(param) sees the caller's array (a table, this
  # sample's local array, or a state array), and len() folds at this
  # call site, so one def serves any length and sizes its own state
  # by its argument. Writes land in the caller's array.
  if a.kind == nkIdent and sc.lookup(a.str).len == 0 and
     not sc.lookupStereo(a.str).ok:
    let sa = c.lookupSarray(sc, a.str)
    if sa.ok:
      inner.sarrays[p] = (sa.base, sa.n, sa.wo)
      return true
    let la = c.lookupLarray(sc, a.str)
    if la.ok:
      inner.larrays[p] = (la.sym, la.n)
      return true
    let ca = c.lookupArray(sc, a.str)
    if ca.ok:
      inner.arrays[p] = (ca.sym, ca.length)
      return true
  if a.kind == nkArr and a.kids.len != 2:
    # An array written at the call: a table when it is all numbers,
    # else this sample's values, built in the caller's scope. (Two
    # numbers are a pair; a two-entry table goes through a let.)
    if a.kids.len > 0 and a.kids.allIt(it.kind == nkNum):
      let sym = c.fresh("arr_arg_" & p)
      var items: seq[string] = @[]
      for k in a.kids: items.add numLit(k.num)
      c.arrayDecls.add &"static const double {sym}[{a.kids.len}] = {{" &
        items.join(", ") & "};\n"
      inner.arrays[p] = (sym, a.kids.len)
    else:
      let tmp = c.fresh("arg_" & p)
      var sub = ""
      c.declareLocalArray(sc, tmp, a, sub)
      preface.add sub
      inner.larrays[p] = sc.larrays[tmp]
    return true
  false

# A def reached again while it is still being inlined has come back to
# itself through another def (f -> g -> f). Only a def calling itself
# repeats, so this can't compile yet.
proc cycleError(c: Ctx; def, call: Node): ref ValueError =
  let chain = c.defStack[c.defStack.find(def.str) .. ^1] & @[def.str]
  newException(ValueError,
    chain.join(" -> ") & ": " & def.str & " comes back to itself " &
    "through another def " & errLoc(call) & ". A def can repeat by " &
    "calling itself, but a cycle through other defs can't repeat yet")

# Inline a user def call. Substitute param names with the evaluated C
# expressions (bound to fresh locals to avoid re-evaluating side effects),
# emit let/var handling inside a statement expression.
proc indexes(n: Node; name: string): bool =
  ## Does `n` index `name` anywhere (`name[...]`)?
  if n == nil: return false
  if n.kind == nkIdx and n.kids[0].kind == nkIdent and n.kids[0].str == name:
    return true
  for k in n.kids:
    if indexes(k, name): return true
  false

proc takesPair(c: Ctx; def: Node): seq[bool] =
  ## Which params take a pair argument whole: those the body reads as a
  ## pair (pairParams), and those it indexes, since a pair indexes like a
  ## two-entry table. A pair handed to any other param makes the call
  ## run once per channel.
  result = c.pairParams(def)
  for i, p in def.params:
    if indexes(def.kids[0], p): result[i] = true

proc emitDefInline(c: Ctx; sc: Scope; def: Node; call: Node): string =
  let args = call.kids
  if args.len != def.params.len:
    raise newException(ValueError,
      "wrong arg count for " & def.str & ": expected " &
      $def.params.len & " got " & $args.len & " " & errLoc(call))
  if def.str in c.defStack: raise c.cycleError(def, call)
  let inner = push(sc)
  var preface = ""
  let pairs = c.pairParams(def)
  for i, p in def.params:
    let a = args[i]
    # Pair param (see pairParams): bind both sides. emitStereo's
    # statements are C declarations, legal inside the `({ ... })` below.
    # So is a param the body indexes when this call hands it a pair: a
    # pair indexes like a two-entry table (p[i] wraps at 2), so a def
    # that reads `tbl[i]` takes `[220, 330]` as it takes `[220, 330, 440]`.
    if pairs[i] or (indexes(def.kids[0], p) and c.refsStereo(sc, a)):
      var sub = ""
      let sv = c.emitStereo(sc, a, sub)
      let tmpL = c.fresh("p_" & p & "_l")
      let tmpR = c.fresh("p_" & p & "_r")
      preface.add sub
      preface.add &"double {tmpL} = ({sv.l}); double {tmpR} = ({sv.r}); "
      inner.stereoNames[p] = (tmpL, tmpR)
      continue
    if c.bindByName(sc, inner, p, a, preface): continue
    # Scalar arg: evaluate in caller scope, bind to fresh local.
    let argC = c.emitExpr(sc, a)
    let tmp = c.fresh("p_" & p)
    preface.add &"double {tmp} = ({argC}); "
    inner.names[p] = tmp
    # Literal propagation: if the arg is a compile-time number (a
    # literal, a name that resolves to one, len(array), arithmetic over
    # those), remember the value alongside the scalar-double binding so
    # builtins that require compile-time constants (sum, each, array)
    # can see through one or more def-param hops. The scalar-double
    # binding above remains authoritative for every other use.
    let cv = c.constValue(sc, a)
    if cv.ok: inner.numLits[p] = cv.val
    else: c.noteInvariant(inner, p, a, sc)
  # def.kids[0] is the body (single expression or nkBlock of stmts).
  # The def's name owns every state region its body claims.
  let body = def.kids[0]
  c.defStack.add def.str
  let bodyC =
    if body.kind == nkBlock: c.emitBlockExpr(inner, body)
    else: c.emitExpr(inner, body)
  discard c.defStack.pop()
  "(({ " & preface & "double __r = (" & bodyC & "); __r; }))"

# Evaluate `n` where one number is needed. An expression that mentions
# a pair but reduces to one number (`mono(st)`, `magnitude(z)`) goes
# through emitStereo, whose statements land in `pre`; a real pair here
# is an error that says which side to pick.
proc scalarValue(c: Ctx; sc: Scope; n: Node; pre: var string;
                 what: string): string =
  if not c.refsStereo(sc, n):
    return c.emitExpr(sc, n)
  let v = c.emitStereo(sc, n, pre)
  if v.stereo:
    raise newException(ValueError,
      what & " needs one number but gets a pair — pick a side with " &
      "[0] / [1], or make it pair state: $x = [0, 0] " & errLoc(n))
  v.l

# Operands of a pair operation, flattened to numbers: a pair operand
# gives its two sides, a number gives itself, and each is evaluated
# once into a temp. `cmul(p, q)`, `cmul(p, 0.5, -1)` and `cmul(1, 0, q)`
# all flatten to four numbers.
proc pairOperands(c: Ctx; sc: Scope; n: Node; pre: var string;
                  want: int; usage: string): seq[string] =
  for k in n.kids:
    if c.refsStereo(sc, k):
      let v = c.emitStereo(sc, k, pre)
      let tmpL = c.fresh(n.str & "_a")
      pre.add &"  double {tmpL} = ({v.l});\n"
      result.add tmpL
      if v.stereo:
        let tmpR = c.fresh(n.str & "_a")
        pre.add &"  double {tmpR} = ({v.r});\n"
        result.add tmpR
    else:
      let tmp = c.fresh(n.str & "_a")
      pre.add &"  double {tmp} = ({c.emitExpr(sc, k)});\n"
      result.add tmp
  if result.len != want:
    raise newException(ValueError,
      n.str & " takes " & usage & ", got " & $result.len &
      (if result.len == 1: " number " else: " numbers ") & errLoc(n))

# `target = rhs` for a pair target. Both sides are computed into temps
# before either is stored, so `$z = [$z[1], $z[0]]` swaps rather than
# reading a half-updated pair. A plain number fills both sides.
proc assignPair(c: Ctx; sc: Scope; target: tuple[l, r: string];
                rhs: Node): string =
  var pre = ""
  let v = c.emitStereo(sc, rhs, pre)
  let tmpL = c.fresh("pa_l")
  let tmpR = c.fresh("pa_r")
  pre & &"  double {tmpL} = ({v.l});\n  double {tmpR} = ({v.r});\n" &
    &"  {target.l} = {tmpL};\n  {target.r} = {tmpR};\n"

# `$z[0] = expr` / `$z[1] = expr`: store one side of a pair.
proc assignPairSide(c: Ctx; sc: Scope; st: Node): string =
  let base = st.kids[0]
  let idx = st.kids[1]
  if base.kind == nkIdent and idx.kind == nkNum and int(idx.num) in {0, 1}:
    let k = int(idx.num)
    var target = ""
    let pair = sc.lookupStereo(base.str)
    if pair.ok: target = (if k == 0: pair.l else: pair.r)
    elif base.str in c.pairVars: target = pairVarRef(base.str, k)
    if target.len > 0:
      var pre = ""
      let rhs = c.scalarValue(sc, st.kids[2], pre,
                              "$" & base.str & "[" & $k & "]")
      return pre & &"  {target} = ({rhs});\n"
  raise newException(ValueError,
    "only one side of pair state can be assigned by index " &
    "($z[0] = ... or $z[1] = ...) " & errLoc(st))

# The size of `array(N, init)`: N must be a compile-time integer.
proc arrayMakeSize(c: Ctx; sc: Scope; call: Node): int =
  let nArg = call.kids[0]
  let cv = c.constValue(sc, nArg)
  if not cv.ok:
    raise newException(ValueError,
      "array: the size must be a compile-time integer " & errLoc(nArg))
  let nVal = cv.val
  let n = int(nVal)
  if float64(n) != nVal or n < 1 or n > 1_000_000:
    raise newException(ValueError,
      "array: the size must be a whole number from 1 to 1000000, got " &
      $nVal & " " & errLoc(nArg))
  n

# `$f = array(N, init)`: a state array. One "sarr" region of 1 + N pool
# slots: an inited flag, then the elements. On the first sample every
# element is set to `init` (0 when omitted); after that the slots keep
# their values across samples and hot reloads (same name, def and
# size). Elements are read with `$f[i]` and stored with `$f[i] = v`;
# see `each` for the simultaneous step.
#
# `$f = array(N, init, every = M)`: a stepped state array. Reads see the
# elements as they were at the last step; stores go to the next
# generation and land together every M samples of the engine clock,
# aligned with tv's frames when M is w·h (a step begins where
# pixel_index is 0). `each`'s held-back stores, held back for M samples.
# The region holds the flag, then the elements reads see, then the next
# generation, which starts each step as a copy, so an element nobody
# stored keeps its value.
proc declareStateArray(c: Ctx; sc: Scope; st: Node; pre: var string;
                       topLevel = false) =
  let call = st.kids[0]
  let args = arrayArgs(call)
  let n = c.arrayMakeSize(sc, call)
  var every = 0                          # 0: an ordinary state array
  if args.every != nil:
    let cv = c.constValue(sc, args.every)
    if not cv.ok or float64(int(cv.val)) != cv.val or cv.val < 0:
      raise newException(ValueError,
        "array: every must be a whole number of samples (0 for none), " &
        "known when the patch compiles " & errLoc(args.every))
    every = int(cv.val)
  let gens = if every > 0: 2 else: 1
  let off = c.registerRegionInit("sarr", 1 + gens * n, stateName = st.str)
  var initPre = ""
  let initC =
    if args.pos.len == 2: c.scalarValue(sc, args.pos[1], initPre, "$" & st.str)
    else: "0.0"
  let wo = if every > 0: n else: 0
  if topLevel:
    # Readable from every later statement through its offset placeholder.
    c.topSarrays[st.str] = (off, n, wo)
  else:
    let base = c.fresh("sa")
    pre.add &"  long {base} = {off};\n"
    sc.sarrays[st.str] = (base, n, wo)
  let base = if topLevel: off else: sc.sarrays[st.str].base
  pre.add &"  if (s->pool[{base}] == 0.0) {{\n" & initPre &
          &"    double _v = ({initC});\n" &
          &"    for (long _k = 0; _k < {gens * n}; ++_k) s->pool[{base} + 1 + _k] = _v;\n" &
          &"    s->pool[{base}] = 1.0;\n  }}\n"
  if every > 0:
    pre.add &"  if (((long)floor(s->t * s->sr + 0.5) - 1) % {every} == 0)\n" &
            &"    for (long _k = 0; _k < {n}; ++_k) " &
            &"s->pool[{base} + 1 + _k] = s->pool[{base} + 1 + {n} + _k];\n"

# `let a = [e1, ..., eN]` with runtime values, or `let a = array(N, init)`:
# a per-sample local array, a plain C array of this sample's numbers.
proc declareLocalArray(c: Ctx; sc: Scope; name: string; rhs: Node;
                       pre: var string; topLevel = false) =
  let sym = c.fresh("la_" & name)
  if rhs.kind == nkArr:
    pre.add &"  double {sym}[{rhs.kids.len}];\n"
    for k, e in rhs.kids:
      let v = c.scalarValue(sc, e, pre, name & "[" & $k & "]")
      pre.add &"  {sym}[{k}] = ({v});\n"
    if topLevel: c.topLarrays[name] = (sym, rhs.kids.len)
    else: sc.larrays[name] = (sym, rhs.kids.len)
  else:
    let args = arrayArgs(rhs)
    if args.every != nil:
      raise newException(ValueError,
        "every steps a state array, which keeps its values: $" & name &
        " = array(...), not let " & errLoc(rhs))
    let n = c.arrayMakeSize(sc, rhs)
    let initC =
      if args.pos.len == 2: c.scalarValue(sc, args.pos[1], pre, name)
      else: "0.0"
    pre.add &"  double {sym}[{n}];\n" &
            &"  {{ double _v = ({initC}); " &
            &"for (long _k = 0; _k < {n}; ++_k) {sym}[_k] = _v; }}\n"
    if topLevel: c.topLarrays[name] = (sym, n)
    else: sc.larrays[name] = (sym, n)

# `name[i] = v`: an element store. A state array inside `each` takes the
# store at the end of the step (see each); elsewhere it lands at once. A
# local array cycles its index like a read does. Anything else is a pair
# side (`$z[0] = ...`).
proc emitIdxAssign(c: Ctx; sc: Scope; st: Node): string =
  let base = st.kids[0]
  if base.kind == nkIdent:
    let sa = c.lookupSarray(sc, base.str)
    if sa.ok:
      var pre = ""
      let ix = c.emitExpr(sc, st.kids[1])
      let v = c.scalarValue(sc, st.kids[2], pre, "$" & base.str & "[i]")
      let it = c.fresh("ai")
      let vt = c.fresh("av")
      pre.add &"  long {it} = (long)({ix}); double {vt} = ({v});\n"
      let store = &"  if ({it} >= 0 && {it} < {sa.n}) " &
                  &"s->pool[{sa.base} + 1 + {sa.wo} + {it}] = {vt};\n"
      if c.eachDepth > 0:
        c.deferred.add store
        return pre
      return pre & store
    let la = c.lookupLarray(sc, base.str)
    if la.ok:
      var pre = ""
      let ix = c.emitExpr(sc, st.kids[1])
      let v = c.scalarValue(sc, st.kids[2], pre, base.str & "[i]")
      return pre & &"  {{ long _i = (long)({ix}) % {la.n}; " &
             &"{la.sym}[_i < 0 ? _i + {la.n} : _i] = ({v}); }}\n"
    if c.lookupArray(sc, base.str).ok:
      raise newException(ValueError,
        base.str & " is a constant table; to change elements declare it " &
        "with runtime values or array(N) " & errLoc(st))
  c.assignPairSide(sc, st)

# One non-final statement of a def, lambda or stereo-def body: `let`, a
# a `$state` declaration or assignment, `$z[k] = ...`, or a bare
# expression run for its side effects. Appends C statements to `pre`.
proc emitLocalStmt(c: Ctx; sc: Scope; st: Node; pre: var string) =
  case st.kind
  of nkLet:
    let rhs = st.kids[0]
    # Numeric-literal array let: hoist to a static const so def bodies
    # can express tabular data (`let p = [2.0, 3.0, 5.0]; p[n - 1]`).
    # Without this, a def body forced the parenthesised `if/else`
    # cascade for any constant-table lookup. Two numbers are also a
    # pair value (`let c = [0.3, 0.1]; cmul(z, c)`).
    if (rhs.kind == nkArr and rhs.kids.len != 2 and rhs.kids.len > 0 and
        not rhs.kids.allIt(it.kind == nkNum)) or isArrayMake(rhs):
      c.declareLocalArray(sc, st.str, rhs, pre)
    elif rhs.kind == nkArr and rhs.kids.len > 0 and
       rhs.kids.allIt(it.kind == nkNum):
      let sym = c.fresh("arr_def_" & st.str)
      var items: seq[string] = @[]
      for k in rhs.kids: items.add numLit(k.num)
      c.arrayDecls.add &"static const double {sym}[{rhs.kids.len}] = {{" &
        items.join(", ") & "};\n"
      sc.arrays[st.str] = (sym, rhs.kids.len)
      if rhs.kids.len == 2:
        let tmpL = c.fresh("l_" & st.str & "_l")
        let tmpR = c.fresh("l_" & st.str & "_r")
        pre.add &"  double {tmpL} = {items[0]};\n  double {tmpR} = {items[1]};\n"
        sc.stereoNames[st.str] = (tmpL, tmpR)
    elif c.refsStereo(sc, rhs):
      let v = c.emitStereo(sc, rhs, pre)
      if v.stereo:
        let tmpL = c.fresh("l_" & st.str & "_l")
        let tmpR = c.fresh("l_" & st.str & "_r")
        pre.add &"  double {tmpL} = ({v.l});\n  double {tmpR} = ({v.r});\n"
        sc.stereoNames[st.str] = (tmpL, tmpR)
      else:
        let tmp = c.fresh("l_" & st.str)
        pre.add &"  double {tmp} = ({v.l});\n"
        sc.names[st.str] = tmp
    else:
      let tmp = c.fresh("l_" & st.str)
      let v = c.emitExpr(sc, rhs)
      pre.add &"  double {tmp} = ({v});\n"
      sc.names[st.str] = tmp
      # Literal propagation: see the matching comment in emitDefInline.
      let cv = c.constValue(sc, rhs)
      if cv.ok: sc.numLits[st.str] = cv.val
      else: c.noteInvariant(sc, st.str, rhs, sc)
  of nkVar:
    if isArrayMake(st.kids[0]):
      c.declareStateArray(sc, st, pre)
    elif c.isStereoReturn(st.kids[0]):
      # Pair state (`$z = [1, 0]`): a "pvar" region of three slots, the
      # inited flag then the two sides. Its own helper type, so adding
      # one doesn't shift the "var" regions of scalar state.
      let flagSlot = c.fresh("pf")
      let off = c.registerRegionInit("pvar", 3, stateName = st.str)
      pre.add &"  long {flagSlot} = {off};\n"
      var initPre = ""
      let v = c.emitStereo(sc, st.kids[0], initPre)
      pre.add &"  if (s->pool[{flagSlot}] == 0.0) {{\n" & initPre &
              &"    s->pool[{flagSlot} + 1] = ({v.l});\n" &
              &"    s->pool[{flagSlot} + 2] = ({v.r});\n" &
              &"    s->pool[{flagSlot}] = 1.0;\n  }}\n"
      sc.stereoNames[st.str] = (&"s->pool[{flagSlot} + 1]",
                                &"s->pool[{flagSlot} + 2]")
    else:
      # `$x` inside a def body -> claim two pool slots keyed under a
      # "var" helper-type region: one "inited" flag, one value. On the
      # first evaluation the init expression runs; thereafter the stored
      # value persists across samples (and across hot reloads, because
      # the (type, perTypeIdx) pair survives a state migration).
      let flagSlot = c.fresh("vf")
      let valSlot = c.fresh("vs")
      let off = c.registerRegionInit("var", 2, stateName = st.str)
      pre.add &"  long {flagSlot} = {off};\n  long {valSlot} = ({off}) + 1;\n"
      var initPre = ""
      let initC = c.scalarValue(sc, st.kids[0], initPre, "$" & st.str)
      pre.add &"  if (s->pool[{flagSlot}] == 0.0) {{\n" & initPre &
              &"    s->pool[{valSlot}] = ({initC});\n" &
              &"    s->pool[{flagSlot}] = 1.0;\n  }}\n"
      sc.names[st.str] = &"s->pool[{valSlot}]"
  of nkAssign:
    let pair = sc.lookupStereo(st.str)
    if pair.ok:
      pre.add c.assignPair(sc, (pair.l, pair.r), st.kids[0])
    else:
      let target = sc.lookup(st.str)
      if target.len == 0:
        raise newException(ValueError,
          "unknown assignment target: " & st.str & " " & errLoc(st))
      let rhs = c.scalarValue(sc, st.kids[0], pre, "$" & st.str)
      pre.add &"  {target} = ({rhs});\n"
  of nkIdxAssign:
    pre.add c.emitIdxAssign(sc, st)
  else:
    # A bare expression statement is legal (ignored value except last).
    if c.refsStereo(sc, st):
      let v = c.emitStereo(sc, st, pre)
      pre.add &"  (void)({v.l});\n  (void)({v.r});\n"
    else:
      pre.add &"  (void)({c.emitExpr(sc, st)});\n"

# Body of a def (nkBlock). All statements but the last are stmts; last
# is the expression returned. `let` adds a C local; `$x` is call-site
# state stored in the pool.
proc emitBlockExpr(c: Ctx; sc: Scope; blk: Node): string =
  var pre = ""
  let last = blk.kids.len - 1
  for i, s in blk.kids:
    if i == last:
      let v = c.scalarValue(sc, s, pre, "the result")
      return "({ " & pre & "double __r = (" & v & "); __r; })"
    c.emitLocalStmt(sc, s, pre)
  "0.0"   # empty block

# ---- recursion ----------------------------------------------------------
# A def that calls itself (notes/tail-recursion-2026-10-16.md) compiles
# to a loop at each call site instead of being inlined. A call to itself
# that is the whole result of a branch is a repeat: the new arguments are
# computed from the old values, the parameters take them, and the body
# runs again. Nothing counts the steps. A recursion still running at its
# block's deadline gives up (n_overrun), and the engine rolls the voice
# back or mutes it.

type RecLoop = object
  def: Node
  vars: seq[tuple[l, r: string]]   # each parameter's loop variable(s); "" when bound by name
  pairs: seq[bool]                 # parameters that hold a pair
  pair: bool                       # the def returns a pair
  rl, rr: string                   # where the result goes

# The first call a def's body makes to the def itself, or nil.
proc selfCall(def: Node): Node =
  proc walk(name: string; n: Node): Node =
    if n == nil: return nil
    if n.kind == nkCall and n.str == name: return n
    for k in n.kids:
      let found = walk(name, k)
      if found != nil: return found
    nil
  walk(def.str, def.kids[0])

# The defs that repeat: those marked `def rec` that call themselves. A
# def that calls itself without the mark is a mistake, often a wrapper
# reaching for the stdlib def it replaces.
proc markRecursion(c: Ctx) =
  for name, def in c.userDefs:
    let call = selfCall(def)
    if call == nil: continue
    if not def.isRec:
      raise newException(ValueError,
        name & " calls itself " & errLoc(call) & ". Write def rec " & name &
        " to make it repeat. (A def can't call the one it replaces: a " &
        "wrapper needs a name of its own.)")
    c.recursive.incl name

# A C string literal: a def's name and line, for the log.
proc cLiteral(s: string): string =
  result = "\""
  for ch in s:
    if ch in {'"', '\\'}: result.add '\\'
    result.add ch
  result.add '"'

# Every call a def makes to itself has to be a repeat: the body, either
# branch of an `if` that is one, or the last line of a block that is one.
proc checkRepeats(c: Ctx; def: Node) =
  if def.str in c.tailChecked: return
  proc walk(def, n: Node; tail: bool) =
    if n == nil: return
    case n.kind
    of nkCall:
      if n.str == def.str and not tail:
        raise newException(ValueError,
          def.str & " calls itself inside an expression " & errLoc(n) &
          ". A def repeats only when the call is the whole result of a " &
          "branch: if done then x else " & def.str & "(...). Carry the " &
          "rest of the work in a parameter instead, like def fib(n, " &
          "a = 0, b = 1): if n == 0 then a else fib(n - 1, b, a + b)")
      for k in n.kids: walk(def, k, false)
    of nkIf:
      walk(def, n.kids[0], false)
      walk(def, n.kids[1], tail)
      walk(def, n.kids[2], tail)
    of nkBlock:
      for i, k in n.kids: walk(def, k, tail and i == n.kids.len - 1)
    else:
      for k in n.kids: walk(def, k, false)
  walk(def, def.kids[0], true)
  c.tailChecked.incl def.str

# The parameters some repeat changes: it passes something other than the
# parameter itself. A name that a `let` or `$` in the body rebinds counts
# as changed.
proc changingParams(def: Node): seq[bool] =
  result = newSeq[bool](def.params.len)
  proc binds(n: Node; names: var HashSet[string]) =
    if n == nil: return
    if n.kind in {nkLet, nkVar}: names.incl n.str
    for k in n.kids: binds(k, names)
  var rebound = initHashSet[string]()
  binds(def.kids[0], rebound)
  proc walk(def, n: Node; rebound: HashSet[string]; res: var seq[bool]) =
    if n == nil: return
    if n.kind == nkCall and n.str == def.str:
      for i, a in n.kids:
        if i < res.len and not (a.kind == nkIdent and
                                a.str == def.params[i] and
                                a.str notin rebound):
          res[i] = true
    for k in n.kids: walk(def, k, rebound, res)
  walk(def, def.kids[0], rebound, result)

# The tail of a recursive def's body, as C statements inside the loop:
# a real branch for each `if` (only the branch taken runs), `continue`
# for a repeat, `break` with the result for anything else.
proc emitTail(c: Ctx; sc: Scope; lp: RecLoop; n: Node; o: var string) =
  case n.kind
  of nkBlock:
    let inner = push(sc)
    for i, st in n.kids:
      if i == n.kids.len - 1: c.emitTail(inner, lp, st, o)
      else: c.emitLocalStmt(inner, st, o)
    return
  of nkIf:
    let cond = c.scalarValue(sc, n.kids[0], o, "the condition")
    o.add &"  if (({cond}) != 0.0) {{\n"
    c.emitTail(sc, lp, n.kids[1], o)
    o.add "  } else {\n"
    let els = if n.kids[2] != nil: n.kids[2]
              else: Node(kind: nkNum, num: 0.0, line: n.line, source: n.source)
    c.emitTail(sc, lp, els, o)
    o.add "  }\n"
    return
  of nkCall:
    if n.str == lp.def.str:
      let def = lp.def
      if n.kids.len != def.params.len:
        raise newException(ValueError,
          "wrong arg count for " & def.str & ": expected " &
          $def.params.len & " got " & $n.kids.len & " " & errLoc(n))
      # Every new value first, from the old ones; then the parameters
      # take them all at once.
      var assigns = ""
      for i, a in n.kids:
        let p = def.params[i]
        let v = lp.vars[i]
        if v.l.len == 0:
          if not (a.kind == nkIdent and a.str == p):
            raise newException(ValueError,
              def.str & " passes a different array or function as '" & p &
              "' " & errLoc(n) & ": an array or function parameter is " &
              "passed on unchanged when a def repeats")
          continue
        if lp.pairs[i]:
          if a.kind == nkIdent and sc.lookupStereo(a.str).l == v.l: continue
          let sv = c.emitStereo(sc, a, o)
          let tl = c.fresh("next_" & p & "_l")
          let tr = c.fresh("next_" & p & "_r")
          o.add &"  double {tl} = ({sv.l}); double {tr} = ({sv.r});\n"
          assigns.add &"  {v.l} = {tl}; {v.r} = {tr};\n"
        else:
          if a.kind == nkIdent and sc.lookup(a.str) == v.l: continue
          let x = c.scalarValue(sc, a, o, def.str & "'s " & p)
          let t = c.fresh("next_" & p)
          o.add &"  double {t} = ({x});\n"
          assigns.add &"  {v.l} = {t};\n"
      o.add assigns
      o.add "  continue;\n"
      return
  else: discard
  if lp.pair:
    let v = c.emitStereo(sc, n, o)
    o.add &"  {lp.rl} = ({v.l}); {lp.rr} = ({v.r}); break;\n"
  else:
    let v = c.scalarValue(sc, n, o, "the result of " & lp.def.str)
    o.add &"  {lp.rl} = ({v}); break;\n"

# A call to a recursive def: C statements that run the loop, and the C
# names of its result (`l` and `r` are the same name for a number).
proc emitLoop(c: Ctx; sc: Scope; def, call: Node; pair: bool):
    tuple[stmts, l, r: string] =
  let args = call.kids
  if args.len != def.params.len:
    raise newException(ValueError,
      "wrong arg count for " & def.str & ": expected " &
      $def.params.len & " got " & $args.len & " " & errLoc(call))
  if def.str in c.defStack: raise c.cycleError(def, call)
  c.checkRepeats(def)
  let changing = changingParams(def)
  var lp = RecLoop(def: def, pairs: c.pairParams(def), pair: pair)
  let inner = push(sc)
  var o = ""
  for i, p in def.params:
    let a = args[i]
    if lp.pairs[i]:
      let sv = c.emitStereo(sc, a, o)
      let vl = c.fresh("rec_" & p & "_l")
      let vr = c.fresh("rec_" & p & "_r")
      o.add &"  double {vl} = ({sv.l}); double {vr} = ({sv.r});\n"
      inner.stereoNames[p] = (vl, vr)
      lp.vars.add (vl, vr)
      continue
    var sub = ""
    if c.bindByName(sc, inner, p, a, sub):
      o.add sub
      lp.vars.add ("", "")
      continue
    let v = c.scalarValue(sc, a, o, def.str & "'s " & p)
    let vn = c.fresh("rec_" & p)
    o.add &"  double {vn} = ({v});\n"
    inner.names[p] = vn
    # A parameter no repeat changes keeps its compile-time value, so
    # each(n, ...) and array(n) in the body still see it.
    if not changing[i]:
      let cv = c.constValue(sc, a)
      if cv.ok: inner.numLits[p] = cv.val
      else: c.noteInvariant(inner, p, a, sc)
    lp.vars.add (vn, "")
  lp.rl = c.fresh("rec_" & def.str)
  lp.rr = if pair: c.fresh("rec_" & def.str & "_r") else: lp.rl
  let steps = c.fresh("steps")
  let giveUp = if pair: &"{lp.rl} = {lp.rr} = 0.0 / 0.0;"
               else: &"{lp.rl} = 0.0 / 0.0;"
  let where = cLiteral(def.str & " " & errLoc(def))
  o.add (if pair: &"  double {lp.rl}, {lp.rr};\n" else: &"  double {lp.rl};\n")
  o.add &"  long {steps} = 0;\n"
  o.add &"  if (s->stuck) {{ {giveUp} }} else for (;;) {{\n"
  o.add &"  if ((++{steps} & 65535) == 0 && n_overrun((void*)s, {where})) " &
        &"{{ {giveUp} break; }}\n"
  # The body is the def's (its state regions are owned by it), and its
  # stores land at once even when the call sits inside an `each`.
  c.defStack.add def.str
  let depth = c.eachDepth
  let deferred = c.deferred
  c.eachDepth = 0
  c.deferred = ""
  c.emitTail(inner, lp, def.kids[0], o)
  c.eachDepth = depth
  c.deferred = deferred
  discard c.defStack.pop()
  o.add "  }\n"
  (o, lp.rl, lp.rr)

# phasor(freq) inlined: one pool slot, advance, return wrapped value.
proc emitPhasor(c: Ctx; sc: Scope; freqNode: Node): string =
  # The per-sample increment freq/sr is computed once when the frequency
  # can't change (hoisted); the division's result is the same number.
  let step =
    if c.hoistOn and c.isInvariant(sc, freqNode):
      c.hoist("(" & c.emitInvariant(sc, freqNode) & ") / s->sr")
    else:
      "(" & c.emitExpr(sc, freqNode) & ") / s->sr"
  let slot = c.fresh("ph")
  let off = c.registerRegion("phasor", 1)
  "(({ long " & slot & " = " & off & "; " &
    "s->pool[" & slot & "] = fmod(s->pool[" & slot & "] + " & step &
    ", 1.0); " &
    "if (s->pool[" & slot & "] < 0.0) s->pool[" & slot & "] += 1.0; " &
    "s->pool[" & slot & "]; }))"

# noise() inlined: xorshift32 on a pool slot interpreted as uint32.
# A fresh slot (0) seeds from where the call is: its statement, the defs
# it's inlined through, and which noise() it is there. So every call
# site is its own stream, the same on every run, and an edit elsewhere
# leaves it alone. (It used to seed from its pool address, which moves
# whenever anything earlier in the layout is added or removed: dropping
# reverbs from a set's mix line reshuffled every bird and strike in it.)
proc emitNoise(c: Ctx): string =
  let slot = c.fresh("nz")
  let off = c.registerRegion("noise", 1)
  let r = c.regions[^1]
  var seed = 2166136261'u32                    # FNV-1a over the call's identity
  for ch in r.scope & "|" & r.owner & "|" & $r.scopeIdx:
    seed = (seed xor uint32(ord(ch))) * 16777619'u32
  if seed == 0: seed = 2463534242'u32
  "(({ long " & slot & " = " & off & "; " &
    "unsigned int r = (unsigned int)s->pool[" & slot & "]; " &
    "if (r == 0) r = " & $seed & "u; " &
    "r ^= r << 13; r ^= r >> 17; r ^= r << 5; " &
    "s->pool[" & slot & "] = (double)r; " &
    "((double)r / 4294967295.0) * 2.0 - 1.0; }))"

# Size (in float64 slots) claimed by one call to a given native. For
# delay/fbdelay this depends on the `max_time` argument, which must be
# a numeric literal so the region size is compile-time known.
proc delayBufSlots(c: Ctx; name: string; maxTimeArg: Node): int =
  if maxTimeArg.kind != nkNum:
    raise newException(ValueError,
      name & " max_time must be a numeric literal " & errLoc(maxTimeArg))
  1 + max(1, int(maxTimeArg.num * c.sr))

proc nativeSlotSize(c: Ctx; name: string; kids: seq[Node]): int =
  case name
  of "lp1", "hp1", "impulse", "discharge", "tremolo", "slew", "wave": 1
  of "lpf", "hpf", "bpf", "notch", "resonator", "dho", "dho_v":       2
  of "delay", "fbdelay":          delayBufSlots(c, name, kids[2])
  of "reverb":
    # Matches dsp.nim nReverb's total = sum(2+L for combLens) + sum(1+L for apLens).
    (2+1557) + (2+1617) + (2+1491) + (2+1422) + (1+225) + (1+556)
  else:
    raise newException(ValueError, "no slot size for native '" & name & "'")

proc emitExpr(c: Ctx; sc: Scope; n: Node): string =
  case n.kind
  of nkNum:
    numLit(n.num)
  of nkIdent:
    case n.str
    of "t":       "s->t"
    of "sr":      "s->sr"
    of "dt":      "s->dt"
    of "start_t": "s->start_t"
    of "PI":      "3.14159265358979323846"
    of "TAU":     "(2.0 * 3.14159265358979323846)"
    else:
      let scoped = sc.lookup(n.str)
      let table = c.lookupArray(sc, n.str)
      if scoped.len > 0: scoped
      elif table.ok and table.length != 2:
        # A hoisted table has no scalar C symbol; without this the C
        # compile failed with "'l_name' undeclared".
        raise newException(ValueError,
          n.str & " is a table of " & $table.length & " numbers, but one " &
          "number is needed here — index it (" & n.str & "[i]) or hand " &
          "it to wave " & errLoc(n))
      elif sc.lookupStereo(n.str).ok or n.str in c.stereoLets or
           n.str in c.pairVars or n.str in c.playSet:
        raise newException(ValueError,
          n.str & " is a pair, but one number is needed here — pick a " &
          "side with " & n.str & "[0] / " & n.str & "[1], or use mono(" &
          n.str & ") " & errLoc(n))
      elif n.str in c.topLets:
        if n.str notin c.letsDone:
          raise newException(ValueError,
            "'" & n.str & "' is used before its `let` on line " &
            $c.letLines.getOrDefault(n.str) & ": move that let above " &
            "this line " & errLoc(n))
        "l_" & n.str
      elif n.str in c.topVarSet: "s->v_" & n.str
      else:
        raise newException(ValueError,
          "unknown identifier: " & n.str & " " & errLoc(n))
  of nkUnary:
    let v = c.emitExpr(sc, n.kids[0])
    case n.str
    of "-":   "(-(" & v & "))"
    of "not": "((" & v & ") == 0.0 ? 1.0 : 0.0)"
    else:     raise newException(ValueError,
                "bad unary: " & n.str & " " & errLoc(n))
  of nkBinOp:
    let a = c.emitExpr(sc, n.kids[0])
    let b = c.emitExpr(sc, n.kids[1])
    case n.str
    of "+":  &"(({a}) + ({b}))"
    of "-":  &"(({a}) - ({b}))"
    of "*":  &"(({a}) * ({b}))"
    of "/":  &"(({b}) == 0.0 ? 0.0 : ({a}) / ({b}))"
    of "mod":
      # aither mod: `a - floor(a/b) * b` (safe on zero)
      &"(({b}) == 0.0 ? 0.0 : ({a}) - floor(({a}) / ({b})) * ({b}))"
    of "==": &"((({a}) == ({b})) ? 1.0 : 0.0)"
    of "!=": &"((({a}) != ({b})) ? 1.0 : 0.0)"
    of "<":  &"((({a}) < ({b})) ? 1.0 : 0.0)"
    of ">":  &"((({a}) > ({b})) ? 1.0 : 0.0)"
    of "<=": &"((({a}) <= ({b})) ? 1.0 : 0.0)"
    of ">=": &"((({a}) >= ({b})) ? 1.0 : 0.0)"
    # Eager eval: aither's `and` / `or` do NOT short-circuit. Both
    # operands always run, even when the result is determined by the
    # first. This matters for the rising-edge idiom `g > 0.5 and
    # prev(g) < 0.5` — `prev(g)` is the call site that updates prev's
    # last-sample slot. With short-circuit (`&&`), prev() doesn't run
    # while g is low, so its slot freezes at the last high value, and
    # the second note-on never fires. Bitwise `&` / `|` on the 0/1
    # results of the comparisons gives the same logical answer without
    # the conditional skip.
    of "and": &"(((({a}) != 0.0) & (({b}) != 0.0)) ? 1.0 : 0.0)"
    of "or":  &"(((({a}) != 0.0) | (({b}) != 0.0)) ? 1.0 : 0.0)"
    else: raise newException(ValueError,
            "bad binop: " & n.str & " " & errLoc(n))
  of nkIf:
    let cond = c.emitExpr(sc, n.kids[0])
    let thn  = c.emitExpr(sc, n.kids[1])
    let els  = if n.kids[2] != nil: c.emitExpr(sc, n.kids[2]) else: "0.0"
    &"(({cond}) != 0.0 ? ({thn}) : ({els}))"
  of nkCall:
    # Pure math over invariant inputs (knobs, numbers, names bound to
    # them) runs once a block in hoist_init instead of every sample:
    # spectra's per-partial exp(log(n) * (1 - u) + ...) with u from a
    # knob. Only the costly calls; a sum or a min isn't worth a static.
    if c.hoistOn and not c.inHoist and
       n.str in ["exp", "log", "log2", "pow", "sin", "cos", "tan", "tanh", "sqrt"] and
       n.str notin c.userDefs and sc.lookupFn(n.str).len == 0 and
       sc.lookupLambda(n.str) == nil and c.isInvariant(sc, n) and
       not c.constValue(sc, n).ok:
      return c.hoist(c.emitInvariant(sc, n))
    # Lambda-valued param? Inline the body, binding call args to lambda
    # params. Pre-empts the named-fn / builtin / native paths because a
    # lambda has no name to dispatch through. Powers `voice_fn(f, g)`
    # inside `def poly(n_voices, voice_fn): ...`.
    let lamNode = sc.lookupLambda(n.str)
    if lamNode != nil:
      # A lambda may take fewer arguments than it is called with; the
      # extras aren't evaluated. So a caller can pass everything it
      # knows (`voice_fn(freq, gate, vel, key)`) and each lambda names
      # what it uses, and adding an argument later breaks nothing.
      if n.kids.len < lamNode.params.len:
        raise newException(ValueError,
          "lambda '" & n.str & "' takes " & $lamNode.params.len &
          " args, got " & $n.kids.len & " " & errLoc(n))
      let inner = push(sc)
      var preface = ""
      for i, p in lamNode.params:
        let argC = c.emitExpr(sc, n.kids[i])
        let tmp = c.fresh("p_" & p)
        preface.add &"double {tmp} = ({argC}); "
        inner.names[p] = tmp
        # Literal propagation through lambda params — same logic as
        # emitDefInline. Lets a lambda body call sum(p, ...) when p is
        # bound to a numeric literal at the call site.
        let cv = c.constValue(sc, n.kids[i])
        if cv.ok: inner.numLits[p] = cv.val
        else: c.noteInvariant(inner, p, n.kids[i], sc)
      let body = lamNode.kids[0]
      let bodyC =
        if body.kind == nkBlock: c.emitBlockExpr(inner, body)
        else: c.emitExpr(inner, body)
      return "(({ " & preface & "double __r = (" & bodyC & "); __r; }))"
    # If the call target is a fn-valued param, resolve to the concrete name.
    let resolved = sc.lookupFn(n.str)
    let name = if resolved.len > 0: resolved else: n.str
    # sum(N, fn) — compile-time fold. Special-cased before the native
    # table because it's pure codegen (no n_sum runtime symbol). N must
    # resolve to a numeric literal at codegen time; the literal may
    # travel through def params or lets via `numLits` propagation.
    if name == "sum":
      if n.kids.len != 2:
        raise newException(ValueError,
          "sum takes 2 args: sum(N, fn) " & errLoc(n))
      let nArg = n.kids[0]
      let lamArg = n.kids[1]
      let cv = c.constValue(sc, nArg)
      if not cv.ok:
        raise newException(ValueError,
          "sum: first arg must be a compile-time-constant integer " &
          "(a literal, a name bound to one, or len(array)) " & errLoc(nArg))
      var nVal = cv.val
      let N = int(nVal)
      if float64(N) != nVal:
        raise newException(ValueError,
          "sum: first arg must be an integer, got " & $nVal & " " &
          errLoc(nArg))
      if N < 0:
        raise newException(ValueError,
          "sum: first arg must be non-negative, got " & $N & " " &
          errLoc(nArg))
      if lamArg.kind != nkLambda:
        raise newException(ValueError,
          "sum: second arg must be a lambda like `n => expr`, got " &
          $lamArg.kind & " " & errLoc(lamArg))
      if lamArg.params.len != 1:
        raise newException(ValueError,
          "sum: lambda must take exactly one parameter " & errLoc(lamArg))
      if N == 0: return "0.0"
      let paramName = lamArg.params[0]
      let body = lamArg.kids[0]
      var terms: seq[string] = @[]
      for k in 1..N:
        # Each iteration gets its own child scope — so the param binding
        # doesn't leak across iterations, and per-iteration stateful
        # primitives (phasor, delay, etc.) each hit registerRegion anew
        # and claim their own pool slot.
        let iter = push(sc)
        let litText = numLit(float64(k))
        iter.names[paramName] = litText
        iter.numLits[paramName] = float64(k)
        # A lambda body may be either a single expression or an nkBlock
        # of `let` bindings terminated by a final expression. Route the
        # latter through emitBlockExpr so its `let` cases bind locals
        # in the iteration scope.
        let bodyC =
          if body.kind == nkBlock: c.emitBlockExpr(iter, body)
          else: c.emitExpr(iter, body)
        terms.add bodyC
      return "(" & terms.join(" + ") & ")"
    # each(N, i => stmts) — the element-wise step. Unrolled like sum, but
    # the body is statements (`$f[i] = ...`, lets) and the value is 0.
    # Every store to a state array inside it is held back and committed
    # when the each ends, so each body reads the arrays as they were
    # before the step: a waveguide's section i+1 reads section i's wave
    # from the last sample, not the one just written. Stores to local
    # arrays and scalars happen in order, as usual.
    if name == "each":
      if n.kids.len != 2 or n.kids[1].kind != nkLambda or
         n.kids[1].params.len != 1:
        raise newException(ValueError,
          "each takes a count and a one-parameter lambda: " &
          "each(N, i => ...) " & errLoc(n))
      let nArg = n.kids[0]
      let cv = c.constValue(sc, nArg)
      if not cv.ok:
        raise newException(ValueError,
          "each: the count must be a compile-time integer " &
          "(a literal, a name bound to one, or len(array)) " & errLoc(nArg))
      var nVal = cv.val
      let N = int(nVal)
      if float64(N) != nVal or N < 0:
        raise newException(ValueError,
          "each: the count must be a non-negative integer, got " & $nVal &
          " " & errLoc(nArg))
      let lam = n.kids[1]
      let savedDeferred = c.deferred
      c.deferred = ""
      inc c.eachDepth
      var body = ""
      for k in 0 ..< N:
        let iter = push(sc)
        iter.names[lam.params[0]] = numLit(float64(k))
        iter.numLits[lam.params[0]] = float64(k)
        let b = lam.kids[0]
        if b.kind == nkBlock:
          for st in b.kids[0 ..< b.kids.len - 1]:
            c.emitLocalStmt(iter, st, body)
          let last = b.kids[^1]
          if not (last.kind == nkNum and last.num == 0.0):
            body.add &"  (void)({c.emitExpr(iter, last)});\n"
        else:
          body.add &"  (void)({c.emitExpr(iter, b)});\n"
      dec c.eachDepth
      let commits = c.deferred
      c.deferred = savedDeferred
      return "(({\n" & body & commits & "  0.0; }))"
    # len(a): the length of an array, a compile-time constant.
    if name == "len":
      if n.kids.len == 1 and n.kids[0].kind == nkIdent:
        let a = n.kids[0].str
        let sa = c.lookupSarray(sc, a)
        if sa.ok: return numLit(float64(sa.n))
        let la = c.lookupLarray(sc, a)
        if la.ok: return numLit(float64(la.n))
        let ca = c.lookupArray(sc, a)
        if ca.ok: return numLit(float64(ca.length))
      raise newException(ValueError,
        "len takes the name of an array " & errLoc(n))
    if name == "array":
      raise newException(ValueError,
        "array(N) declares an array: `$f = array(N)` for state that " &
        "lasts, `let a = array(N)` for this sample's scratch " & errLoc(n))
    # Stateful builtins inlined
    if name == "phasor":
      if n.kids.len != 1:
        raise newException(ValueError, "phasor takes 1 arg " & errLoc(n))
      return c.emitPhasor(sc, n.kids[0])
    if name == "noise":
      return c.emitNoise()
    # Libm 1-arg
    if name in Libm1 and n.kids.len == 1:
      return name & "(" & c.emitExpr(sc, n.kids[0]) & ")"
    # abs on doubles is fabs in C
    if name == "abs" and n.kids.len == 1:
      return "fabs(" & c.emitExpr(sc, n.kids[0]) & ")"
    # Shape helpers (builtin as bytecode ops previously) — inline via
    # phasor + shape fn. But these take a *phase* argument, not freq.
    # saw/tri/sqr are shape functions: saw(x) where x is TAU*phase.
    # Keeping the aitherism: shape_saw/tri/sqr map x in [0, TAU] to wave.
    if name in ["saw", "tri", "sqr"] and n.kids.len == 1:
      return "shape_" & name & "(" & c.emitExpr(sc, n.kids[0]) & ")"
    # Math misc
    if name == "pow" and n.kids.len == 2:
      return "pow(" & c.callArgs(sc, n.kids) & ")"
    # min/max take two or more: max(a, b, c) = fmax(fmax(a, b), c).
    if name in ["min", "max"] and n.kids.len >= 2:
      var acc = c.emitExpr(sc, n.kids[0])
      for k in n.kids[1 .. ^1]:
        acc = &"f{name}({acc}, {c.emitExpr(sc, k)})"
      return acc
    if name == "clamp" and n.kids.len == 3:
      let v = c.emitExpr(sc, n.kids[0])
      let lo = c.emitExpr(sc, n.kids[1])
      let hi = c.emitExpr(sc, n.kids[2])
      return &"fmin(fmax({v}, {lo}), {hi})"
    if name == "int" and n.kids.len == 1:
      return "((double)(long)(" & c.emitExpr(sc, n.kids[0]) & "))"
    # Pair-returning calls in scalar context: refuse with a hint. The
    # stereo/pair path captures these in emitStereo; reaching emitExpr
    # means the user wrote one where a single number is needed.
    if name in PairCalls:
      raise newException(ValueError,
        name & " returns a pair — pick a side with `" & name &
        "(...)[0]` / `[1]`, bind it with `let p = " & name & "(...)`, " &
        "or pass it to `magnitude` / `phase` " & errLoc(n))
    # Scalar reductions of a pair: one pair, or its two sides as numbers.
    if name in PairReaders:
      var pre = ""
      let a = c.pairOperands(sc, n, pre, 2, "one pair (or two numbers)")
      let v =
        if name == "magnitude": &"sqrt({a[0]} * {a[0]} + {a[1]} * {a[1]})"
        else: &"atan2({a[1]}, {a[0]})"
      return "(({\n" & pre & "  double __m = " & v & "; __m; }))"
    # freq_shift(signal, hz) — analytic + rotation, scalar real-part-only
    # output. Stateful native (32-slot Hilbert pair + 1 phase slot).
    if name == "freq_shift" and n.kids.len == 2:
      let sig = c.emitExpr(sc, n.kids[0])
      let hz = c.emitExpr(sc, n.kids[1])
      let off = c.registerRegion("freq_shift", 33)
      return c.nativeAt(off, [sig, hz], "n_freq_shift((DspState*)s, $1, $2)")
    if name == "freq_shift":
      raise newException(ValueError,
        "freq_shift(signal, hz) takes exactly 2 args " & errLoc(n))
    # Mutable arrays. Per-call-site region (length, capacity, data...).
    # Handle = pool offset of the region's metadata head, returned as
    # double for plumbing through aither's normal scalar path. Bounds
    # checks are runtime; out-of-bounds reads return 0 (no crash).
    if name == "array_make":
      if n.kids.len != 1 or n.kids[0].kind != nkNum:
        raise newException(ValueError,
          "array_make takes a numeric literal capacity " & errLoc(n))
      let cap = int(n.kids[0].num)
      if cap <= 0 or cap > 4096:
        raise newException(ValueError,
          "array_make: capacity must be 1..4096, got " &
          $cap & " " & errLoc(n))
      # Header: 2 slots (length, capacity). Data: cap slots, zero-init.
      let off = c.registerRegionInit("array",
                                     2 + cap,
                                     @[float64(cap), float64(cap)])
      return &"((double){off})"
    if name == "array_get" and n.kids.len == 2:
      let h = c.emitExpr(sc, n.kids[0])
      let i = c.emitExpr(sc, n.kids[1])
      return "(({ long _h = (long)(" & h & "); long _i = (long)(" & i & "); " &
             "long _len = (long)s->pool[_h]; " &
             "(_i >= 0 && _i < _len) ? s->pool[_h + 2 + _i] : 0.0; }))"
    if name == "array_set" and n.kids.len == 3:
      let h = c.emitExpr(sc, n.kids[0])
      let i = c.emitExpr(sc, n.kids[1])
      let v = c.emitExpr(sc, n.kids[2])
      return "(({ long _h = (long)(" & h & "); long _i = (long)(" & i & "); " &
             "double _v = (" & v & "); " &
             "long _len = (long)s->pool[_h]; " &
             "if (_i >= 0 && _i < _len) s->pool[_h + 2 + _i] = _v; " &
             "_v; }))"
    if name == "array_len" and n.kids.len == 1:
      let h = c.emitExpr(sc, n.kids[0])
      return "(s->pool[(long)(" & h & ")])"
    if name == "array_push" and n.kids.len == 2:
      let h = c.emitExpr(sc, n.kids[0])
      let v = c.emitExpr(sc, n.kids[1])
      return "(({ long _h = (long)(" & h & "); double _v = (" & v & "); " &
             "long _len = (long)s->pool[_h]; long _cap = (long)s->pool[_h + 1]; " &
             "if (_len < _cap) { s->pool[_h + 2 + _len] = _v; " &
             "s->pool[_h] = (double)(_len + 1); _len = _len + 1; } " &
             "(double)_len; }))"
    if name == "array_pop" and n.kids.len == 1:
      let h = c.emitExpr(sc, n.kids[0])
      return "(({ long _h = (long)(" & h & "); " &
             "long _len = (long)s->pool[_h]; double _r; " &
             "if (_len > 0) { _len = _len - 1; s->pool[_h] = (double)_len; " &
             "_r = s->pool[_h + 2 + _len]; } else { _r = 0.0; } " &
             "_r; }))"
    if name == "array_resize" and n.kids.len == 2:
      let h = c.emitExpr(sc, n.kids[0])
      let nl = c.emitExpr(sc, n.kids[1])
      return "(({ long _h = (long)(" & h & "); long _n = (long)(" & nl & "); " &
             "long _cap = (long)s->pool[_h + 1]; long _old = (long)s->pool[_h]; " &
             "if (_n < 0) _n = 0; if (_n > _cap) _n = _cap; " &
             "for (long _k = _old; _k < _n; ++_k) s->pool[_h + 2 + _k] = 0.0; " &
             "s->pool[_h] = (double)_n; (double)_n; }))"
    if name in ["array_make", "array_get", "array_set", "array_len",
                "array_push", "array_pop", "array_resize"]:
      raise newException(ValueError,
        name & ": wrong arg count " & errLoc(n))
    # MIDI input primitives — read engine-owned global state.
    # midi_cc/note/trig take a note-or-CC number; midi_freq/gate are 0-arg.
    # midi_trig is stateful (per-voice pool slot for edge detection), so
    # it goes through the region path; the other four are pure reads.
    if name == "midi_cc" and n.kids.len == 1:
      return "n_midi_cc((int)(" & c.emitExpr(sc, n.kids[0]) & "))"
    if name == "midi_note" and n.kids.len == 1:
      return "n_midi_note((int)(" & c.emitExpr(sc, n.kids[0]) & "))"
    if name == "midi_freq" and n.kids.len == 0:
      return "n_midi_freq()"
    if name == "midi_gate" and n.kids.len == 0:
      return "n_midi_gate()"
    if name == "midi_trig" and n.kids.len == 1:
      let off = c.registerRegion("midi_trig", 1)
      return c.nativeAt(off, [c.emitExpr(sc, n.kids[0])],
                        "n_midi_trig((DspState*)s, (int)($1))")
    if name == "midi_voice_freq" and n.kids.len == 1:
      return "n_midi_voice_freq((int)(" & c.emitExpr(sc, n.kids[0]) & "))"
    if name == "midi_voice_gate" and n.kids.len == 1:
      return "n_midi_voice_gate((int)(" & c.emitExpr(sc, n.kids[0]) & "))"
    if name == "midi_voice_vel" and n.kids.len == 1:
      return "n_midi_voice_vel((int)(" & c.emitExpr(sc, n.kids[0]) & "))"
    if name == "midi_voice_key" and n.kids.len == 1:
      return "n_midi_voice_key((int)(" & c.emitExpr(sc, n.kids[0]) & "))"
    if name == "midi_vel" and n.kids.len == 0:
      return "n_midi_vel()"
    if name in ["midi_cc", "midi_note", "midi_trig", "midi_freq",
                "midi_gate", "midi_voice_freq", "midi_voice_gate",
                "midi_voice_vel", "midi_voice_key", "midi_vel"]:
      raise newException(ValueError,
        name & ": wrong arg count " & errLoc(n))
    # wave(freq, array) — array must resolve to a compile-time constant.
    if name == "wave":
      if n.kids.len != 2:
        raise newException(ValueError, "wave takes 2 args " & errLoc(n))
      let freq = c.emitExpr(sc, n.kids[0])
      let arr = n.kids[1]
      var sym = ""
      var length = 0
      if arr.kind == nkIdent:
        let found = c.lookupArray(sc, arr.str)
        if not found.ok:
          raise newException(ValueError,
            "wave: " & arr.str & " must be a constant table of numeric " &
            "literals" &
            (if c.lookupLarray(sc, arr.str).ok: " (it holds runtime values)"
             else: "") & " " & errLoc(arr))
        (sym, length) = (found.sym, found.length)
      elif arr.kind == nkArr:
        sym = c.fresh("arr")
        length = arr.kids.len
        var items: seq[string] = @[]
        for k in arr.kids:
          if k.kind != nkNum:
            raise newException(ValueError,
              "wave array must be numeric literals only " & errLoc(k))
          items.add numLit(k.num)
        c.arrayDecls.add &"static const double {sym}[{length}] = {{" &
          items.join(", ") & "};\n"
      else:
        raise newException(ValueError,
          "wave: second arg must be an array literal or a let binding " &
          "to one " & errLoc(arr))
      let size = c.nativeSlotSize("wave", n.kids)
      let off = c.registerRegion("wave", size)
      return c.nativeAt(off, [freq],
                        &"n_wave((DspState*)s, $1, (double*){sym}, {length})")
    # Hoisted natives: coefficients that can't change are computed once
    # (hoist_init) and the per-sample `_c` step takes them ready-made.
    if c.hoistOn and name in ["lp1", "hp1"] and n.kids.len == 2 and
       c.isInvariant(sc, n.kids[1]):
      let a = c.hoist("n_lp1_coef(" & c.emitInvariant(sc, n.kids[1]) & ", s->sr)")
      let off = c.registerRegion(name, c.nativeSlotSize(name, n.kids))
      return c.nativeAt(off, [c.emitExpr(sc, n.kids[0])],
                        "n_" & name & "_c((DspState*)s, $1, " & a & ")")
    if c.hoistOn and name in ["lpf", "hpf", "bpf", "notch"] and n.kids.len == 3 and
       c.isInvariant(sc, n.kids[1]) and c.isInvariant(sc, n.kids[2]):
      let h = c.hoistArr(4, "n_svf_coef(" & c.emitInvariant(sc, n.kids[1]) & ", " &
                            c.emitInvariant(sc, n.kids[2]) & ", s->sr, $H)")
      let off = c.registerRegion(name, c.nativeSlotSize(name, n.kids))
      return c.nativeAt(off, [c.emitExpr(sc, n.kids[0])],
                        "n_" & name & "_c((DspState*)s, $1, " & h & ")")
    if c.hoistOn and name == "reverb" and n.kids.len == 3 and
       c.isInvariant(sc, n.kids[1]):
      let h = c.hoistArr(4, "n_reverb_coef(" & c.emitInvariant(sc, n.kids[1]) &
                            ", s->sr, $H)")
      let off = c.registerRegion(name, c.nativeSlotSize(name, n.kids))
      return c.nativeAt(off, [c.emitExpr(sc, n.kids[0]), c.emitExpr(sc, n.kids[2])],
                        "n_reverb_c((DspState*)s, $1, $2, " & h & ")")
    # Native dsp calls. Each call site has a baked region offset, and
    # nativeAt points s->idx at it after evaluating the args, so the
    # native's internal claim() writes into its dedicated region
    # regardless of what natives the args themselves call.
    if name in NativeArities:
      let arity = NativeArities[name]
      if arity == -1:
        raise newException(ValueError,
          name & " (array arg) not yet supported in codegen " & errLoc(n))
      if n.kids.len != arity:
        raise newException(ValueError,
          name & " takes " & $arity & " args, got " & $n.kids.len &
          " " & errLoc(n))
      let size = c.nativeSlotSize(name, n.kids)
      let off = c.registerRegion(name, size)
      var args: seq[string] = @[]
      var slots: seq[string] = @[]
      for i, k in n.kids:
        args.add c.emitExpr(sc, k)
        slots.add "$" & $(i + 1)
      return c.nativeAt(off, args,
                        "n_" & name & "((DspState*)s, " & slots.join(", ") & ")")
    # User def inline; a def that calls itself runs as a loop.
    if name in c.userDefs:
      if name in c.recursive:
        let lp = c.emitLoop(sc, c.userDefs[name], n, pair = false)
        return "(({\n" & lp.stmts & "  " & lp.l & "; }))"
      return c.emitDefInline(sc, c.userDefs[name], n)
    raise newException(ValueError,
      "unknown function: " & name & " " & errLoc(n))
  of nkArr:
    # Only inline stereo pair arrays are supported as values. Emit as
    # two scalar expressions — the caller context (a final expression or
    # an index) has to unpack.
    raise newException(ValueError,
      "array value can't appear here " & errLoc(n) &
      " — only numeric literals for wave() or top-level stereo return")
  of nkStr:
    raise newException(ValueError,
      "a string is only an import path here " & errLoc(n))
  of nkImport:
    raise newException(ValueError,
      "import belongs at the top of the file " & errLoc(n))
  of nkLambda:
    # v1: lambdas are not first-class values. They may only appear as
    # the second arg to sum(N, fn); reaching emitExpr here means the
    # user stored/returned/used one as a value somewhere else.
    raise newException(ValueError,
      "lambda `" & (if n.params.len > 0: n.params[0] else: "_") &
      " => ...` can only appear as a builtin argument (e.g. " &
      "sum(N, n => expr)), not as a standalone value " & errLoc(n))
  of nkIdx:
    let a = n.kids[0]
    let i = n.kids[1]
    if a.kind == nkIdent:
      # State array: an element past either end reads 0 — beyond a
      # tube or string there is nothing, and a boundary is written by
      # the patch, not borrowed from the far end.
      let sa = c.lookupSarray(sc, a.str)
      if sa.ok:
        let ix = c.emitExpr(sc, i)
        return "(({ long _i = (long)(" & ix & "); " &
               "(_i >= 0 && _i < " & $sa.n & ") ? s->pool[" & sa.base &
               " + 1 + _i] : 0.0; }))"
      # Local and constant arrays cycle, like a table should.
      let la = c.lookupLarray(sc, a.str)
      if la.ok:
        let ix = c.emitExpr(sc, i)
        return "(({ long _i = (long)(" & ix & ") % " & $la.n & "; " &
               la.sym & "[_i < 0 ? _i + " & $la.n & " : _i]; }))"
      # Numeric array: top-level, or auto-hoisted play-local.
      let found = c.lookupArray(sc, a.str)
      if found.ok:
        let ix = c.emitExpr(sc, i)
        return "(({ long _i = (long)(" & ix & ") % " & $found.length & "; " &
               found.sym & "[_i < 0 ? _i + " & $found.length & " : _i]; }))"
    # Play-name / stereo-let / scope-bound stereo indexed with a
    # constant 0 or 1 → L or R channel scalar.
    if a.kind == nkIdent and i.kind == nkNum and int(i.num) in {0, 1}:
      let stereo = sc.lookupStereo(a.str)
      if stereo.ok:
        return (if int(i.num) == 0: stereo.l else: stereo.r)
      if a.str in c.pairVars:
        return pairVarRef(a.str, int(i.num))
      if a.str in c.playSet or a.str in c.stereoLets:
        return stereoRef(c, a.str, if int(i.num) == 0: "l" else: "r")
    # Any other index on a pair: a number worked out as it runs, or a
    # constant past 1. It's truncated and wraps at 2, as a table's index
    # wraps at its length, so a pair and a two-entry table read alike:
    # p[i] is the left side for even i, the right for odd (p[-1] is the
    # right, like a table's last entry).
    let bound = if a.kind == nkIdent: sc.lookupStereo(a.str)
                else: (l: "", r: "", ok: false)
    if bound.ok or c.refsStereo(sc, a):
      var pre = ""
      var l = bound.l
      var r = bound.r
      if not bound.ok:
        let v = c.emitStereo(sc, a, pre)
        l = v.l
        r = v.r
      let ix = c.emitExpr(sc, i)
      return "(({\n" & pre & "  long _i = (long)(" & ix & ") % 2; " &
             "double __pick = (_i == 0) ? (" & l & ") : (" & r & "); __pick; }))"
    # Inline constant array literal with constant index.
    if a.kind == nkArr and i.kind == nkNum:
      let k = int(i.num)
      if k < 0 or k >= a.kids.len:
        raise newException(ValueError,
          "array index out of range " & errLoc(n))
      return c.emitExpr(sc, a.kids[k])
    # One side of any other pair-valued expression: `cmul(a, b)[0]`,
    # `input()[1]`, `lpf(stereo, 800, 0.5)[0]`.
    if i.kind == nkNum and int(i.num) in {0, 1} and c.refsStereo(sc, a):
      var pre = ""
      let v = c.emitStereo(sc, a, pre)
      let pick = if int(i.num) == 0: v.l else: v.r
      return "(({\n" & pre & "  double __pick = (" & pick & "); __pick; }))"
    raise newException(ValueError,
      "indexing non-constant arrays not supported " & errLoc(n))
  else:
    raise newException(ValueError,
      "cannot emit expression node: " & $n.kind & " " & errLoc(n))

proc emitStereo(c: Ctx; sc: Scope; n: Node; pre: var string): StereoVal =
  # Scalar fast path. If the subtree contains no stereo refs, it has
  # exactly one value — emit it once via emitExpr, bind to a temp via
  # `pre` when the expression may have side effects, and return the
  # temp as both .l and .r. Without the bind, compound scalar results
  # (anything containing phasor/delay/native DSP/etc.) would be inlined
  # textually into both *outL and *outR by the caller and run their
  # state-advancing side effects twice per sample (doubling phasor
  # frequency, delay write-rate, etc.). Literals and bare idents are
  # side-effect-free so we skip the bind.
  if not c.refsStereo(sc, n):
    case n.kind
    of nkNum:
      let s = numLit(n.num)
      return (s, s, false)
    of nkIdent:
      # Same fn-ident fallback as the old nkIdent case — a function
      # identifier has no C value, but we return (name, name) so the
      # caller (a stereo-def inlining pass that captures fn-valued
      # params) can read it.
      if isFnName(c, sc, n.str):
        return (n.str, n.str, false)
      let s = c.emitExpr(sc, n)
      return (s, s, false)
    else:
      let s = c.emitExpr(sc, n)
      let tmp = c.fresh("sst")
      pre.add &"  double {tmp} = ({s});\n"
      return (tmp, tmp, false)

  # refsStereo is true: at least one descendant is stereo. Dispatch by
  # node kind to build a channel-split result. The "both operands
  # scalar" branches that used to live here are unreachable now — if
  # they ever applied, the early-out above caught the whole subtree.
  case n.kind
  of nkIdent:
    let scoped = sc.lookupStereo(n.str)
    if scoped.ok:
      return (scoped.l, scoped.r, true)
    if n.str in c.pairVars:
      return (pairVarRef(n.str, 0), pairVarRef(n.str, 1), true)
    if n.str in c.playSet or n.str in c.stereoLets:
      return (stereoRef(c, n.str, "l"), stereoRef(c, n.str, "r"), true)
    raise newException(ValueError,
      "internal: nkIdent '" & n.str & "' flagged as stereo by refsStereo " &
      "but no stereo binding found " & errLoc(n))
  of nkArr:
    if n.kids.len == 2:
      # Each channel gets its own emission — the two expressions are
      # separate subtrees, so any side effect they share (unlikely but
      # possible) is intended to happen per channel.
      let l = c.emitExpr(sc, n.kids[0])
      let r = c.emitExpr(sc, n.kids[1])
      return (l, r, true)
    raise newException(ValueError,
      "array literal with " & $n.kids.len &
      " elements in stereo context (only [L,R] supported) " & errLoc(n))
  of nkIdx:
    let base = n.kids[0]
    let idx = n.kids[1]
    if base.kind == nkIdent and idx.kind == nkNum and
       int(idx.num) in {0, 1}:
      let stereo = sc.lookupStereo(base.str)
      if stereo.ok:
        let pick = if int(idx.num) == 0: stereo.l else: stereo.r
        return (pick, pick, false)
      if base.str in c.pairVars:
        let pick = pairVarRef(base.str, int(idx.num))
        return (pick, pick, false)
      if base.str in c.playSet or base.str in c.stereoLets:
        let pick = stereoRef(c, base.str, if int(idx.num) == 0: "l" else: "r")
        return (pick, pick, false)
    raise newException(ValueError,
      "stereo-indexed expression must be base[0] or base[1] " & errLoc(n))
  of nkBinOp:
    let a = c.emitStereo(sc, n.kids[0], pre)
    let b = c.emitStereo(sc, n.kids[1], pre)
    let loc = errLoc(n)
    proc combine(op, x, y: string): string =
      case op
      of "+":  &"(({x}) + ({y}))"
      of "-":  &"(({x}) - ({y}))"
      of "*":  &"(({x}) * ({y}))"
      of "/":  &"(({y}) == 0.0 ? 0.0 : ({x}) / ({y}))"
      else: raise newException(ValueError,
              "binop '" & op & "' not allowed on stereo values " & loc)
    # a.l / a.r / b.l / b.r are all either bare temps (from the scalar
    # fast path) or true stereo refs — safe to duplicate textually.
    return (combine(n.str, a.l, b.l), combine(n.str, a.r, b.r), true)
  of nkUnary:
    let a = c.emitStereo(sc, n.kids[0], pre)
    if n.str == "-":
      return ("(-(" & a.l & "))", "(-(" & a.r & "))", true)
    raise newException(ValueError,
      "unary '" & n.str & "' not allowed on stereo " & errLoc(n))
  of nkIf:
    # cond is always scalar (no stereo conditional in aither). Bind it
    # so the ternary body textually referencing condTmp in both L and R
    # doesn't evaluate the condition twice per sample if it contains
    # side-effecting subexpressions.
    let condExpr = c.emitExpr(sc, n.kids[0])
    let condTmp = c.fresh("ifc")
    pre.add &"  double {condTmp} = ({condExpr});\n"
    let thn = c.emitStereo(sc, n.kids[1], pre)
    let els =
      if n.kids[2] != nil: c.emitStereo(sc, n.kids[2], pre)
      else: ("0.0", "0.0", false)
    return (&"(({condTmp}) != 0.0 ? ({thn.l}) : ({els.l}))",
            &"(({condTmp}) != 0.0 ? ({thn.r}) : ({els.r}))", true)
  of nkCall:
    # Pair-returning builtin: flatten the operands (pairs or numbers)
    # into temps via `pre`, then return one C expression per component.
    # analytic and phasor_pair are the stateful entries; input reads the
    # engine's current input frame.
    if n.str in PairCalls:
      case n.str
      of "cmul":
        let a = c.pairOperands(sc, n, pre, 4, "two pairs (or four numbers)")
        return (&"({a[0]} * {a[2]} - {a[1]} * {a[3]})",
                &"({a[0]} * {a[3]} + {a[1]} * {a[2]})", true)
      of "cdiv":
        let a = c.pairOperands(sc, n, pre, 4, "two pairs (or four numbers)")
        let den = c.fresh("cdiv_den")
        pre.add &"  double {den} = {a[2]} * {a[2]} + {a[3]} * {a[3]};\n"
        # Guard /0 by yielding (0, 0) when the divisor pair is the origin.
        # Matches aither's general "divide by zero is zero" convention.
        return (&"({den} == 0.0 ? 0.0 : ({a[0]} * {a[2]} + {a[1]} * {a[3]}) / {den})",
                &"({den} == 0.0 ? 0.0 : ({a[1]} * {a[2]} - {a[0]} * {a[3]}) / {den})", true)
      of "cscale":
        let a = c.pairOperands(sc, n, pre, 3,
                               "a number and a pair (or three numbers)")
        return (&"({a[0]} * {a[1]})", &"({a[0]} * {a[2]})", true)
      of "rotate":
        let a = c.pairOperands(sc, n, pre, 3,
                               "a pair and an angle (or three numbers)")
        let cs = c.fresh("rot_c")
        let sn = c.fresh("rot_s")
        pre.add &"  double {cs} = cos({a[2]});\n"
        pre.add &"  double {sn} = sin({a[2]});\n"
        return (&"({a[0]} * {cs} - {a[1]} * {sn})",
                &"({a[0]} * {sn} + {a[1]} * {cs})", true)
      of "conj":
        # Mirror across the real axis: the same pair turning the other way.
        let a = c.pairOperands(sc, n, pre, 2, "one pair (or two numbers)")
        return (a[0], &"(-{a[1]})", true)
      of "analytic":
        let a = c.pairOperands(sc, n, pre, 1, "one signal")
        let sigT = a[0]
        let offA = c.registerRegion("hilbert_a", 16)
        let offB = c.registerRegion("hilbert_b", 16)
        let reT = c.fresh("an_re")
        let imT = c.fresh("an_im")
        # Negate branchB output: that cascade implements -H[m] under
        # this design's polyphase convention, and `analytic(signal)`
        # is documented to return (signal, +H[signal]).
        pre.add &"  s->idx = {offA};\n"
        pre.add &"  double {reT} = n_hilbert_a((DspState*)s, {sigT});\n"
        pre.add &"  s->idx = {offB};\n"
        pre.add &"  double {imT} = -n_hilbert_b((DspState*)s, {sigT});\n"
        return (reT, imT, true)
      of "phasor_pair":
        # Steinmetz's phasor: a true rotating two-component value at
        # the given rate. (cos, sin) advanced once per call. One pool
        # slot for the phase accumulator; the cos/sin fall out cheaply
        # so the cost is one trig pair per sample, identical to
        # sin(TAU*phasor(r)) + cos(TAU*phasor(r)) but with shared phase.
        let a = c.pairOperands(sc, n, pre, 1, "a frequency")
        let freq = a[0]
        let off = c.registerRegion("phasor_pair", 1)
        let phT = c.fresh("ppair_ph")
        pre.add &"  s->pool[{off}] = fmod(s->pool[{off}] + ({freq}) / s->sr, 1.0);\n"
        pre.add &"  if (s->pool[{off}] < 0.0) s->pool[{off}] += 1.0;\n"
        pre.add &"  double {phT} = s->pool[{off}] * (2.0 * 3.14159265358979323846);\n"
        return (&"cos({phT})", &"sin({phT})", true)
      of "input":
        # The current input frame [L, R]: the capture device live, or a
        # recording under `aither audit ... <input>`. Both sides are
        # plain reads, so referencing them twice is free.
        discard c.pairOperands(sc, n, pre, 0, "no arguments")
        return ("s->in_l", "s->in_r", true)
      else: discard       # exhaustive case below
    # Stereo-returning user def (e.g. haas, pan): inline once, capture
    # the two output channels into fresh temps via the `pre` preamble.
    if n.str in c.userDefs and isStereoDef(c, c.userDefs[n.str]):
      # pan / haas widen mono→stereo. Stereo input has no defined meaning
      # for them and would otherwise descend through the inlining and
      # surface as `unknown identifier: signal` from inside an nkArr
      # element walk. Fail at the call site with a directly actionable
      # message instead.
      if n.str in MonoOnlyInputDefs and n.kids.len > 0 and
         c.refsStereo(sc, n.kids[0]):
        raise newException(ValueError,
          n.str & " takes a mono signal — collapse the input via " &
          "mono(x) or pick a channel via x[0] / x[1] before piping " &
          errLoc(n))
      let def = c.userDefs[n.str]
      if n.kids.len != def.params.len:
        raise newException(ValueError,
          "wrong arg count for " & n.str & ": expected " &
          $def.params.len & " got " & $n.kids.len & " " & errLoc(n))
      if n.str in c.recursive:
        let lp = c.emitLoop(sc, def, n, pair = true)
        pre.add lp.stmts
        return (lp.l, lp.r, true)
      if def.str in c.defStack: raise c.cycleError(def, n)
      let inner = push(sc)
      let tmpL = c.fresh("st_" & n.str & "_l")
      let tmpR = c.fresh("st_" & n.str & "_r")
      var blk = ""
      blk.add &"  double {tmpL}, {tmpR};\n"
      blk.add "  {\n"
      for i, p in def.params:
        let a = n.kids[i]
        if a.kind == nkIdent and isFnName(c, sc, a.str):
          let resolved =
            if sc.lookupFn(a.str).len > 0: sc.lookupFn(a.str) else: a.str
          inner.fns[p] = resolved
          continue
        # Stereo arg → bind param as a stereo local in the inlined body.
        # A pair param (see pairParams) is bound as a pair even when the
        # arg is one number, which then fills both sides.
        var sub = ""
        let sv = c.emitStereo(sc, a, sub)
        pre.add sub
        if sv.stereo or c.pairParams(def)[i]:
          let tmpL = c.fresh("p_" & p & "_l")
          let tmpR = c.fresh("p_" & p & "_r")
          blk.add &"    double {tmpL} = ({sv.l});\n"
          blk.add &"    double {tmpR} = ({sv.r});\n"
          inner.stereoNames[p] = (tmpL, tmpR)
        else:
          let tmp = c.fresh("p_" & p)
          blk.add &"    double {tmp} = ({sv.l});\n"
          inner.names[p] = tmp
          # Literal propagation — same reasoning as emitDefInline.
          if a.kind == nkNum:
            inner.numLits[p] = a.num
          elif a.kind == nkIdent:
            let nv = sc.lookupNumLit(a.str)
            if nv.ok: inner.numLits[p] = nv.val
          if p notin inner.numLits: c.noteInvariant(inner, p, a, sc)
      # Emit body as a block, storing the stereo final value into tmpL/tmpR.
      let bodyNode = def.kids[0]
      let stmts: seq[Node] =
        if bodyNode.kind == nkBlock: bodyNode.kids else: @[bodyNode]
      let lastIdx = stmts.len - 1
      c.defStack.add def.str
      defer: discard c.defStack.pop()
      for si, st in stmts:
        if si == lastIdx and st.kind notin {nkLet, nkVar, nkAssign, nkIdxAssign}:
          var sub = ""
          let v = c.emitStereo(inner, st, sub)
          blk.add sub
          blk.add &"    {tmpL} = ({v.l});\n"
          blk.add &"    {tmpR} = ({v.r});\n"
        else:
          # Same statement handling as a scalar def body (emitLocalStmt):
          # lets, scalar or pair state, assignments, side-effect calls.
          c.emitLocalStmt(inner, st, blk)
      blk.add "  }\n"
      pre.add blk
      return (tmpL, tmpR, true)
    # A def that reads some params as pairs (see pairParams): when every
    # pair-valued arg lands on a pair param there is nothing to split,
    # so inline it once for one result — `mono(input())`. Otherwise the
    # split below runs it per channel and hands pair params the whole pair.
    let pairs =
      if n.str in c.userDefs: c.takesPair(c.userDefs[n.str]) else: @[]
    if pairs.anyIt(it):
      var perChannel = false
      for i, k in n.kids:
        if i < pairs.len and not pairs[i] and c.refsStereo(sc, k):
          perChannel = true
      if not perChannel:
        let v = c.emitExpr(sc, n)
        let tmp = c.fresh("pp")
        pre.add &"  double {tmp} = ({v});\n"
        return (tmp, tmp, false)
    # Otherwise: walk args stereo-aware. At least one arg is stereo
    # (else refsStereo would have caught the whole call at the top).
    var argVals: seq[StereoVal] = @[]
    var anyStereo = false
    for k in n.kids:
      let v = c.emitStereo(sc, k, pre)
      if v.stereo: anyStereo = true
      argVals.add v
    if not anyStereo:
      raise newException(ValueError,
        "internal: nkCall '" & n.str & "' flagged as stereo by refsStereo " &
        "but no arg resolved to stereo " & errLoc(n))
    # One or more stereo args: try to split per channel. Three classes of
    # callee can split safely:
    #   1. Pure arithmetic / shape / libm — duplicating the call has no
    #      hidden state cost.
    #   2. Pure user defs — the body is side-effect-free, so inlining
    #      twice with different params is a no-op state-wise.
    #   3. Transparent helpers (allowlist) — stateful, but each channel
    #      gets ITS OWN fresh state region(s). The codegen path that
    #      registers regions (registerRegion / emitDefInline) runs
    #      independently for the L and R emissions, so a stereo lpf
    #      legitimately pays for two filters.
    # Anything else is a stateful primitive (phasor, noise, dho, …)
    # whose call-site identity carries semantic weight: silently
    # duplicating it across channels would either change the sound
    # (two phasors at slightly different phases) or waste a region.
    let name = n.str
    let canSplit =
      name in Libm1 or name in ["pow", "min", "max", "clamp", "int",
                                 "saw", "tri", "sqr", "abs"] or
      name in TransparentHelpers or
      (name in c.userDefs and isPureForStereo(c, c.userDefs[name].kids[0]))
    if not canSplit:
      var who = "x"
      for i, k in n.kids:
        if argVals[i].stereo and k.kind == nkIdent:
          who = k.str
          break
      raise newException(ValueError,
        "stateful call '" & name & "' receives a stereo value " &
        errLoc(n) & " — split L/R first via " & who & "[0] / " & who &
        "[1], or collapse it with mono(" & who & ")")
    # Substitute per-channel stereo args via fresh scope-bound names,
    # then emit two scalar calls via emitExpr. Synthesised nodes inherit
    # n's line/source so any error raised during the split emission still
    # points at the user's code, not a line-0 ghost.
    #
    # Scalar args reuse the temp emitStereo already bound them to in the
    # `pre` preamble (see argVals[i].l). Without this — i.e., re-using
    # the original AST node — emitExpr would re-emit the whole subtree
    # twice (once per channel), and stateful subexpressions like
    # phasor() would register two extra regions that aren't visible from
    # the source. With this, a `lpf(stereo, 800 + lfo, 0.5)` runs `lfo`
    # exactly once and shares the value across both channel calls.
    let innerL = push(sc)
    let innerR = push(sc)
    var callL = Node(kind: nkCall, str: n.str,
                     line: n.line, source: n.source, kids: @[])
    var callR = Node(kind: nkCall, str: n.str,
                     line: n.line, source: n.source, kids: @[])
    for i, k in n.kids:
      if i < pairs.len and pairs[i] and argVals[i].stereo:
        # Pair param: both channel calls get the whole pair.
        let tmp = c.fresh("spp")
        innerL.stereoNames[tmp] = (argVals[i].l, argVals[i].r)
        innerR.stereoNames[tmp] = (argVals[i].l, argVals[i].r)
        callL.kids.add Node(kind: nkIdent, str: tmp,
                            line: n.line, source: n.source)
        callR.kids.add Node(kind: nkIdent, str: tmp,
                            line: n.line, source: n.source)
      elif argVals[i].stereo:
        let tmpL = c.fresh("stl")
        let tmpR = c.fresh("str")
        innerL.names[tmpL] = argVals[i].l
        innerR.names[tmpR] = argVals[i].r
        callL.kids.add Node(kind: nkIdent, str: tmpL,
                            line: n.line, source: n.source)
        callR.kids.add Node(kind: nkIdent, str: tmpR,
                            line: n.line, source: n.source)
      elif k.kind in {nkNum, nkIdent}:
        # Numeric / ident args have no side effects, AND the downstream
        # codegen for some helpers (delay/fbdelay) inspects the literal
        # node directly to size their state region — so we must keep the
        # original AST node, not replace it with a synthesised tmp ident.
        callL.kids.add k
        callR.kids.add k
      else:
        # Compound scalar arg: emitStereo already bound it to a temp via
        # `pre`. Reference that temp from both channel calls — without
        # this rebind, emitExpr would re-emit the original subtree twice
        # and any stateful subexpression (phasor, noise, etc.) would
        # register two extra regions silently.
        let tmp = c.fresh("ssa")
        innerL.names[tmp] = argVals[i].l
        innerR.names[tmp] = argVals[i].l
        callL.kids.add Node(kind: nkIdent, str: tmp,
                            line: n.line, source: n.source)
        callR.kids.add Node(kind: nkIdent, str: tmp,
                            line: n.line, source: n.source)
    let lExpr = c.emitExpr(innerL, callL)
    let rExpr = c.emitExpr(innerR, callR)
    return (lExpr, rExpr, true)
  else:
    # Any other node kind (e.g. nkLambda reaching emitStereo as a value)
    # is invalid in stereo context — the scalar fast path at the top
    # handles legitimate shapes. Fall through to emitExpr so its own
    # diagnostic (e.g. the nkLambda misuse error) surfaces.
    let s = c.emitExpr(sc, n)
    return (s, s, false)

# Detect whether a subtree references any play name, stereo let, or
# scope-bound stereo name, or calls a stereo-returning user def. Used to
# (1) decide whether a top-level `let` after plays is stereo and (2)
# gate emitStereo's scalar fast path — a refsStereo=false subtree is
# safe to emit once through emitExpr and bound to a temp.
#
# NOTE: takes the Scope so stereo params of an in-progress stereo-def
# inline are visible (their bindings live in sc.stereoNames, not in
# playSet/stereoLets). Without the scope walk, a lambda / inner expr
# inside a stereo-def body that references such a param would look
# scalar, short-circuit through emitExpr, and then raise "unknown
# identifier" because emitExpr can't resolve a scope-bound stereo.
proc refsStereo(c: Ctx; sc: Scope; n: Node): bool =
  if n == nil: return false
  case n.kind
  of nkIdent:
    if sc != nil and sc.scalarShadows(n.str): return false
    if n.str in c.playSet or n.str in c.stereoLets: return true
    if n.str in c.pairVars: return true
    if sc != nil and sc.lookupStereo(n.str).ok: return true
    return false
  of nkArr:
    return n.kids.len == 2
  of nkLambda:
    # Lambda bodies are a scalar scope (v1). Don't peek inside them
    # when deciding whether the enclosing expression is stereo-typed.
    return false
  of nkIdx:
    # Indexing a stereo (play / stereo-let / scoped) with a constant 0
    # or 1 collapses to scalar; only propagate stereo if the index
    # isn't a constant pick. An array element is one number.
    let a = n.kids[0]; let i = n.kids[1]
    if a.kind == nkIdent and (c.lookupSarray(sc, a.str).ok or
                              c.lookupLarray(sc, a.str).ok):
      return c.refsStereo(sc, i)
    if a.kind == nkIdent and i.kind == nkNum and int(i.num) in {0, 1}:
      let isStereoBase = (sc == nil or not sc.scalarShadows(a.str)) and
                         (a.str in c.playSet or a.str in c.stereoLets or
                          a.str in c.pairVars or
                          (sc != nil and sc.lookupStereo(a.str).ok))
      if isStereoBase: return false
    return c.refsStereo(sc, i)   # ignore base; its stereo-ness is consumed
  of nkCall:
    if n.str in PairCalls: return true
    if n.str in PairReaders: return false    # a pair in, one number out
    if n.str == "wave":
      # wave's table is data whatever its length: a two-step sequence
      # is not an [L, R] pair. Only the freq arg can carry stereo.
      return n.kids.len > 0 and c.refsStereo(sc, n.kids[0])
    if n.str in c.userDefs:
      let def = c.userDefs[n.str]
      if isStereoDef(c, def): return true
      # A pair arg on a param that takes one whole (see takesPair) is
      # consumed; only pair args elsewhere make the call per-channel.
      let pairs = c.takesPair(def)
      for i, k in n.kids:
        if i < pairs.len and pairs[i]: continue
        if k != nil and c.refsStereo(sc, k): return true
      return false
    for k in n.kids:
      if k != nil and c.refsStereo(sc, k): return true
    return false
  else:
    for k in n.kids:
      if k != nil and c.refsStereo(sc, k): return true
    return false

# ---- top-level emission ------------------------------------------------

# The Region.scope name for a statement: what it binds, else its role.
proc stmtScope(s: Node; isFinal: bool): string =
  case s.kind
  of nkLet: "let " & s.str
  of nkVar, nkAssign, nkIdxAssign: "set " & s.str
  of nkPlay: "play " & s.str
  else: (if isFinal: "out" else: "expr")

proc emit*(c: Ctx; program: Node): string =
  # First pass: collect top-level vars (including ones nested in play
  # bodies — `var` is name-keyed, play-scope doesn't change that) and
  # top-level lets (play-local lets stay local).
  # Pass 1a: collect every play name up-front. Shadow checks in
  # subsequent passes rely on c.playSet being complete regardless of
  # the play's source position.
  for s in program.kids:
    if s.kind == nkPlay:
      c.playSet.incl s.str

  # A file-level `$x = init` (top level or play body). First declaration
  # wins. A pair init (`$z = [0, 0]`, `$z = phasor_pair(1)`) makes pair
  # state: two struct fields, z__0 and z__1.
  proc declareVar(c: Ctx; n: Node) =
    if n.str in c.playSet:
      raise newException(ValueError,
        "'" & n.str & "' is the name of a play block — " &
        "choose a different variable name " & errLoc(n))
    if n.str in c.topVarSet or n.str in c.pairVars or
       n.str in c.topSarrayNames: return
    c.varDecls.add n
    if isArrayMake(n.kids[0]):
      c.topSarrayNames.incl n.str
    elif c.isStereoReturn(n.kids[0]):
      c.pairVars.incl n.str
      c.topVarNames.add pairVarField(n.str, 0)
      c.topVarNames.add pairVarField(n.str, 1)
    else:
      c.topVarSet.incl n.str
      c.topVarNames.add n.str
  proc scanVars(c: Ctx; n: Node) =
    if n == nil: return
    case n.kind
    of nkVar:
      c.declareVar(n)
    of nkPlay:
      let body = n.kids[0]
      if body.kind == nkBlock:
        for k in body.kids: c.scanVars(k)
      else: c.scanVars(body)
    else: discard
  for s in program.kids:
    case s.kind
    of nkVar:
      c.declareVar(s)
    of nkLet:
      if s.str in c.playSet:
        raise newException(ValueError,
          "'" & s.str & "' is the name of a play block — " &
          "choose a different variable name " & errLoc(s))
      c.topLets.incl s.str
      if s.str notin c.letLines: c.letLines[s.str] = s.line
    of nkPlay:
      let body = s.kids[0]
      if body.kind == nkBlock:
        for k in body.kids: c.scanVars(k)
      else: c.scanVars(body)
    else: discard

  # Prelude
  var pre = ""
  pre.add "#include <math.h>\n"
  pre.add "typedef struct DspState DspState;\n"
  # extern declarations for every dsp.nim primitive we might call.
  pre.add "extern double n_lpf(DspState*,double,double,double);\n"
  pre.add "extern double n_hpf(DspState*,double,double,double);\n"
  pre.add "extern double n_bpf(DspState*,double,double,double);\n"
  pre.add "extern double n_notch(DspState*,double,double,double);\n"
  pre.add "extern double n_lp1(DspState*,double,double);\n"
  pre.add "extern double n_hp1(DspState*,double,double);\n"
  # Hoisted forms (coefficient once, step per sample).
  pre.add "extern double n_lp1_coef(double,double);\n"
  pre.add "extern double n_lp1_c(DspState*,double,double);\n"
  pre.add "extern double n_hp1_c(DspState*,double,double);\n"
  pre.add "extern void n_svf_coef(double,double,double,double*);\n"
  pre.add "extern double n_lpf_c(DspState*,double,const double*);\n"
  pre.add "extern double n_hpf_c(DspState*,double,const double*);\n"
  pre.add "extern double n_bpf_c(DspState*,double,const double*);\n"
  pre.add "extern double n_notch_c(DspState*,double,const double*);\n"
  pre.add "extern void n_reverb_coef(double,double,double*);\n"
  pre.add "extern double n_reverb_c(DspState*,double,double,const double*);\n"
  pre.add "extern double n_delay(DspState*,double,double,double);\n"
  pre.add "extern double n_fbdelay(DspState*,double,double,double,double);\n"
  pre.add "extern double n_reverb(DspState*,double,double,double);\n"
  pre.add "extern double n_impulse(DspState*,double);\n"
  pre.add "extern double n_resonator(DspState*,double,double,double);\n"
  pre.add "extern double n_dho(DspState*,double,double,double);\n"
  pre.add "extern double n_dho_v(DspState*,double,double,double);\n"
  pre.add "extern double n_discharge(DspState*,double,double);\n"
  pre.add "extern double n_tremolo(DspState*,double,double,double);\n"
  pre.add "extern double n_slew(DspState*,double,double);\n"
  pre.add "extern double n_wave(DspState*,double,double*,int);\n"
  pre.add "extern double n_hilbert_a(DspState*,double);\n"
  pre.add "extern double n_hilbert_b(DspState*,double);\n"
  pre.add "extern double n_freq_shift(DspState*,double,double);\n"
  pre.add "extern double shape_saw(double);\n"
  pre.add "extern double shape_tri(double);\n"
  pre.add "extern double shape_sqr(double);\n"
  pre.add "extern double n_midi_cc(int);\n"
  pre.add "extern double n_midi_note(int);\n"
  pre.add "extern double n_midi_freq(void);\n"
  pre.add "extern double n_midi_gate(void);\n"
  pre.add "extern double n_midi_trig(DspState*,int);\n"
  pre.add "extern double n_midi_voice_freq(int);\n"
  pre.add "extern double n_midi_voice_gate(int);\n"
  pre.add "extern double n_midi_voice_vel(int);\n"
  pre.add "extern double n_midi_voice_key(int);\n"
  pre.add "extern double n_midi_vel(void);\n"
  pre.add "extern double n_input_l(void);\n"
  pre.add "extern double n_input_r(void);\n"
  pre.add "extern long n_overrun(void*,const char*);\n"
  pre.add "extern double n_now(void);\n"
  # VoiceState — DspState prefix is *exactly* the Nim layout; new fields
  # go after.
  # NOTE: voice.nim depends on this exact layout for state migration:
  #   [DspState prefix (sizeof DspState = 4194328 bytes)]
  #   [double t, dt, start_t]  (3*8 = 24 bytes, header ends at 4194352)
  #   [double v_<name0>, v_<name1>, ...]  (one slot per top-level var)
  #   [unsigned char inited[N]]  (one byte per top-level var)
  pre.add "typedef struct {\n"
  pre.add "  double pool[524288];\n"
  pre.add "  long   idx;\n"
  pre.add "  char   overflow;\n"
  pre.add "  double sr;\n"
  pre.add "  double t, dt, start_t;\n"
  pre.add "  double* part_gains;\n"
  pre.add "  double* part_out;\n"
  pre.add "  double* part_park;\n"
  # Block mode (tick_block): the engine hands the per-sample work over.
  # Mirrored field for field by VoiceHeader in voice.nim.
  pre.add "  double* part_fade_delta;\n"     # per-part gain ramp, per sample
  pre.add "  double* part_fade_target;\n"
  pre.add "  unsigned char* part_park_pending;\n"   # park once the fade-out lands (Nim bools)
  pre.add "  double* part_peak;\n"           # per-part meters, kept in C
  pre.add "  double* part_sq;\n"
  pre.add "  double* in_buf;\n"              # block input, interleaved L R (NULL: silence)
  pre.add "  double  in_l, in_r;\n"          # this sample's input()
  pre.add "  double  fade_gain, fade_delta;\n"   # the voice's own fade ramp
  pre.add "  double  time_sec, time_frac;\n"     # the engine clock, advanced per sample
  pre.add "  double  peak_decay, rms_alpha;\n"   # meter constants
  pre.add "  double  peak_raw;\n"            # loudest |sample| before the fade, this block
  pre.add "  long    bad;\n"                 # non-finite samples this block (zeroed)
  pre.add "  long    n_parts;\n"
  # Recursion's guard: the engine's deadline for this block (monotonic
  # seconds, 0 for none); a recursion that gives up sets `stuck` and
  # leaves its name and line in `stuck_at`.
  pre.add "  double  deadline;\n"
  pre.add "  long    stuck;\n"
  pre.add "  const char* stuck_at;\n"
  # `aither profile`: while `profiling` is set, tick1 adds each top-level
  # statement's time to its slot in `prof` (slots in source order, named
  # by prof_names[]) and counts its calls in `prof_ticks`.
  pre.add "  double* prof;\n"
  pre.add "  long    profiling;\n"
  pre.add "  long    prof_ticks;\n"
  for vn in c.topVarNames:
    pre.add "  double v_" & vn & ";\n"
  pre.add "  unsigned char inited[" & $max(1, c.topVarNames.len) & "];\n"
  pre.add "} VoiceState;\n\n"

  # Body of tick()
  var body = ""
  body.add "static void tick1(VoiceState* s, double* outL, double* outR) {\n"
  body.add "  s->idx = 0;\n"
  body.add "  double prof_last = 0.0;\n"
  body.add "  if (s->profiling) { prof_last = n_now(); s->prof_ticks++; }\n"
  var profNames: seq[string]
  let topSc = push(nil)
  # Lazy-init file-level state, top level and play bodies alike, in
  # declaration order. (Play-body inits used to be skipped, so `$x = 0.1`
  # inside a play started at 0.) A pair inits both of its fields.
  # File-level `$x = init` (top level or play body) lazy-inits where it
  # is declared, so its init can read the lets above it. Only the first
  # declaration of a name inits; later ones are plain state.
  proc varInit(c: Ctx; sc: Scope; st: Node): string =
    var isFirst = false
    for d in c.varDecls:
      if d == st: isFirst = true
    if not isFirst: return ""
    var initPre = ""
    if st.str in c.topSarrayNames:
      c.declareStateArray(sc, st, initPre, topLevel = true)
      return initPre
    if st.str in c.pairVars:
      let i0 = c.topVarNames.find(pairVarField(st.str, 0))
      let i1 = i0 + 1
      let v = c.emitStereo(sc, st.kids[0], initPre)
      &"  if (!(s->inited[{i0}] & 1)) {{\n" & initPre &
        &"    {pairVarRef(st.str, 0)} = ({v.l});\n" &
        &"    {pairVarRef(st.str, 1)} = ({v.r});\n" &
        &"    s->inited[{i0}] |= 1; s->inited[{i1}] |= 1;\n  }}\n"
    else:
      let i = c.topVarNames.find(st.str)
      let init = c.scalarValue(sc, st.kids[0], initPre, "$" & st.str)
      &"  if (!(s->inited[{i}] & 1)) {{\n" & initPre &
        &"    s->v_{st.str} = ({init}); s->inited[{i}] |= 1;\n  }}\n"

  # Top-level statements: let -> local; var handled above (init) and via
  # assign below; def -> skipped (inlined at call sites); final expression
  # -> set outputs.
  var finalIdx = -1
  for i in countdown(program.kids.len - 1, 0):
    let s = program.kids[i]
    # `each(...)` is a statement (its value is 0), never the output: a
    # top-level `each` must not silence a file of play blocks.
    let isEach = s.kind == nkCall and s.str == "each"
    if s.kind notin {nkDef, nkVar, nkLet, nkAssign, nkIdxAssign, nkPlay, nkImport} and not isEach:
      finalIdx = i; break

  for i, s in program.kids:
    # Map generated-C line back to aither source. Best-effort: puts the
    # directive at a statement boundary; errors inside inlined expressions
    # still report under their enclosing top-level line. TCC honors these.
    # Prefer the per-node source tag so a stdlib-originating statement
    # reports as stdlib in TCC errors, not as the user's patch path.
    let srcFile =
      if s.source.len > 0: s.source else: c.patchPath
    if srcFile.len > 0 and s.line > 0:
      body.add &"#line {s.line} \"{srcFile}\"\n"
    c.scope = stmtScope(s, i == finalIdx)
    if i > 0 and program.kids[i - 1].kind == nkLet:
      c.letsDone.incl program.kids[i - 1].str
    case s.kind
    of nkDef: discard              # inlined at call sites
    of nkImport:
      raise newException(ValueError,
        "import \"" & s.str & "\" was not resolved — the program was built " &
        "without import expansion " & errLoc(s))
    of nkVar: body.add c.varInit(topSc, s)
    of nkLet:
      # Runtime-valued array (`let a = [p, q, r]`, `let a = array(N)`):
      # a per-sample local array.
      if (s.kids[0].kind == nkArr and s.kids[0].kids.len != 2 and
          s.kids[0].kids.len > 0 and
          not s.kids[0].kids.allIt(it.kind == nkNum)) or
         isArrayMake(s.kids[0]):
        c.declareLocalArray(topSc, s.str, s.kids[0], body, topLevel = true)
      # Numeric array literal -> hoist to a static const, record it.
      elif s.kids[0].kind == nkArr and
         s.kids[0].kids.allIt(it.kind == nkNum):
        let arrKids = s.kids[0].kids
        let sym = "arr_" & s.str
        var items: seq[string] = @[]
        for k in arrKids: items.add numLit(k.num)
        c.arrayDecls.add &"static const double {sym}[{arrKids.len}] = {{" &
          items.join(", ") & "};\n"
        c.topArrays[s.str] = (sym, arrKids.len)
        # Two numbers are also a pair value, the same as inside a play:
        # `let c = [0.3, 0.1]` then `c * x` or `cmul(z, c)`.
        if arrKids.len == 2:
          body.add &"  double l_{s.str}_l = {items[0]};\n"
          body.add &"  double l_{s.str}_r = {items[1]};\n"
          c.stereoLets.incl s.str
      elif c.refsStereo(topSc, s.kids[0]):
        var pre = ""
        let v = c.emitStereo(topSc, s.kids[0], pre)
        body.add pre
        body.add &"  double l_{s.str}_l = ({v.l});\n"
        body.add &"  double l_{s.str}_r = ({v.r});\n"
        c.stereoLets.incl s.str
      else:
        let v = c.emitExpr(topSc, s.kids[0])
        body.add &"  double l_{s.str} = ({v});\n"
        # Literal propagation (top-level lets): enables `let H = 16`
        # followed by `sum(H, ...)` — H flows through to sum as a
        # compile-time constant.
        if s.kids[0].kind == nkNum:
          topSc.numLits[s.str] = s.kids[0].num
        elif s.kids[0].kind == nkIdent:
          let nv = topSc.lookupNumLit(s.kids[0].str)
          if nv.ok: topSc.numLits[s.str] = nv.val
        if s.str notin topSc.numLits: c.noteInvariant(topSc, s.str, s.kids[0], topSc)
    of nkAssign:
      if s.str in c.pairVars:
        body.add c.assignPair(topSc,
          (pairVarRef(s.str, 0), pairVarRef(s.str, 1)), s.kids[0])
      else:
        let target =
          if s.str in c.topVarSet: "s->v_" & s.str
          elif s.str in c.topLets: "l_" & s.str
          else: raise newException(ValueError,
                  "unknown assign: " & s.str & " " & errLoc(s))
        var pre = ""
        let rhs = c.scalarValue(topSc, s.kids[0], pre, "$" & s.str)
        body.add pre
        body.add &"  {target} = ({rhs});\n"
    of nkIdxAssign:
      body.add c.emitIdxAssign(topSc, s)
    of nkPlay:
      # A play's lets and computed value live inside a `{...}` block so
      # the lets can't collide with other plays or later code. Each play
      # also registers a gain slot (s->part_gains[partIdx]) applied to
      # the value. Bodies may be scalar (mirrored to both channels) or
      # stereo ([L,R] literal / referencing earlier plays).
      let partIdx = c.playNames.len
      c.playNames.add s.str
      c.playSet.incl s.str
      let inner = push(topSc)
      body.add &"  double p_{s.str}_l, p_{s.str}_r;\n"
      # A parked part (see `park`) is silent and its body doesn't run, so
      # it costs nothing and its state waits where it was.
      body.add &"  if (s->part_park && s->part_park[{partIdx}] != 0.0) {{\n" &
               &"    p_{s.str}_l = 0.0; p_{s.str}_r = 0.0;\n" &
               &"    if (s->part_out) {{ s->part_out[{2*partIdx}] = 0.0; " &
               &"s->part_out[{2*partIdx+1}] = 0.0; }}\n  }} else {{\n"
      let bodyNode = s.kids[0]
      let stmts: seq[Node] =
        if bodyNode.kind == nkBlock: bodyNode.kids else: @[bodyNode]
      let lastIdx = stmts.len - 1
      for si, st in stmts:
        c.scope = "play " & s.str & "/" & stmtScope(st, si == lastIdx)
        if si == lastIdx and st.kind notin {nkLet, nkVar, nkAssign, nkIdxAssign}:
          var sub = ""
          let v = c.emitStereo(inner, st, sub)
          body.add sub
          body.add &"    p_{s.str}_l = ({v.l}) * s->part_gains[{partIdx}];\n"
          body.add &"    p_{s.str}_r = ({v.r}) * s->part_gains[{partIdx}];\n"
          # Each part's output for the engine's per-part meters (`parts`).
          body.add &"    if (s->part_out) {{ s->part_out[{2*partIdx}] = p_{s.str}_l; " &
                   &"s->part_out[{2*partIdx+1}] = p_{s.str}_r; }}\n"
        else:
          case st.kind
          of nkLet:
            # Shadow guard: a play-local let that reuses a play name is
            # almost always a typo. Without this, `let bass = ...` inside
            # another play captures a scalar while later references to
            # `bass` go through the stereo play-name path, producing a
            # misleading type error deep in the expression walker.
            if st.str in c.playSet:
              raise newException(ValueError,
                "'" & st.str & "' is the name of a play block — " &
                "choose a different local variable name " & errLoc(st))
            # Numeric-literal array: auto-hoist so wave() (and indexing)
            # can treat it like a top-level array. Pre-fix the user had to
            # move the array above the play block; now it can live next to
            # the play that uses it. Length 2 is also a stereo pair (see
            # `[L, R]` lets), so it goes on to the stereo branch as well.
            let rhs = st.kids[0]
            let numericArr = rhs.kind == nkArr and rhs.kids.len > 0 and
                             rhs.kids.allIt(it.kind == nkNum)
            if numericArr:
              let sym = c.fresh("arr_" & s.str & "_" & st.str)
              var items: seq[string] = @[]
              for k in rhs.kids: items.add numLit(k.num)
              c.arrayDecls.add &"static const double {sym}[{rhs.kids.len}] = {{" &
                items.join(", ") & "};\n"
              inner.arrays[st.str] = (sym, rhs.kids.len)
            # A non-numeric array of length != 2, or array(N): a local
            # array of this sample's numbers (see declareLocalArray).
            if (rhs.kind == nkArr and rhs.kids.len != 2 and rhs.kids.len > 0 and
                not numericArr) or isArrayMake(rhs):
              c.declareLocalArray(inner, st.str, rhs, body)
            elif numericArr and rhs.kids.len != 2:
              discard
            # Stereo-producing RHS (haas, pan, arr literal, or references
            # to a prior stereo local): emit two locals, register name
            # as a stereo let for downstream emission.
            elif c.refsStereo(inner, st.kids[0]):
              var sub = ""
              let v = c.emitStereo(inner, st.kids[0], sub)
              body.add sub
              body.add &"    double l_{st.str}_l = ({v.l});\n"
              body.add &"    double l_{st.str}_r = ({v.r});\n"
              c.stereoLets.incl st.str
            else:
              let v = c.emitExpr(inner, st.kids[0])
              let tmp = "l_" & st.str
              body.add &"    double {tmp} = ({v});\n"
              inner.names[st.str] = tmp
              # Literal propagation (play-local lets).
              let cv = c.constValue(inner, st.kids[0])
              if cv.ok: inner.numLits[st.str] = cv.val
              else: c.noteInvariant(inner, st.str, st.kids[0], inner)
          of nkVar:
            body.add c.varInit(inner, st)
          of nkAssign:
            if st.str in c.pairVars:
              body.add c.assignPair(inner,
                (pairVarRef(st.str, 0), pairVarRef(st.str, 1)), st.kids[0])
            else:
              let target =
                if st.str in c.topVarSet: "s->v_" & st.str
                elif inner.lookup(st.str).len > 0: inner.lookup(st.str)
                else: raise newException(ValueError,
                  "unknown assignment target '" & st.str & "' " & errLoc(st))
              var sub = ""
              let rhs = c.scalarValue(inner, st.kids[0], sub, "$" & st.str)
              body.add sub
              body.add &"    {target} = ({rhs});\n"
          of nkIdxAssign:
            body.add c.emitIdxAssign(inner, st)
          else:
            # Non-final expression statements run for their side effects
            # (the value is dropped). This matters for stateful builtins
            # where the user expects mutation: `array_set(arr, i, v)`,
            # `array_push(arr, v)`. Without `(void)(...)` they'd silently
            # vanish and the array would never update.
            if c.refsStereo(inner, st):
              var sub = ""
              let v = c.emitStereo(inner, st, sub)
              body.add sub
              body.add &"    (void)({v.l}); (void)({v.r});\n"
            else:
              let v = c.emitExpr(inner, st)
              body.add &"    (void)({v});\n"
      body.add "  }\n"
    else:
      if i == finalIdx:
        var pre = ""
        let v = c.emitStereo(topSc, s, pre)
        body.add pre
        body.add &"  *outL = ({v.l}); *outR = ({v.r});\n"
      else:
        # Non-final bare expression at top level: run for side effects.
        # `array_set`, `array_push`, and similar mutators are useful
        # statements; without this they'd compile to nothing.
        if c.refsStereo(topSc, s):
          var pre = ""
          let v = c.emitStereo(topSc, s, pre)
          body.add pre
          body.add &"  (void)({v.l}); (void)({v.r});\n"
        else:
          let v = c.emitExpr(topSc, s)
          body.add &"  (void)({v});\n"
    # `aither profile`: charge the time since the last statement to this
    # one's slot. Every top-level statement and play has its own, in
    # source order, so `profile` can name the costly line.
    if s.kind notin {nkDef, nkImport}:
      let loc = (if s.source.len > 0 and s.source != c.patchPath: s.source & " " else: "line ") & $s.line
      if profNames.len < MaxProfSlots:
        profNames.add stmtScope(s, i == finalIdx) & " (" & loc & ")"
      body.add &"  if (s->profiling) {{ double prof_now = n_now(); " &
               &"s->prof[{profNames.len - 1}] += prof_now - prof_last; prof_last = prof_now; }}\n"
  # Which parts the final expression reaches: the names it mentions,
  # and the names those parts' bodies mention, and so on. A part it
  # never reaches computes every sample and is never heard; `check`
  # and `audit` say so by name, since forgetting to add a new part to
  # the last line is the commonest silent mistake in a set.
  c.unheard = @[]
  if finalIdx >= 0 and c.playNames.len > 0:
    var plays = initTable[string, Node]()
    for s in program.kids:
      if s.kind == nkPlay: plays[s.str] = s
    var reached = initHashSet[string]()
    proc mention(n: Node) =
      if n == nil: return
      if n.kind == nkIdent and n.str in plays and n.str notin reached:
        reached.incl n.str
        mention(plays[n.str].kids[0])
      for k in n.kids: mention(k)
    mention(program.kids[finalIdx])
    for name in c.playNames:
      if name notin reached: c.unheard.add name
  # No final expression: a file of play blocks plays all of them, rather
  # than compiling to silence.
  let heard = c.playNames
  if finalIdx < 0 and heard.len > 0:
    body.add "  *outL = " & heard.mapIt("p_" & it & "_l").join(" + ") & ";\n"
    body.add "  *outR = " & heard.mapIt("p_" & it & "_r").join(" + ") & ";\n"
  body.add "}\n\n"
  # The slots' names, for `aither profile` (read with getSymbol).
  body.add "const char* const prof_names[] = {" &
           profNames.mapIt("\"" & it.multiReplace(("\\", "\\\\"), ("\"", "\\\"")) & "\"").join(", ") &
           (if profNames.len == 0: "0" else: "") & "};\n"
  body.add &"const long prof_count = {profNames.len};\n\n"
  body.add BlockTemplate

  # The hoisted coefficients: statics, and the init both entry points
  # call (once per block in tick_block, per call in tick).
  var hoistC = ""
  for h in c.hoists: hoistC.add h.decl
  hoistC.add "static void hoist_init(VoiceState* s) {\n"
  for h in c.hoists: hoistC.add h.init
  hoistC.add "  (void)s;\n}\n\n"

  pre & c.arrayDecls & hoistC & body

# Assign per-type base offsets (in first-seen order), rewrite each
# region's intra-offset into an absolute pool slot, and back-patch the
# placeholders left in the emitted C. Also enforces the pool budget.
proc finaliseLayout(c: Ctx; src: var string) =
  var base = 0
  var bases = initTable[string, int]()
  for t in c.typeOrder:
    bases[t] = base
    base += c.typeCumSize[t]
  if base > PoolSize:
    raise newException(ValueError,
      "patch needs " & $base & " pool slots, pool is " & $PoolSize &
      " — reduce delays / reverbs or raise DspPoolSize")
  for r in c.regions.mitems:
    r.offset = bases[r.typeName] + r.offset
  for rid, r in c.regions:
    src = src.replace(offsetPlaceholder(rid), $r.offset)


# A default stands on its own: numbers, PI/TAU, operators and calls,
# never a name from some scope (whose scope would it be?).
proc checkDefault(c: Ctx; def: Node; e: Node) =
  case e.kind
  of nkNum: discard
  of nkIdent:
    if e.str notin ["PI", "TAU"]:
      raise newException(ValueError,
        "default for a parameter of " & def.str & " can't use the name '" &
        e.str & "': a default is numbers, PI/TAU, operators and calls " &
        errLoc(e))
  of nkCall, nkBinOp, nkUnary, nkIf:
    for k in e.kids: c.checkDefault(def, k)
  else:
    raise newException(ValueError,
      "default for a parameter of " & def.str & " must be an expression " &
      errLoc(e))

# Named arguments and defaults are resolved here, once, so the rest of
# codegen only ever sees a call with one positional argument per
# parameter: `sing(220, g, vibrato = 0.3)` becomes the full list, each
# missing argument filled with the def's default expression.
# Builtins' parameters, named as SPEC writes them, so any call can name
# its arguments: `reverb(sig, rt60 = 2.4, wet = 0.3)`. `name=v` has a
# default. Stdlib functions are defs and name theirs already.
const BuiltinParams = {
  "lp1": @["sig", "cut"], "hp1": @["sig", "cut"],
  "lpf": @["sig", "cut", "res"], "hpf": @["sig", "cut", "res"],
  "bpf": @["sig", "cut", "res"], "notch": @["sig", "cut", "res"],
  "delay": @["sig", "time", "max_time"],
  "fbdelay": @["sig", "time", "max_time", "fb"],
  "reverb": @["sig", "rt60", "wet"],
  "impulse": @["freq"], "resonator": @["sig", "freq", "decay"],
  "discharge": @["sig", "rate"],
  "dho": @["force", "freq", "damp"], "dho_v": @["force", "freq", "damp"],
  "tremolo": @["sig", "rate", "depth"], "slew": @["sig", "time"],
  "wave": @["freq", "arr"], "phasor": @["freq"],
  "pow": @["a", "b"], "min": @["a", "b"], "max": @["a", "b"],
  "clamp": @["x", "l", "h"],
  "array": @["n", "init=0", "every=0"],
}.toTable

proc reorderArgs(n: Node; what: string; params: seq[string];
                 defaults: seq[Node]) =
  ## A call's arguments in parameter order: positional first, then named,
  ## missing ones from `defaults` (nil: required).
  var args = newSeq[Node](params.len)
  var pos = 0
  var seenNamed = false
  for k in n.kids:
    if k.kind == nkNamedArg:
      seenNamed = true
      let i = params.find(k.str)
      if i < 0:
        raise newException(ValueError,
          what & " has no parameter '" & k.str & "' (it takes " &
          params.join(", ") & ") " & errLoc(k))
      if args[i] != nil:
        raise newException(ValueError,
          what & ": '" & k.str & "' given twice " & errLoc(k))
      args[i] = k.kids[0]
    else:
      if seenNamed:
        raise newException(ValueError,
          what & ": positional arguments go before named ones " & errLoc(k))
      if pos >= params.len:
        raise newException(ValueError,
          "wrong arg count for " & what & ": expected " &
          $params.len & " got " & $n.kids.len & " " & errLoc(n))
      args[pos] = k
      inc pos
  for i, a in args:
    if a == nil:
      let d = if i < defaults.len: defaults[i] else: nil
      if d == nil:
        raise newException(ValueError,
          what & ": missing '" & params[i] & "' " & errLoc(n))
      args[i] = d
  n.kids = args

proc normalizeCalls(c: Ctx; n: Node; shadow: HashSet[string]) =
  ## `shadow`: parameter names in scope. Inside `def inharmonic(freq,
  ## ratio, ...)`, `ratio(n)` calls the parameter, not a top-level def
  ## that happens to be called ratio, so it isn't checked against one.
  ## Calls that name an argument become positional here, defs and
  ## builtins alike, so codegen only ever sees positional calls.
  if n == nil: return
  var inner = shadow
  if n.kind in {nkDef, nkLambda}:
    for p in n.params: inner.incl p
  for k in n.kids: c.normalizeCalls(k, inner)
  if n.kind == nkDef:
    for d in n.defaults:
      if d != nil: c.checkDefault(n, d)
  if n.kind != nkCall or n.str in shadow: return
  var anyNamed = false
  for k in n.kids:
    if k.kind == nkNamedArg: anyNamed = true
  if n.str notin c.userDefs:
    if not anyNamed: return
    if n.str notin BuiltinParams:
      raise newException(ValueError,
        n.str & " takes its arguments by position " & errLoc(n))
    var params: seq[string]
    var defaults: seq[Node]
    for p in BuiltinParams[n.str]:
      let eq = p.find('=')
      if eq < 0:
        params.add p
        defaults.add nil
      else:
        params.add p[0 ..< eq]
        defaults.add Node(kind: nkNum, num: parseFloat(p[eq + 1 .. ^1]))
    reorderArgs(n, n.str, params, defaults)
    return
  let def = c.userDefs[n.str]
  if not anyNamed and n.kids.len == def.params.len: return
  reorderArgs(n, def.str, def.params, def.defaults)

proc generateFull*(program: Node; patchPath: string = "";
                   sr: float64 = 48000.0):
    tuple[csrc: string; varNames, partNames, unheard: seq[string];
          regions: seq[Region]] =
  let c = newCtx(program)
  c.patchPath = patchPath
  c.sr = sr
  c.normalizeCalls(program, initHashSet[string]())
  c.markRecursion()
  var src = c.emit(program)
  finaliseLayout(c, src)
  (src, c.topVarNames, c.playNames, c.unheard, c.regions)

proc generate*(program: Node; patchPath: string = "";
               sr: float64 = 48000.0):
    tuple[csrc: string; varNames, partNames: seq[string];
          regions: seq[Region]] =
  ## generateFull without the unheard list (what the tests destructure).
  let r = generateFull(program, patchPath, sr)
  (r.csrc, r.varNames, r.partNames, r.regions)
