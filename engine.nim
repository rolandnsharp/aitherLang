## aither engine — audio callback + socket CLI for live coding

import std/[os, net, strutils, math, locks, tables]
import parser, voice, miniaudio, midi
import engine_types, cli_output, analysis
export engine_types

const
  SampleRate    = 48000'u32
  Channels      = 2'u32
  BufferFrames  = 512'u32
  MaxVoices     = 16
  SocketPath    = "/tmp/aither.sock"
  DefaultFadeMs = 20.0

const Stdlib = staticRead("stdlib.aither")

const EnvBinSamples = 2400  # 50ms per bin at 48 kHz

# Per-voice rolling buffer of the most recent ~0.5s of post-fade
# stereo samples. Read by `aither spectrum` for offline-style FFT
# analysis on the live engine state. float32 keeps the cost small
# (~190 KB per voice, ~3 MB total at MaxVoices, plus the master).
const RecentSamples = 24000

type
  RecentRing = object
    l, r: array[RecentSamples, float32]
    idx: int
const PeakDecay = 0.99993   # ~300ms exponential decay
const RmsAlpha  = 1.04e-4   # ~200ms smoothing

type
  Stats = object
    peakL, peakR:     float64
    rmsSqL, rmsSqR:   float64
    clips:            int
    envRing:          array[EnvBins, float32]
    envBinIdx:        int
    envBinMax:        float32
    envCount:         int

  Slot = object
    name:       string
    voice:      NativeVoice
    active:     bool
    muted:      bool
    fadeGain:   float64
    fadeDelta:  float64
    stats:      Stats
    recent:     RecentRing  # rolling 0.5s buffer for `spectrum`
    nanLogged:  bool        # one log line per voice per session, then quiet

  # Public data structs (VoiceInfo, StatsSnapshot, etc.) live in
  # engine_types.nim — exported up so callers see them through engine.

proc update(s: var Stats; gl, gr: float64) {.inline.} =
  let al = abs(gl)
  let ar = abs(gr)
  s.peakL = max(s.peakL * PeakDecay, al)
  s.peakR = max(s.peakR * PeakDecay, ar)
  s.rmsSqL = s.rmsSqL * (1.0 - RmsAlpha) + al * al * RmsAlpha
  s.rmsSqR = s.rmsSqR * (1.0 - RmsAlpha) + ar * ar * RmsAlpha
  if al > 1.0 or ar > 1.0: inc s.clips
  let mAbs = max(al, ar).float32
  if mAbs > s.envBinMax: s.envBinMax = mAbs
  inc s.envCount
  if s.envCount >= EnvBinSamples:
    s.envRing[s.envBinIdx] = s.envBinMax
    s.envBinIdx = (s.envBinIdx + 1) mod EnvBins
    s.envBinMax = 0
    s.envCount = 0

var
  slots: array[MaxVoices, Slot]
  slotCount: int
  master: Stats
  masterRecent: RecentRing
  running: bool
  timeSec: float64
  timeFrac: float64
  mtx: Lock

initLock(mtx)

# --------------------------------------------------------------- audio callback

