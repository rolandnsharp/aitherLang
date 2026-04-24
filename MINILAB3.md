# Arturia MiniLab 3 — MIDI mapping reference

Captured live with `aseqdump -p "Minilab3:0"` while wiggling each control.
The factory Arturia mapping is non-standard (sliders aren't simply CC 80-83
in order), so we record it here to avoid re-probing.

## Controls

| Hardware     | MIDI       | Confirmed | Notes                          |
|--------------|------------|-----------|--------------------------------|
| Slider 1     | CC 82      | yes       |                                |
| Slider 2     | CC 83      | yes       |                                |
| Slider 3     | CC 85      | yes       |                                |
| Slider 4     | CC 17      | yes       | non-adjacent to other sliders  |
| Knob 1       | CC 74      | yes       | filter cutoff convention       |
| Knob 2       | CC 71      | yes       | filter resonance convention    |
| Knob 3       | CC 76      | yes       |                                |
| Knob 4       | CC 77      | yes       |                                |
| Knob 5       | CC 93      | yes       | **INVERTED** — max at left CCW |
| Knob 6       | CC 18      | yes       |                                |
| Knob 7       | CC 19      | yes       |                                |
| Knob 8       | CC 16      | yes       |                                |
| Mod wheel    | CC 1       | standard  |                                |
| Pitch wheel  | pitchbend  | standard  | not exposed in aither v1       |
| Pads 1-8     | notes 36-43| standard  | Bank A; pads are velocity      |
| Keys (25)    | notes 36-60| transposable | shares note range with pads |

## How to probe a control

```bash
aseqdump -p "Minilab3:0" 2>&1 | grep "Control change"
# wiggle the control — read the CC number from the output
```

`aseqdump` works alongside the running engine — ALSA seq supports
multiple subscribers. If you see no output, make sure you're moving
the control during the capture window.

## Patch-level idiom

```aither
let knob1   = midi_cc(74)        # filter cutoff
let slider1 = midi_cc(82)        # didge gain etc.

play synth:
  let cut = 200 + knob1 * knob1 * 5000
  osc(saw, midi_freq()) |> lpf(cut, 0.8) * adsr(midi_gate(), 0.01, 0.2, 0.7, 0.4) * 0.3
```

## Notes / quirks

- **CCs only update on movement.** A slider's value at engine startup is 0,
  not its physical position. Wiggle each control once after `aither start`
  so the values match the hardware.
- **Slider CCs are 82, 83, 85, 17** — sliders 1-3 are adjacent (82/83/85, with 84
  skipped) and slider 4 jumps to CC 17. Knob 8 is also CC 16, so the bottom-row
  knob 8 + slider 4 use the unrelated CC range 16-17 instead of continuing 84-86.
- **Knob 5 is INVERTED.** Sends 127 at full counter-clockwise, 0 at full clockwise.
  Wrap with `1 - midi_cc(93)` in patches.
- **Pads share note numbers with low keys.** The default 25-key range starts
  at note 36 (C2), so pressing low keys triggers the same drums as the pads.
  Either avoid the lowest octave on the keys, or transpose pads up a bank.
