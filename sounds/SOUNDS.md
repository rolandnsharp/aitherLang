# aither sound library

A growing catalogue of complete, reusable **voices** — full instruments
extracted from working patches, ready to copy-paste into any patch.

This is **not** a primitive library. Primitives live in
`/stdlib.aither` (oscillators, filters, envelopes, pair operations).
This is a library of **assembled sounds** — each one a self-contained
voice with its own controls, ready to drop into a patch alongside other
voices.

## How to use a sound

aither has no `include` mechanism. Each sound here is a snippet meant to
be **copy-pasted** into your patch:

1. Open the sound file you want.
2. Copy the `let`s, any `def`s, and the `play` block into your patch.
3. If the sound's CC numbers collide with other voices in your patch,
   rebind by changing the `midi_cc(N)` calls at the top.
4. Add the voice's name (e.g. `bodhran`) to your master mix sum.

Each sound's file header documents:
- What it sounds like
- Its CC controls (default bindings)
- Output format (mono / stereo, recommended gain)
- Any `def`s it shares with other sounds

If two sounds use the same `def` name (e.g. both hand pans use
`panTone`), declare it **once** at the top of your patch and skip the
duplicate.

## The catalogue

### Drums

- **[gaelic_bodhran](gaelic_bodhran.aither)** — Irish frame drum, self-running 12-step rhythm. One knob morphs heartbeat → jig → reel; second knob takes the wood thump from clean to fat saturated. The "drum that sounds like a player" pattern from COMPOSING.md.

### Tuned percussion / pitched

- **[four_level_handpan](four_level_handpan.aither)** — D-Kurd hand pan with a 4-level rhythm + melody ladder on one knob. Each level is a different note sequence AND velocity pattern, not just density — going up the ladder progresses both rhythm and melody. Per-voice sample-and-hold prevents pitch sliding during decay.

## What makes a sound library-worthy

A snippet earns a place here when it is:

1. **Self-contained** — nothing required from the parent patch except the
   master mix sum. **No dependencies between sounds.** If sound A and
   sound B both need a `panTone` def, each declares its own copy. Sounds
   in this library must be safe to chop, change, or delete in isolation
   without breaking other sounds.
2. **Live-controllable** — exposes one or more CCs that radically change
   the sound (paradigm crossfade, pattern morph, drive, etc.) without
   breaking musicality.
3. **Distinctive** — not just a sine + envelope; a sound that earned its
   shape by being interesting in a real patch.
4. **Documented** — header explains what it is, what controls it, what
   to watch for. Headers must NOT reference other sound files — the
   library is a flat catalogue of independent voices.

Sounds are extracted **from working patches**, not invented in isolation.
The patches in `/patches/` are the proving ground; this library is where
the proven ones get crystallised for reuse.

## Next candidates

Sounds in patches that are good enough to extract but haven't been yet:

- **acid_complex K1 paradigm crossfade** (acid → bell → noise on one knob)
- **Tesla-organ FM swarm** (the gothic fm_swarm recipe with per-partial drift)
- **freq_shift halo** (coherent inharmonicity on a held drone)
- **Single-pan velocity ladder** (handpan from gaelic_ladder, 3-zone)
- **Aither voice cumulative-layer pad** (Tesla drone → melody → halo →
  phase fractal, the additive-layer pattern)

These should move into the catalogue as their patches stabilise.