proc audioCallback(output: ptr UncheckedArray[cfloat], frameCount: cuint,
                   userData: pointer) {.cdecl.} =
  let dt = 1.0 / float64(SampleRate)
  let frames = int(frameCount)

  if not tryAcquire(mtx):
    for i in 0 ..< frames * int(Channels):
      output[i] = 0.cfloat
    return

  let sc = slotCount
  for i in 0 ..< frames:
    timeFrac += dt
    if timeFrac >= 1.0:
      timeSec += 1.0
      timeFrac -= 1.0
    let t = timeSec + timeFrac

    var lMix = 0.0
    var rMix = 0.0
    for v in 0 ..< sc:
      if not slots[v].active or slots[v].muted: continue
      var l = 0.0
      var r = 0.0
      let s = slots[v].voice.tick(t)
      l = s.l; r = s.r
      # NaN/Inf defense: a single bad sample (unstable filter, runaway
      # resonator, sqrt of a negative, etc.) would otherwise propagate
      # forever through the voice's pool state. Reset the pool, drop
      # this sample's contribution, and log once per voice so the user
      # learns about the issue without spamming 48000 lines/sec.
      if l != l or r != r or l > 1e6 or r > 1e6 or l < -1e6 or r < -1e6:
        slots[v].voice.resetPool()
        if not slots[v].nanLogged:
          stderr.writeLine "[aither] voice " & slots[v].name &
                           " produced NaN/Inf — pool reset"
          slots[v].nanLogged = true
        continue
      slots[v].fadeGain = clamp(
        slots[v].fadeGain + slots[v].fadeDelta, 0.0, 1.0)
      if slots[v].fadeGain <= 0.0 and slots[v].fadeDelta < 0.0:
        slots[v].active = false
      # Advance per-part gains toward their targets (clamped 0..1). When
      # a fade reaches its target, snap to it and clear the delta.
      let voice = slots[v].voice
      for p in 0 ..< voice.partGains.len:
        let d = voice.partFadeDeltas[p]
        if d != 0.0:
          let target = voice.partFadeTargets[p]
          let newG = voice.partGains[p] + d
          if (d > 0.0 and newG >= target) or (d < 0.0 and newG <= target):
            voice.partGains[p] = target
            voice.partFadeDeltas[p] = 0.0
          else:
            voice.partGains[p] = clamp(newG, 0.0, 1.0)
      let gl = l * slots[v].fadeGain
      let gr = r * slots[v].fadeGain
      update(slots[v].stats, gl, gr)
      # Per-voice rolling buffer for `aither spectrum`. Wraps in place;
      # snapshot reads from idx outward (oldest sample first).
      slots[v].recent.l[slots[v].recent.idx] = float32(gl)
      slots[v].recent.r[slots[v].recent.idx] = float32(gr)
      slots[v].recent.idx = (slots[v].recent.idx + 1) mod RecentSamples
      lMix += gl
      rMix += gr

    update(master, lMix, rMix)
    masterRecent.l[masterRecent.idx] = float32(lMix)
    masterRecent.r[masterRecent.idx] = float32(rMix)
    masterRecent.idx = (masterRecent.idx + 1) mod RecentSamples
    output[i * 2]     = cfloat(tanh(lMix))
    output[i * 2 + 1] = cfloat(tanh(rMix))

  release(mtx)

# ------------------------------------------------------------- voice management

proc findSlot(name: string): int =
  for i in 0 ..< slotCount:
    if slots[i].name == name: return i
  -1

# Compact stopped voices out of the slots array so they don't count
# against MaxVoices. Caller must hold mtx — modifies slots / slotCount.
# Inactive slots are the ones whose fade-out completed (audio callback
# set active=false). Without sweeping, a session that stops 16 voices
# without re-sending exhausts the table even though no voice is
# audible. Order of remaining active slots is preserved.
proc sweepInactiveSlots() =
  var dst = 0
  for src in 0 ..< slotCount:
    if slots[src].active:
      if dst != src:
        slots[dst] = slots[src]
      inc dst
  for i in dst ..< slotCount:
    slots[i] = Slot()
  slotCount = dst

proc fadeDeltaFor(seconds: float64): float64 =
  let s = if seconds <= 0.0: DefaultFadeMs / 1000.0 else: seconds
  1.0 / (s * float64(SampleRate))

# Scan a codegen error message for `(source:line)` locations and append
# the text of the referenced source line, so the user sees the offending
# code in the response instead of having to open the file and count.
# Stdlib is baked in at compile-time; only the user's patch text is
# passed in. Other sources are ignored. The first match is enough — if
# codegen raised on a nested construct, later locations usually repeat
# the same anchor.
proc annotateErr(msg, userSrc, userPath: string): string =
  # Look for a "(name:line)" token where name ends at ':' and line is an
  # int. A bare `(line N)` (untagged source) passes through unchanged.
  var i = 0
  while i < msg.len:
    let lp = msg.find('(', i)
    if lp < 0: break
    let rp = msg.find(')', lp + 1)
    if rp < 0: break
    let inner = msg[lp+1 ..< rp]
    let colon = inner.find(':')
    if colon > 0:
      let src = inner[0 ..< colon]
      let lineStr = inner[colon+1 .. ^1]
      var lineNum = 0
      try: lineNum = parseInt(lineStr)
      except ValueError: lineNum = 0
      if lineNum > 0:
        let body =
          if src == "stdlib": Stdlib
          elif src == userPath: userSrc
          else: ""
        if body.len > 0:
          let lines = body.split('\n')
          if lineNum <= lines.len:
            return msg & "\n  " & src & ":" & $lineNum & "  " &
                   lines[lineNum - 1].strip()
        return msg
    i = rp + 1
  msg

