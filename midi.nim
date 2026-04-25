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

import std/[atomics, math, os, strutils]
import dsp

{.compile: "alsa_midi.c".}
{.passL: "-lasound".}

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

# ---- ALSA seq backend ---------------------------------------------------
# The C wrapper in alsa_midi.c isolates us from libasound's unions and
# bitfields. Our side runs a single thread that blocks in snd_seq_event_input
# and dispatches to the writer API above.

type
  NoteOnFn*  = proc (note, vel: cint) {.cdecl.}
  NoteOffFn* = proc (note: cint) {.cdecl.}
  CcFn*      = proc (cc, value: cint) {.cdecl.}

proc aither_alsa_open(name: cstring): cint {.importc, cdecl.}
proc aither_alsa_connect_from(client, port: cint): cint {.importc, cdecl.}
proc aither_alsa_list_ports(buf: cstring; size: cint): cint {.importc, cdecl.}
proc aither_alsa_auto_connect(buf: cstring; size: cint): cint {.importc, cdecl.}
proc aither_alsa_parse_address(spec: cstring; client, port: ptr cint): cint
  {.importc, cdecl.}
proc aither_alsa_run(onOn: NoteOnFn; onOff: NoteOffFn; onCc: CcFn)
  {.importc, cdecl.}
proc aither_alsa_close() {.importc, cdecl.}

# Callbacks that the C thread invokes directly. They must be {.cdecl,
# gcsafe.} — they touch only atomic globals so the GC is never reached.
proc cbNoteOn(note, vel: cint) {.cdecl.} = midiNoteOn(int(note), int(vel))
proc cbNoteOff(note: cint) {.cdecl.} = midiNoteOff(int(note))
proc cbCc(cc, value: cint) {.cdecl.} = midiCc(int(cc), int(value))

var midiThread: Thread[void]
var midiThreadStarted: bool
# Subscription bookkeeping. Tracks the last successful connect so we
# can auto-resubscribe after an unexpected event-loop exit (the failure
# mode BUGS_AND_ISSUES.md issue 4c described — Minilab silently stops
# triggering after a few patch reloads, but `aither midi list` still
# shows the port).
var midiThreadActive: Atomic[bool]            # set true before thread run; false on exit
var midiShuttingDown: Atomic[bool]            # suppresses drop log on intentional close
var lastConnectInfo*: string = ""              # human-readable spec (for `list`)
var lastConnectClient: int = -1                # ALSA client id of the last subscribe
var lastConnectPort: int = -1                  # ALSA port id of the last subscribe

proc midiRunLoop() {.thread, nimcall.} =
  aither_alsa_run(cbNoteOn, cbNoteOff, cbCc)
  midiThreadActive.store(false)
  if not midiShuttingDown.load():
    stderr.writeLine "[aither] MIDI subscription dropped — next `aither send` " &
                     "will attempt auto-resubscribe"

# Public API --------------------------------------------------------------

proc midiOpen*(clientName = "aither"): bool =
  ## Open the ALSA seq client and create an input port. No subscription
  ## yet — the caller picks a source via midiAutoConnect or midiConnect.
  ## Returns false if libasound isn't available / seq can't be opened.
  aither_alsa_open(cstring(clientName)) == 0

proc midiListPorts*(): string =
  ## Render all readable ALSA seq ports as "client:port\tname" lines.
  ## Returns an empty string if nothing is available.
  var buf = newString(4096)
  let n = aither_alsa_list_ports(cstring(buf), cint(buf.len))
  if n <= 0: return ""
  buf.setLen(buf.cstring.len)
  buf

proc midiAutoConnect*(): string =
  ## Subscribe to the first connectable input port; return a human-readable
  ## identifier ("Device - Port (client:port)"), or "" if nothing found.
  var buf = newString(256)
  if aither_alsa_auto_connect(cstring(buf), cint(buf.len)) == 0:
    buf.setLen(buf.cstring.len)
    lastConnectInfo = buf
    # Parse "name (client:port)" trailer. Storing the numeric address
    # gives midiResubscribe something to call without re-running the
    # full first-port discovery.
    let lp = buf.rfind('(')
    let rp = buf.rfind(')')
    if lp > 0 and rp > lp + 1:
      let inner = buf[lp+1 ..< rp]
      let colon = inner.find(':')
      if colon > 0:
        try:
          lastConnectClient = parseInt(inner[0 ..< colon])
          lastConnectPort = parseInt(inner[colon+1 .. ^1])
        except ValueError: discard
    return buf
  ""

proc midiConnect*(spec: string): bool =
  ## Subscribe to a specific port; spec is ALSA's "client:port" form,
  ## either numeric ("28:0") or client-name ("Minilab3:0").
  var client, port: cint
  if aither_alsa_parse_address(cstring(spec), addr client, addr port) != 0:
    return false
  if aither_alsa_connect_from(client, port) != 0:
    return false
  lastConnectClient = int(client)
  lastConnectPort = int(port)
  lastConnectInfo = spec & " (" & $client & ":" & $port & ")"
  true

proc midiStartThread*() =
  ## Spawn the blocking input thread. Idempotent.
  if midiThreadStarted: return
  midiShuttingDown.store(false)
  midiThreadActive.store(true)
  createThread(midiThread, midiRunLoop)
  midiThreadStarted = true

proc midiShutdown*() =
  ## Close the sequencer (which unblocks the input thread) and join.
  if not midiThreadStarted: return
  midiShuttingDown.store(true)
  aither_alsa_close()
  joinThread(midiThread)
  midiThreadStarted = false

proc midiSubscriptionActive*(): bool =
  ## True when the input loop is alive and the last subscription is
  ## current. False after a drop or before the first connect.
  midiThreadActive.load()

proc midiResubscribeIfDropped*(): string =
  ## If the input thread has exited unexpectedly AND we have a stored
  ## connect address, close + reopen + re-subscribe + restart. Returns
  ## a status string ("" = nothing to do, "ok ..." = recovered,
  ## "fail ..." = couldn't recover) for the caller to log.
  if midiThreadActive.load(): return ""
  if lastConnectClient < 0: return ""
  # Thread is gone but the seq may still be open — close cleanly first.
  midiShutdown()
  if not midiOpen():
    return "fail: ALSA seq open"
  if aither_alsa_connect_from(cint(lastConnectClient),
                              cint(lastConnectPort)) != 0:
    return "fail: subscribe to " & $lastConnectClient & ":" & $lastConnectPort
  midiStartThread()
  "ok: resubscribed to " & lastConnectInfo
