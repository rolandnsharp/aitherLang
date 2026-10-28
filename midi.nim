## MIDI, which is C (midi.c, alsa_midi.c), as the Nim tests see it until
## they are C too (notes/c-port-2026-10-28.md): the writer side, and
## whether the input thread is alive.

import std/os

{.compile: "midi.c".}
{.compile: "alsa_midi.c".}
{.passL: "-lasound".}

const MidiH = currentSourcePath().parentDir / "midi.h"

const MaxPolyphony* = 16       # midi.h's MIDI_MAX_POLYPHONY

proc cNoteOn(note, velocity: cint) {.importc: "midi_note_on", header: MidiH.}
proc cNoteOff(note: cint) {.importc: "midi_note_off", header: MidiH.}
proc cCc(cc, value: cint) {.importc: "midi_cc", header: MidiH.}
proc cResetAll() {.importc: "midi_reset_all", header: MidiH.}
proc cSubscriptionActive(): cint {.importc: "midi_subscription_active", header: MidiH.}
proc cLastConnectInfo(): cstring {.importc: "midi_last_connect_info", header: MidiH.}
proc cResubscribe(): cstring {.importc: "midi_resubscribe_if_dropped", header: MidiH.}
proc c_free(p: pointer) {.importc: "free", header: "<stdlib.h>".}

proc midiNoteOn*(note, velocity: int) = cNoteOn(cint(note), cint(velocity))
proc midiNoteOff*(note: int) = cNoteOff(cint(note))
proc midiCc*(cc, value: int) = cCc(cint(cc), cint(value))
proc midiResetAll*() = cResetAll()
proc midiSubscriptionActive*(): bool = cSubscriptionActive() != 0
proc lastConnectInfo*(): string = $cLastConnectInfo()

proc midiResubscribeIfDropped*(): string =
  ## "" when there was nothing to do, else "ok: ..." or "fail: ...".
  let p = cResubscribe()
  if p == nil: return ""
  result = $p
  c_free(p)
