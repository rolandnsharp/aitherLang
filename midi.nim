## MIDI input as sampled signals. Events (note-on/off, CC) arrive on a
## dedicated thread and update a process-global MidiState; generated DSP
## code polls that state at audio rate via the five `n_midi_*` natives.
##
## Design:
##   * Single writer (MIDI thread), many readers (audio thread tick).
##     All fields are std/atomics; moRelaxed is enough because each slot
##     is independent and the audio side tolerates sub-sample staleness.
##   * State is engine-owned, not voice-owned: hot-reloading a patch
##     doesn't lose a held note or a knob's last position.
##   * `midi_trig(n)` is the only primitive with per-voice pool state
##     (one slot tracks the last-seen global trig counter for note n).

import std/[atomics, math]
import dsp

const OverflowSlotMidi = DspPoolSize - 128

type
  MidiState* = object
    cc*:        array[128, Atomic[float64]]
    notes*:     array[128, Atomic[float64]]
    trigs*:     array[128, Atomic[uint32]]   # monotonic counter per note
    lastFreq*:  Atomic[float64]
    lastGate*:  Atomic[float64]
    lastNote*:  Atomic[int32]                # which note currently holds gate

var midiState*: MidiState

# ---- writer API (called from MIDI thread or tests) ----------------------

proc midiNoteOn*(note, velocity: int) =
  if note < 0 or note >= 128: return
  let vel = float64(velocity) / 127.0
  midiState.notes[note].store(vel)
  let hz = 440.0 * pow(2.0, (float64(note) - 69.0) / 12.0)
  midiState.lastFreq.store(hz)
  midiState.lastGate.store(vel)
  midiState.lastNote.store(int32(note))
  discard midiState.trigs[note].fetchAdd(1'u32)

proc midiNoteOff*(note: int) =
  if note < 0 or note >= 128: return
  midiState.notes[note].store(0.0)
  # Only drop the mono gate if this was the most recent note-on — prevents
  # a stale note-off from killing a newer held note.
  if midiState.lastNote.load() == int32(note):
    midiState.lastGate.store(0.0)

proc midiCc*(cc, value: int) =
  if cc < 0 or cc >= 128: return
  midiState.cc[cc].store(float64(value) / 127.0)

proc midiResetAll*() =
  for i in 0 ..< 128:
    midiState.cc[i].store(0.0)
    midiState.notes[i].store(0.0)
    midiState.trigs[i].store(0'u32)
  midiState.lastFreq.store(0.0)
  midiState.lastGate.store(0.0)
  midiState.lastNote.store(-1'i32)

# ---- natives invoked from TCC-compiled C --------------------------------

proc nMidiCc*(n: cint): cdouble {.cdecl, exportc: "n_midi_cc".} =
  if n < 0 or n >= 128: return 0.0
  midiState.cc[n].load()

proc nMidiNote*(n: cint): cdouble {.cdecl, exportc: "n_midi_note".} =
  if n < 0 or n >= 128: return 0.0
  midiState.notes[n].load()

proc nMidiFreq*(): cdouble {.cdecl, exportc: "n_midi_freq".} =
  midiState.lastFreq.load()

proc nMidiGate*(): cdouble {.cdecl, exportc: "n_midi_gate".} =
  midiState.lastGate.load()

# Per-voice edge detect. Pool slot holds the last-seen global counter for
# note n; emit 1.0 on the sample when the counter advanced. Consume-once
# semantics across samples; all readers in the same sample see the edge.
proc nMidiTrig*(s: var DspState; n: cint): cdouble
               {.cdecl, exportc: "n_midi_trig".} =
  if n < 0 or n >= 128: return 0.0
  if s.idx + 1 > OverflowSlotMidi:
    s.overflow = true
    return 0.0
  let i = s.idx
  s.idx += 1
  let cur = float64(midiState.trigs[n].load())
  let prev = s.pool[i]
  s.pool[i] = cur
  if cur > prev: 1.0 else: 0.0
