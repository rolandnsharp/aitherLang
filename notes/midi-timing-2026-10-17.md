# MIDI timing: where the jitter is, and how to remove it if it matters

**Status: parked, by choice.** Discussed 2026-10-17 (Roland, with
Claude). Roland has played the keyboard live and never noticed jitter,
so nothing is built. This note is for the day it does get noticed:
what causes it, the free workaround, and the fix, already designed.

## The short version

- A MIDI event (a key, a knob, a pad) can land up to one block late,
  and how late varies from event to event. At the default block of
  512 frames that is up to 11.6 ms at 44.1 kHz (10.7 ms at 48 kHz).
- **Workaround, no code:** a smaller block. `AITHER_BUFFER=128` gives
  up to 2.9 ms, below what players notice, for some CPU per sample.
- **Fix, designed, not built:** timestamp each event when it arrives
  and apply it at its exact sample. Jitter becomes a constant latency
  of one block; timing is sample-accurate.
- The MIDI **API stays as it is** either way (`midi_cc`, `midi_keyboard`
  with `(freq, gate, vel, key)`, and the rest). Only the engine's
  internals would change.

## Where the jitter comes from

The engine computes audio in bursts. `pumpPipe` mixes a block of
`AITHER_BUFFER` frames, writes it, and blocks until the player takes
it; computing a block takes a fraction of the block's duration, and
the rest is waiting. MIDI arrives on its own thread (`midi.nim`, the ALSA
sequencer), which writes the knob and note tables (atomics) the
moment an event comes in. The voices read those tables per sample,
but every sample of a block is computed inside that one burst, so an
event that arrives while the engine is waiting is picked up by the
next burst, at the first sample of a block, whenever in the previous
block it actually happened. That's quantising to blocks, with a delay
that varies by up to one block: jitter.

Since 2026-10-17 knobs are also hoisted: `midi_cc` is read once at
the start of each block (`hoist_init`). That is the same quantisation
made explicit, not a new one.

The player's own buffer (`tools/play ... 20`, 20 ms) adds latency on
top, but a constant one: it delays everything equally, so it doesn't
add jitter.

| AITHER_BUFFER | worst-case jitter at 44.1 kHz |
|---|---|
| 512 (default) | 11.6 ms |
| 256 | 5.8 ms |
| 128 | 2.9 ms |
| 64 | 1.5 ms |

Who would notice: tight percussive playing (finger drumming on pads,
fast repeated notes) is where 10 ms of jitter is audible. Sustained
sounds, pads and leads hide it in their attack.

## The workaround

```
AITHER_BUFFER=128 ./aither set.aither | tools/play hw:rockchipes8388c 20
```

The engine's per-block work (voice stats, the ring, the per-block
bookkeeping) runs four times as often; the per-sample work is the
same. Measure the set with `aither audit` / `try` first on the
uConsole, where cores are scarce. `tools/card` sets `AITHER_BUFFER` to
its own period, the `PERIOD` constant (512) in `tools/card.c`; under
`card`, smaller blocks mean changing that constant.

## The fix: timestamped events, applied at their sample

1. **Stamp on arrival.** Give the engine's ALSA sequencer subscription
   a queue with real-time timestamps, so every event carries the time
   it arrived (or stamp it with `CLOCK_MONOTONIC` in the MIDI thread
   as it is read). Push `(time, event)` into a lock-free single-
   producer queue instead of writing the tables directly.
2. **Map time to samples.** The engine knows when each block starts
   in the same clock (the player paces it; `tools/card` gives the
   card's own clock and the wall time of frame 0). Each event is due
   at `arrival + D`, where D is a fixed delay of one block, so every
   event is in the future of the block that will play it.
3. **Split the block at events.** When mixing a block, take the events
   due inside it, sorted, and for each one run the voices up to its
   sample (`tickBlock` for k frames), apply the event to the tables,
   and continue. Voices already advance a block at a time, so this
   needs no compiler change; a block with no events runs as now.
4. **Knobs.** A knob event splits the block like a note, and
   `hoist_init` runs again at the split, so hoisted coefficients see
   the new value from that sample on.

What it buys: constant latency (one block plus the player's buffer)
and sample-accurate placement, which a smaller block only
approximates. What it costs: a queue and a split loop in the
engine's innermost path, which is why it waits until someone hears
the problem.

## Decided along the way

**Keep the ALSA sequencer and the MIDI API.** The question was
whether MIDI should go Unix-style, like audio (float pairs on
stdin/stdout, hardware in small tools). Other systems:

| System | How MIDI gets in |
|---|---|
| SuperCollider | in the language: `MIDIdef.cc(\cut, {\|v\| ...}, 74)` |
| ChucK | in the language: `MidiIn min; min.recv(msg)`, sample-accurate |
| Pure Data / Max | objects: `[notein]`, `[ctlin 74]` |
| Csound | opcodes (`ctrl7`); notes start instrument instances |
| Faust | on the control: `hslider("cutoff[midi:ctrl 74]", ...)`; voices get `freq`, `gain`, `gate` |
| Sonic Pi | built in, as events like `/midi:*/note_on` |
| TidalCycles | no MIDI in: OSC for everything |

Nearly all of them read MIDI from the operating system directly, as
aither does. And the ALSA sequencer already is the Unix-y part: any
program can be a source (`tools/pad`, a Minilab, a DAW), and
`aconnect` wires sources to destinations the way `|` wires processes.
Reading it is a standard system interface, like stdin, not hardware.

Rejected:

- **A text event protocol over the control socket** (`on 60 0.8`,
  `cc 20 0.5`, sent by tools or a `feed` command). It reinvents the
  sequencer, cuts aither off from other MIDI software, and `aither
  feed` was a clumsy middleman.
- **Renaming `midi_*` to `knob` / `keyboard` / `note_*`.** MIDI is
  knobs, sliders and drum pads; `knob` would misname two of the three.
- **Events and audio in one pipe.** Different kinds of stream (sparse
  and irregular, against fixed-rate and sample-synchronous); the mic
  stays on stdin as audio.

**`tools/keys` (the uConsole's letter keys as a MIDI keyboard):
parked.** Only needed to play keyboard patches with no MIDI keyboard
plugged in. If wanted, fold it into `tools/pad` (one tool for all of
the uConsole's controls: the gamepad sends knobs, a letter row sends
notes) rather than a second program.