proc loadPatch*(filename: string; fadeIn: float64): string =
  if not fileExists(filename): return "file not found: " & filename
  let userSrc = readFile(filename)
  let baseName = splitFile(filename).name

  stderr.write "loading " & extractFilename(filename) & " ... "

  # Parse stdlib and user source separately so error line numbers
  # reference the user's file directly (not the combined source).
  var stdlibAst: parser.Node
  var userAst: parser.Node
  try:
    stdlibAst = parseProgram(Stdlib)
    userAst = parseProgram(userSrc)
  except ParseError as e:
    stderr.writeLine "parse FAIL"
    return "parse error: " & e.msg

  # Tag every node with its source origin so codegen errors (and the
  # `#line` directives feeding TCC) can point at the right file. Without
  # this, a stdlib-sourced error reports under the user's patch path and
  # sends the user hunting through their own code for a fault that's in
  # stdlib (or in their invocation of a stdlib def).
  setSource(stdlibAst, "stdlib")
  setSource(userAst, filename)

  # Merge: stdlib's top-level statements come first, then the user's.
  let program = parser.Node(
    kind: parser.nkBlock,
    kids: stdlibAst.kids & userAst.kids,
    line: 1)

  # Heavy phase — compile + alloc — runs OUTSIDE the audio mutex. Taking
  # the mutex here was causing buffer drops when TCC compile took >10 ms
  # on big patches. Now the audio callback only blocks briefly (~µs) on
  # the swap below.
  var prepared: Prepared
  try:
    prepared = prepare(program, float64(SampleRate), filename)
  except CatchableError as e:
    stderr.writeLine "compile FAIL"
    return "compile error: " & annotateErr(e.msg, userSrc, filename)

  acquire(mtx)
  # Drop slots whose fade-out completed before deciding hot-swap vs new
  # vs voice-limit-reached. Without this, a session that stopped 16
  # voices without re-sending still saw "voice limit reached" even
  # though every slot was silent (issue: voice slot leak on stop).
  sweepInactiveSlots()
  let idx = findSlot(baseName)
  let now = timeSec + timeFrac
  if idx >= 0:
    slots[idx].voice.commit(prepared)
    slots[idx].nanLogged = false           # fresh diagnostics for the new code
    let retrigger = (not slots[idx].active) or
                    slots[idx].fadeGain <= 0.0 or
                    slots[idx].fadeDelta < 0.0       # interrupting a fade-out
    if retrigger:
      slots[idx].voice.startT = now
      slots[idx].active = true
      slots[idx].fadeGain = if fadeIn > 0.0: 0.0 else: 1.0
      slots[idx].fadeDelta = if fadeIn > 0.0: fadeDeltaFor(fadeIn) else: 0.0
    elif fadeIn > 0.0:
      slots[idx].fadeDelta = fadeDeltaFor(fadeIn)
    release(mtx)
    stderr.writeLine "ok (" & (if retrigger: "retrigger " else: "hot-swap ") & baseName & ")"
  else:
    if slotCount >= MaxVoices:
      release(mtx)
      return "voice limit reached (" & $MaxVoices & ")"
    let voice = newVoice(float64(SampleRate))
    voice.commit(prepared)
    voice.startT = now
    slots[slotCount] = Slot(
      name: baseName, voice: voice, active: true,
      fadeGain: (if fadeIn > 0.0: 0.0 else: 1.0),
      fadeDelta: (if fadeIn > 0.0: fadeDeltaFor(fadeIn) else: 0.0))
    inc slotCount
    release(mtx)
    stderr.writeLine "ok (new " & baseName & ")"
  # Auto-resubscribe MIDI if the input thread had silently died (issue
  # 4c). Cheap when nothing dropped (returns "" immediately); on actual
  # recovery we log so the operator knows their keyboard is back.
  let midiStatus = midiResubscribeIfDropped()
  if midiStatus.len > 0:
    stderr.writeLine "[aither] MIDI " & midiStatus
  ""

