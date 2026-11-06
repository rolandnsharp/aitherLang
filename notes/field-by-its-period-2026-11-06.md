# A field by what it does: the size is the engine's job

**Status (2026-11-06): built, as `ring(period = T)`** (SPEC "Fields").
Both rings use it. From 59 to 123 places, across rates, `aether_ring`'s
squeeze reaches a quarter round at 0.160 to 0.163 s and its twist at 0.370
to 0.375 s. Settled from "Open" below: 1, the engine picks the grain, a
quarter of a place a step, so a knob can go four times faster; 2 and 4,
`+=` on a ring says what changes and the engine owns the stepping, with
rates a round, the ring its own clock, and no `dt` or `t` in its lines
(Roland: "there is a dt in the patch when there is no t in field theory");
3, one period and a ratio; 5, `ring` for now. `scanned_ring`'s sound
matched at every check. Still open: a fast fine field, which needs more
than one place a sample, and whether the engine's step should be better
than the plain one it is. From a conversation in
a forked session, for the main one. It follows
[nabla-and-medium-2026-11-06.md](nabla-and-medium-2026-11-06.md) and SPEC
"Fields" (`cfbff97`). Roland: "love the idea that it can be worked out
engine job".

## The problem

Roland asked what a field theorist is thinking when they write
`array(128)`. Nothing they'd recognise. A field is continuous; the number
is how finely the machine cuts it. It is machine detail in the patch, the
same kind of thing as `here`, and it does more harm than `here` did,
because it silently sets the physics. A push goes `sqrt(stiff)` places a
step and a step is every N samples, so a wave takes

    T = N² / (sqrt(stiff) * sr)      seconds to go round

N is squared. With the same `stiff`, 128 places to 8192 is about 4000
times slower. Changing the grain changes the instrument.

## The proposal

**The patch says what the medium does. The engine works out the places
and the stiffness.** A field is described by its period, the time a push
takes to go round: Tesla's word, from
[step-first-2026-11-05.md](step-first-2026-11-05.md), and a thing you can
hear.

```
# today: the machine's numbers
$x = array(128)
let stiff = 0.05
$v = ($v - concentration($x) * stiff - $x * held) * (1 - loss)

# proposed: the medium's, in a sketch only; the word and form are open
$x = ring(period = 1.6)
$v = ($v - concentration($x) - $x * held) * (1 - loss)
```

The engine picks N and scales `concentration` so a wave goes round in the
period, whatever N it chose. Changing the grain leaves the sound's motion
alone, and N leaves the patch.

The rule, extending "no here, no now": **the engine owns places, the sweep,
the wrap, stepping and the grain. The patch owns what the medium is made
of, how it changes, and what touches it.**

## What the arithmetic says

From `T` above, with c = sqrt(stiff) places a step, and c at most 1 for the
step to be stable:

    c = N² / (T * sr)        so        N <= sqrt(T * sr)

| period | finest N, one place a sample, 44.1 kHz |
|---|---|
| 0.1 s | 66 |
| 1 s | 210 |
| 10 s | 664 |
| 60 s | 1627 |

**This qualifies a claim already made.** SPEC, COMPOSING and the notes say
a field costs one place "however large". The cost is true, and measured
(0.5 s and 0.44 s for 10 s of sound at 128 and 8192). But at one place a
sample a large field is a very slow one: 8192 masses at this stiffness take
about 6800 s to go round, so the 8192 run measured the cost of a medium
nobody would hear moving. Period and grain trade against each other, and a
fast, fine field needs more than one place a sample, which costs more.
That is the real shape of the cost question, and the engine is the right
place for it: given a period and a grain, it steps as many places a sample
as they need, and says so if the machine can't.

`scanned_ring` today: 128 places at `stiff` 0.05 gives 1.66 s round, at
0.45 gives 0.55 s.

## Open

1. **How much grain to ask for.** Perhaps nothing: the engine picks the
   finest the period allows. Or a patch says how fine a ripple the medium
   carries, which is physical (a chain of masses has one).
2. **The other constants.** `held` and `loss` are per step today, so they
   move with N too. They need to be per second, or a period of their own: a
   ring's decay is another thing you can hear.
3. **Two speeds.** `aether_ring` has a squeeze and a twist. Two periods, or
   one and a ratio.
4. **The law apart from its stepping.** This is that open question from the
   ∇ note, arriving: `concentration` would mean a rate, and the engine owns
   the step. How far that goes, and whether `dt` in a field line means the
   field's step, needs working out.
5. **The word,** `ring(period = ...)` or another. `array` stays for data.
6. **Keeping the sound.** Both rings must stay the same when moved to this,
   so the scaling has to be checked against them.

## Edges, while here

The same split answers edges (not built, not tested). Ends needn't be a
language idea: the field stays a ring, and a tied end is a stretch that is
very stiffly held, an absorbing one a stretch that loses more, both the
medium differing from place to place. A free end is more awkward, needing
the coupling to stop there, so a stiffness varying by place. Reflection then
comes out of the physics, as refraction does.