proc retriggerVoice(name: string): string =
  let idx = findSlot(name)
  if idx < 0: return "not found: " & name
  acquire(mtx)
  slots[idx].voice.startT = timeSec + timeFrac
  release(mtx)
  ""

proc markStoppedForTest*(name: string): bool =
  ## Test-only: forcibly mark a voice inactive, simulating the audio
  ## callback's "fade-out completed" branch. Lets tests exercise the
  ## sweep logic without spinning up the audio thread.
  let idx = findSlot(name)
  if idx < 0: return false
  acquire(mtx)
  slots[idx].active = false
  release(mtx)
  true

proc activeVoiceCount*(): int =
  ## Test helper: number of currently-active slots (post-sweep semantics).
  for i in 0 ..< slotCount:
    if slots[i].active: inc result

proc stopVoice*(name: string; fade: float64): string =
  let idx = findSlot(name)
  if idx < 0: return "not found: " & name
  acquire(mtx)
  if fade > 0.0:
    slots[idx].fadeDelta = -fadeDeltaFor(fade)
  else:
    slots[idx].fadeDelta = -fadeDeltaFor(DefaultFadeMs / 1000.0)
  release(mtx)
  ""

proc clearAll(fade: float64): string =
  acquire(mtx)
  for i in 0 ..< slotCount:
    if fade > 0.0:
      slots[i].fadeDelta = -fadeDeltaFor(fade)
    else:
      slots[i].fadeDelta = -fadeDeltaFor(DefaultFadeMs / 1000.0)
  release(mtx)
  ""

proc setMute*(name: string; muted: bool): string =
  let idx = findSlot(name)
  if idx < 0: return "not found: " & name
  acquire(mtx)
  slots[idx].muted = muted
  release(mtx)
  ""

proc soloVoice*(name: string; fade: float64): string =
  let idx = findSlot(name)
  if idx < 0: return "not found: " & name
  acquire(mtx)
  for i in 0 ..< slotCount:
    if i == idx:
      # Targeted voice: ensure it's playing and unmuted. Fade in if it
      # had been silent.
      slots[i].muted = false
      slots[i].fadeDelta = if slots[i].fadeGain < 1.0: fadeDeltaFor(fade) else: 0.0
    else:
      # Other voices: mute (recoverable via `unmute`), not stop. The
      # old behaviour faded gain to 0 then marked the slot inactive,
      # which `unmute` couldn't revive — forcing a re-`send` and losing
      # running state. Mute leaves the voice ticking silently so an
      # operator can toggle it back instantly.
      slots[i].muted = true
  release(mtx)
  ""

proc isNumeric(s: string): bool {.inline.} =
  try: discard parseFloat(s); true
  except ValueError: false

# [Removed: SparkBlocks / dBOf / envBar / fmtDb moved to cli_output.nim
#  in Phase 0 of the analysis-CLI work. Engine now returns raw stats;
#  cli_output handles every conversion from linear to dB and every
#  sparkline render.]

# ----- Data extraction (snapshot under mtx) -----------------------------
# Engine returns these structs; cli_output.nim formats them. Side
# effects that look like part of the text (clips clear-on-read) happen
# here under mtx so the formatter can stay pure.

proc voiceStateOf(s: Slot): VoiceState =
  if not s.active:           vsStopped
  elif s.muted:              vsMuted
  elif s.fadeDelta < 0.0:    vsFadingOut
  elif s.fadeDelta > 0.0:    vsFadingIn
  else:                      vsPlaying

proc partStateOf(g, d: float64): PartState =
  if d > 0.0:    psFadingIn
  elif d < 0.0:  psFadingOut
  elif g <= 0.0: psSilent
  else:          psPlaying

proc snapshotStats(s: Stats): StatsSnapshot =
  StatsSnapshot(
    rmsL: sqrt(s.rmsSqL), rmsR: sqrt(s.rmsSqR),
    peakL: s.peakL, peakR: s.peakR,
    clips: s.clips,
    envRing: s.envRing, envBinIdx: s.envBinIdx)

proc snapshotPartsOf(v: NativeVoice): seq[PartInfo] =
  for i, name in v.partNames:
    result.add PartInfo(
      name: name,
      state: partStateOf(v.partGains[i], v.partFadeDeltas[i]),
      gain: v.partGains[i])

proc voiceInfoList*(): seq[VoiceInfo] =
  ## Snapshot of all voices + their parts. Used by `list`.
  acquire(mtx)
  for i in 0 ..< slotCount:
    result.add VoiceInfo(
      name: slots[i].name,
      state: voiceStateOf(slots[i]),
      gain: slots[i].fadeGain,
      parts: snapshotPartsOf(slots[i].voice))
  release(mtx)

proc midiStatus*(): MidiStatus =
  MidiStatus(portInfo: lastConnectInfo, active: midiSubscriptionActive())

proc voiceStatsSnapshot*(name: string): ScopeQuery =
  ## "" or "*" → master + all voices. "master" → master only. Otherwise
  ## just the named voice (or empty if no such voice). Clears clips.
  acquire(mtx)
  if name == "master":
    result.found = true
    result.snapshots.add VoiceStats(
      name: "master", isMaster: true, stats: snapshotStats(master))
    master.clips = 0
  elif name.len == 0 or name == "*":
    result.found = true
    result.snapshots.add VoiceStats(
      name: "master", isMaster: true, stats: snapshotStats(master))
    master.clips = 0
    for i in 0 ..< slotCount:
      result.snapshots.add VoiceStats(
        name: slots[i].name, isMaster: false,
        state: voiceStateOf(slots[i]), gain: slots[i].fadeGain,
        stats: snapshotStats(slots[i].stats))
      slots[i].stats.clips = 0
  else:
    let idx = findSlot(name)
    if idx >= 0:
      result.found = true
      result.snapshots.add VoiceStats(
        name: slots[idx].name, isMaster: false,
        state: voiceStateOf(slots[idx]), gain: slots[idx].fadeGain,
        stats: snapshotStats(slots[idx].stats))
      slots[idx].stats.clips = 0
  release(mtx)

proc partsSnapshot*(voiceName: string): PartsQuery =
  acquire(mtx)
  result.voiceName = voiceName
  let idx = findSlot(voiceName)
  if idx >= 0:
    result.found = true
    result.parts = snapshotPartsOf(slots[idx].voice)
  release(mtx)

proc snapshotRecent(r: RecentRing): seq[float64] =
  ## Mono mix of the rolling buffer, oldest-first. Caller must hold mtx.
  result.setLen(RecentSamples)
  for k in 0 ..< RecentSamples:
    let i = (r.idx + k) mod RecentSamples
    result[k] = (r.l[i].float64 + r.r[i].float64) * 0.5

proc voiceBufferSnapshot*(name: string): seq[float64] =
  ## Mono mix of the latest ~0.5s. "" or "master" → master; named voice
  ## → that voice. Empty seq if the named voice isn't found.
  acquire(mtx)
  if name.len == 0 or name == "master":
    result = snapshotRecent(masterRecent)
  else:
    let idx = findSlot(name)
    if idx >= 0:
      result = snapshotRecent(slots[idx].recent)
  release(mtx)

proc voiceMeterAfterSend*(name: string):
    tuple[found: bool, stats: StatsSnapshot] =
  ## One-line meter snapshot tacked onto `send` responses. Does NOT
  ## clear clips (the OK is informational, not the official scope read).
  acquire(mtx)
  let idx = findSlot(name)
  if idx >= 0:
    result.found = true
    result.stats = snapshotStats(slots[idx].stats)
  release(mtx)

proc parseFloatArg(s: string; fallback: float64 = 0.0): float64 =
  if s.len == 0: return fallback
  try: return parseFloat(s)
  except ValueError: return fallback

proc findPart(voice: NativeVoice; name: string): int =
  for i, n in voice.partNames:
    if n == name: return i
  -1

proc setPartGainTarget(voice: NativeVoice; part: int;
                       target, fade: float64) =
  let clamped = clamp(target, 0.0, 1.0)
  voice.partFadeTargets[part] = clamped
  if fade <= 0.0:
    voice.partGains[part] = clamped
    voice.partFadeDeltas[part] = 0.0
  else:
    let cur = voice.partGains[part]
    let steps = max(1.0, fade * float64(SampleRate))
    voice.partFadeDeltas[part] = (clamped - cur) / steps

proc setPartMute(voiceName, partName: string;
                 muted: bool; fade: float64): string =
  let vIdx = findSlot(voiceName)
  if vIdx < 0: return "voice not found: " & voiceName
  let voice = slots[vIdx].voice
  let pIdx = findPart(voice, partName)
  if pIdx < 0:
    return "no play named " & partName & " in voice " & voiceName
  let target = if muted: 0.0 else: 1.0
  let f = if fade > 0.0: fade else: DefaultFadeMs / 1000.0
  acquire(mtx); setPartGainTarget(voice, pIdx, target, f); release(mtx)
  ""

proc soloPart(voiceName, partName: string; fade: float64): string =
  let vIdx = findSlot(voiceName)
  if vIdx < 0: return "voice not found: " & voiceName
  let voice = slots[vIdx].voice
  let pIdx = findPart(voice, partName)
  if pIdx < 0:
    return "no play named " & partName & " in voice " & voiceName
  let f = if fade > 0.0: fade else: DefaultFadeMs / 1000.0
  acquire(mtx)
  for i in 0 ..< voice.partNames.len:
    let target = if i == pIdx: 1.0 else: 0.0
    setPartGainTarget(voice, i, target, f)
  release(mtx)
  ""

# [Removed: listVoices text builder moved to cli_output.formatVoiceList.
#  Engine returns voiceInfoList() + midiStatus(); cli_output composes.]

# ------------------------------------------------------------- command parsing

proc handleCmd(line: string): string =
  let parts = line.strip().splitWhitespace()
  if parts.len == 0: return "ERR empty"
  case parts[0].toLowerAscii()
  of "send":
    if parts.len < 2: return "ERR usage: send <file> [fade-seconds]"
    let fade = if parts.len >= 3: parseFloatArg(parts[2]) else: 0.0
    let err = loadPatch(parts[1], fade)
    if err.len > 0: "ERR " & err
    else:
      let baseName = splitFile(parts[1]).name
      let snap = voiceMeterAfterSend(baseName)
      if snap.found: "OK " & formatMeterLine(baseName, snap.stats)
      else: "OK"
  of "stop":
    if parts.len < 2: return "ERR usage: stop <name> [fade-seconds]"
    let fade = if parts.len >= 3: parseFloatArg(parts[2]) else: 0.0
    let err = stopVoice(parts[1], fade)
    if err.len > 0: "ERR " & err else: "OK"
  of "mute":
    if parts.len < 2: return "ERR usage: mute <voice> [play] [fade-seconds]"
    if parts.len >= 3 and not isNumeric(parts[2]):
      let fade = if parts.len >= 4: parseFloatArg(parts[3]) else: 0.0
      let err = setPartMute(parts[1], parts[2], true, fade)
      if err.len > 0: "ERR " & err else: "OK"
    else:
      let err = setMute(parts[1], true)
      if err.len > 0: "ERR " & err else: "OK"
  of "unmute":
    if parts.len < 2: return "ERR usage: unmute <voice> [play] [fade-seconds]"
    if parts.len >= 3 and not isNumeric(parts[2]):
      let fade = if parts.len >= 4: parseFloatArg(parts[3]) else: 0.0
      let err = setPartMute(parts[1], parts[2], false, fade)
      if err.len > 0: "ERR " & err else: "OK"
    else:
      let err = setMute(parts[1], false)
      if err.len > 0: "ERR " & err else: "OK"
  of "solo":
    if parts.len < 2: return "ERR usage: solo <voice> [play] [fade-seconds]"
    if parts.len >= 3 and not isNumeric(parts[2]):
      let fade = if parts.len >= 4: parseFloatArg(parts[3]) else: 0.0
      let err = soloPart(parts[1], parts[2], fade)
      if err.len > 0: "ERR " & err else: "OK"
    else:
      let fade = if parts.len >= 3: parseFloatArg(parts[2]) else: 0.0
      let err = soloVoice(parts[1], fade)
      if err.len > 0: "ERR " & err else: "OK"
  of "clear":
    let fade = if parts.len >= 2: parseFloatArg(parts[1]) else: 0.0
    let err = clearAll(fade)
    if err.len > 0: "ERR " & err else: "OK"
  of "list":
    "OK\n" & formatVoiceList(midiStatus(), voiceInfoList())
  of "scope":
    let target = if parts.len >= 2: parts[1] else: ""
    let q = voiceStatsSnapshot(target)
    "OK\n" & formatScope(target, q)
  of "spectrum":
    let target = if parts.len >= 2: parts[1] else: ""
    let buf = voiceBufferSnapshot(target)
    if buf.len == 0:
      "ERR voice not found: " & target
    else:
      let summary = analyze(buf, float64(SampleRate))
      "OK\n" & formatSpectrum(summary)
  of "retrigger":
    if parts.len < 2: return "ERR usage: retrigger <name>"
    let err = retriggerVoice(parts[1])
    if err.len > 0: "ERR " & err else: "OK"
  of "parts":
    if parts.len < 2: return "ERR usage: parts <voice>"
    "OK\n" & formatParts(partsSnapshot(parts[1]))
  of "midi":
    if parts.len < 2:
      return "ERR usage: midi list|connect <spec>|disconnect"
    case parts[1].toLowerAscii()
    of "list":
      let body = midiListPorts()
      if body.len == 0: "OK (no MIDI ports)"
      else: "OK\n" & body.strip(leading = false)
    of "connect":
      if parts.len < 3: return "ERR usage: midi connect <client:port>"
      if midiConnect(parts[2]): "OK connected " & parts[2]
      else: "ERR midi connect failed: " & parts[2]
    of "disconnect":
      # v1: we don't track individual subscriptions, so the only thing we
      # can offer is closing + reopening the sequencer. That also drops
      # the thread — call it a deliberate limitation for now.
      midiShutdown()
      if midiOpen():
        midiStartThread()
        "OK midi reset"
      else:
        "ERR midi reopen failed"
    else:
      "ERR usage: midi list|connect <spec>|disconnect"
  of "kill":
    running = false
    "OK bye"
  else:
    "ERR unknown: " & parts[0]

# ----------------------------------------------------------------------- engine

proc startEngine*() =
  if aither_audio_init(SampleRate, Channels, BufferFrames,
                       audioCallback, nil) != 0:
    quit "audio init failed", 1
  if aither_audio_start() != 0:
    quit "audio start failed", 1

  echo "aither \xC2\xB7 ", SampleRate, " Hz \xC2\xB7 ", SocketPath

  # Bring up MIDI input. If no sequencer is available (unusual on Linux
  # but possible in containers / chroots) we just log and carry on —
  # midi_* primitives read zero, patches still run.
  if midiOpen():
    let who = midiAutoConnect()
    if who.len > 0:
      stderr.writeLine "[aither] connected to MIDI: ", who
    else:
      stderr.writeLine "[aither] MIDI ready, no input port auto-connected"
    midiStartThread()
  else:
    stderr.writeLine "[aither] MIDI unavailable (ALSA seq open failed)"

  if fileExists(SocketPath): removeFile(SocketPath)
  var server = newSocket(AF_UNIX, SOCK_STREAM, IPPROTO_IP)
  server.bindUnix(SocketPath)
  server.listen()

  running = true
  while running:
    try:
      var client: Socket
      server.accept(client)
      let cmd = client.recvLine()
      let resp = handleCmd(cmd)
      client.send(resp & "\n")
      client.close()
    except CatchableError:
      if running: discard

  midiShutdown()
  discard aither_audio_stop()
  aither_audio_uninit()
  server.close()
  try: removeFile(SocketPath) except CatchableError: discard
  echo "bye"

# ----------------------------------------------------------------------- client

proc sendCmd*(cmd: string) =
  var sock = newSocket(AF_UNIX, SOCK_STREAM, IPPROTO_IP)
  try:
    sock.connectUnix(SocketPath)
    sock.send(cmd & "\n")
    let resp = sock.recv(65536)
    echo resp.strip()
  except OSError:
    echo "error: engine not running? (" & SocketPath & ")"
  finally:
    sock.close()

# [CLI dispatch moved to aither.nim — engine.nim is now a library
#  module. Build entry point is `nim c aither.nim`.]
