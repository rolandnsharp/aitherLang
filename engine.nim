## aither engine — audio callback + socket CLI for live coding

import std/[os, net, strutils, math, locks, tables]
import parser, voice, miniaudio

const
  SampleRate    = 48000'u32
  Channels      = 2'u32
  BufferFrames  = 512'u32
  MaxVoices     = 16
  SocketPath    = "/tmp/aither.sock"
  DefaultFadeMs = 20.0

const Stdlib = staticRead("stdlib.aither")

const EnvBins = 20          # sparkline slots
const EnvBinSamples = 2400  # 50ms per bin at 48 kHz
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
      lMix += gl
      rMix += gr

    update(master, lMix, rMix)
    output[i * 2]     = cfloat(tanh(lMix))
    output[i * 2 + 1] = cfloat(tanh(rMix))

  release(mtx)

# ------------------------------------------------------------- voice management

proc findSlot(name: string): int =
  for i in 0 ..< slotCount:
    if slots[i].name == name: return i
  -1

proc fadeDeltaFor(seconds: float64): float64 =
  let s = if seconds <= 0.0: DefaultFadeMs / 1000.0 else: seconds
  1.0 / (s * float64(SampleRate))

proc loadPatch(filename: string; fadeIn: float64): string =
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

  # Merge: stdlib's top-level statements come first, then the user's.
  let program = parser.Node(
    kind: parser.nkBlock,
    kids: stdlibAst.kids & userAst.kids,
    line: 1)

  let idx = findSlot(baseName)
  acquire(mtx)
  let now = timeSec + timeFrac
  if idx >= 0:
    # Hot-reload: re-compile in place; voice keeps top-level vars (by name)
    # and call-site state (by slot) across the swap.
    try:
      slots[idx].voice.load(program, float64(SampleRate))
    except CatchableError as e:
      release(mtx)
      stderr.writeLine "compile FAIL"
      return "compile error: " & e.msg
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
    stderr.writeLine "ok (" & (if retrigger: "retrigger " else: "hot-swap ") & baseName & ")"
  else:
    if slotCount >= MaxVoices:
      release(mtx)
      return "voice limit reached (" & $MaxVoices & ")"
    let voice = newVoice(float64(SampleRate))
    try:
      voice.load(program, float64(SampleRate))
    except CatchableError as e:
      release(mtx)
      stderr.writeLine "compile FAIL"
      return "compile error: " & e.msg
    voice.startT = now
    slots[slotCount] = Slot(
      name: baseName, voice: voice, active: true,
      fadeGain: (if fadeIn > 0.0: 0.0 else: 1.0),
      fadeDelta: (if fadeIn > 0.0: fadeDeltaFor(fadeIn) else: 0.0))
    inc slotCount
    stderr.writeLine "ok (new " & baseName & ")"
  release(mtx)
  ""

proc retriggerVoice(name: string): string =
  let idx = findSlot(name)
  if idx < 0: return "not found: " & name
  acquire(mtx)
  slots[idx].voice.startT = timeSec + timeFrac
  release(mtx)
  ""

proc stopVoice(name: string; fade: float64): string =
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

proc setMute(name: string; muted: bool): string =
  let idx = findSlot(name)
  if idx < 0: return "not found: " & name
  acquire(mtx)
  slots[idx].muted = muted
  release(mtx)
  ""

proc soloVoice(name: string; fade: float64): string =
  let idx = findSlot(name)
  if idx < 0: return "not found: " & name
  acquire(mtx)
  for i in 0 ..< slotCount:
    if i == idx:
      slots[i].fadeDelta = if slots[i].fadeGain < 1.0: fadeDeltaFor(fade) else: 0.0
    else:
      slots[i].fadeDelta = -fadeDeltaFor(if fade > 0.0: fade else: DefaultFadeMs / 1000.0)
  release(mtx)
  ""

const SparkBlocks = [" ", "\u2581", "\u2582", "\u2583", "\u2584", "\u2585", "\u2586", "\u2587", "\u2588"]

proc dBOf(v: float64): float64 {.inline.} =
  if v < 1e-9: -180.0 else: 20.0 * log10(v)

proc envBar(v: float32): string =
  let f = v.float64
  if f < 1e-5: return SparkBlocks[0]
  let db = dBOf(f)
  let idx = clamp(int((db + 60.0) / 60.0 * 8.0 + 0.5), 0, 8)
  SparkBlocks[idx]

proc fmtDb(v: float64): string =
  if v < 1e-9: return "  -inf"
  let db = dBOf(v)
  formatFloat(db, ffDecimal, 1)

proc statsReport(header: string; s: Stats): string =
  var spark = ""
  for k in 0 ..< EnvBins:
    spark &= envBar(s.envRing[(s.envBinIdx + k) mod EnvBins])
  let rmsL = sqrt(s.rmsSqL)
  let rmsR = sqrt(s.rmsSqR)
  header & "\n" &
  "  RMS   L " & fmtDb(rmsL) & " dB   R " & fmtDb(rmsR) & " dB\n" &
  "  peak  L " & fmtDb(s.peakL) & " dB   R " & fmtDb(s.peakR) &
       " dB   clips=" & $s.clips & "\n" &
  "  env   " & spark

proc voiceReport(i: int): string =
  let state =
    if not slots[i].active:        "stopped"
    elif slots[i].muted:           "muted"
    elif slots[i].fadeDelta < 0.0: "fading-out"
    elif slots[i].fadeDelta > 0.0: "fading-in"
    else:                          "playing"
  let header = slots[i].name & "  " & state & "  gain=" &
               formatFloat(slots[i].fadeGain, ffDecimal, 2)
  let rep = statsReport(header, slots[i].stats)
  slots[i].stats.clips = 0          # clear-on-read
  rep

proc masterReport(): string =
  let rep = statsReport("master", master)
  master.clips = 0                  # clear-on-read
  rep

proc scopeReport(name: string): string =
  if name == "master":
    return masterReport()
  if name.len == 0 or name == "*":
    var parts: seq[string] = @[masterReport()]
    for i in 0 ..< slotCount:
      parts.add voiceReport(i)
    return parts.join("\n\n")
  let idx = findSlot(name)
  if idx < 0: return "not found: " & name
  voiceReport(idx)

proc parseFloatArg(s: string; fallback: float64 = 0.0): float64 =
  if s.len == 0: return fallback
  try: return parseFloat(s)
  except ValueError: return fallback

proc findPart(voice: NativeVoice; name: string): int =
  for i, n in voice.partNames:
    if n == name: return i
  -1

proc partStateWord(g, d: float64): string =
  if d > 0.0: "fading-in"
  elif d < 0.0: "fading-out"
  elif g <= 0.0: "silent"
  else: "playing"

proc partsReport(voiceName: string): string =
  let idx = findSlot(voiceName)
  if idx < 0: return "not found: " & voiceName
  let v = slots[idx].voice
  if v.partNames.len == 0: return "(no parts)"
  var lines: seq[string] = @[]
  for i, name in v.partNames:
    let state = partStateWord(v.partGains[i], v.partFadeDeltas[i])
    lines.add "  " & name & "  [" & state & "]  gain=" &
              formatFloat(v.partGains[i], ffDecimal, 2)
  voiceName & "\n" & lines.join("\n")

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

proc partCmd(voiceName, partName, action: string;
             args: seq[string]): string =
  let vIdx = findSlot(voiceName)
  if vIdx < 0: return "voice not found: " & voiceName
  let voice = slots[vIdx].voice
  let pIdx = findPart(voice, partName)
  if pIdx < 0: return "part not found: " & partName
  case action.toLowerAscii()
  of "play":
    let fade = if args.len >= 1: parseFloatArg(args[0]) else: 0.02
    acquire(mtx); setPartGainTarget(voice, pIdx, 1.0, fade); release(mtx)
    ""
  of "stop":
    let fade = if args.len >= 1: parseFloatArg(args[0]) else: 0.02
    acquire(mtx); setPartGainTarget(voice, pIdx, 0.0, fade); release(mtx)
    ""
  of "mute":
    acquire(mtx); setPartGainTarget(voice, pIdx, 0.0, 0.01); release(mtx)
    ""
  of "unmute":
    acquire(mtx); setPartGainTarget(voice, pIdx, 1.0, 0.01); release(mtx)
    ""
  of "gain":
    if args.len < 1: return "usage: part <voice> <name> gain <value> [fade]"
    let target = parseFloatArg(args[0])
    let fade = if args.len >= 2: parseFloatArg(args[1]) else: 0.02
    acquire(mtx); setPartGainTarget(voice, pIdx, target, fade); release(mtx)
    ""
  else:
    "unknown part action: " & action

proc listVoices(): string =
  var lines: seq[string]
  for i in 0 ..< slotCount:
    let state =
      if not slots[i].active:           "stopped"
      elif slots[i].muted:              "muted"
      elif slots[i].fadeDelta < 0.0:    "fading-out"
      elif slots[i].fadeDelta > 0.0:    "fading-in"
      else:                             "playing"
    lines.add slots[i].name & " [" & state & "] gain=" &
              formatFloat(slots[i].fadeGain, ffDecimal, 2)
  if lines.len == 0: "(no voices)" else: lines.join("\n")

# ------------------------------------------------------------- command parsing

proc handleCmd(line: string): string =
  let parts = line.strip().splitWhitespace()
  if parts.len == 0: return "ERR empty"
  case parts[0].toLowerAscii()
  of "send":
    if parts.len < 2: return "ERR usage: send <file> [fade-seconds]"
    let fade = if parts.len >= 3: parseFloatArg(parts[2]) else: 0.0
    let err = loadPatch(parts[1], fade)
    if err.len > 0: "ERR " & err else: "OK"
  of "stop":
    if parts.len < 2: return "ERR usage: stop <name> [fade-seconds]"
    let fade = if parts.len >= 3: parseFloatArg(parts[2]) else: 0.0
    let err = stopVoice(parts[1], fade)
    if err.len > 0: "ERR " & err else: "OK"
  of "mute":
    if parts.len < 2: return "ERR usage: mute <name>"
    let err = setMute(parts[1], true)
    if err.len > 0: "ERR " & err else: "OK"
  of "unmute":
    if parts.len < 2: return "ERR usage: unmute <name>"
    let err = setMute(parts[1], false)
    if err.len > 0: "ERR " & err else: "OK"
  of "solo":
    if parts.len < 2: return "ERR usage: solo <name> [fade-seconds]"
    let fade = if parts.len >= 3: parseFloatArg(parts[2]) else: 0.0
    let err = soloVoice(parts[1], fade)
    if err.len > 0: "ERR " & err else: "OK"
  of "clear":
    let fade = if parts.len >= 2: parseFloatArg(parts[1]) else: 0.0
    let err = clearAll(fade)
    if err.len > 0: "ERR " & err else: "OK"
  of "list":
    "OK\n" & listVoices()
  of "scope":
    let target = if parts.len >= 2: parts[1] else: ""
    "OK\n" & scopeReport(target)
  of "retrigger":
    if parts.len < 2: return "ERR usage: retrigger <name>"
    let err = retriggerVoice(parts[1])
    if err.len > 0: "ERR " & err else: "OK"
  of "parts":
    if parts.len < 2: return "ERR usage: parts <voice>"
    "OK\n" & partsReport(parts[1])
  of "part":
    if parts.len < 4:
      return "ERR usage: part <voice> <name> <play|stop|mute|unmute|gain> [args]"
    let extra = if parts.len >= 5: parts[4 .. ^1] else: @[]
    let err = partCmd(parts[1], parts[2], parts[3], extra)
    if err.len > 0: "ERR " & err else: "OK"
  of "kill":
    running = false
    "OK bye"
  else:
    "ERR unknown: " & parts[0]

# ----------------------------------------------------------------------- engine

proc startEngine() =
  if aither_audio_init(SampleRate, Channels, BufferFrames,
                       audioCallback, nil) != 0:
    quit "audio init failed", 1
  if aither_audio_start() != 0:
    quit "audio start failed", 1

  echo "aither \xC2\xB7 ", SampleRate, " Hz \xC2\xB7 ", SocketPath

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

  discard aither_audio_stop()
  aither_audio_uninit()
  server.close()
  try: removeFile(SocketPath) except CatchableError: discard
  echo "bye"

# ----------------------------------------------------------------------- client

proc sendCmd(cmd: string) =
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

# -------------------------------------------------------------------------- CLI

when isMainModule:
  let args = commandLineParams()
  if args.len == 0:
    echo "usage: aither <start|send|stop|mute|unmute|solo|clear|list|kill> [args]"
    echo ""
    echo "  start                       launch engine"
    echo "  send <file> [fade]          load patch (instant or fade-in seconds)"
    echo "  stop <name> [fade]          fade out & remove voice"
    echo "  mute <name>                 silence (state keeps running)"
    echo "  unmute <name>               resume"
    echo "  solo <name> [fade]          fade out everything else"
    echo "  clear [fade]                stop all voices"
    echo "  list                        show active voices"
    echo "  scope [name]                per-voice RMS/peak/clips/envelope (all if no name; 'master' for mix bus)"
    echo "  retrigger <name>            reset start_t so the composition plays from the top"
    echo "  parts <voice>               list named parts (play blocks) with gain + state"
    echo "  part <voice> <name> <action> [args]   play/stop/mute/unmute/gain a part"
    echo "  kill                        shut down engine"
    quit 0

  case args[0]
  of "start":
    startEngine()
  of "send":
    if args.len < 2: quit "usage: aither send <file> [fade]"
    let extra = if args.len >= 3: " " & args[2] else: ""
    sendCmd("send " & absolutePath(args[1]) & extra)
  of "stop":
    if args.len < 2: quit "usage: aither stop <name> [fade]"
    let extra = if args.len >= 3: " " & args[2] else: ""
    sendCmd("stop " & args[1] & extra)
  of "mute":
    if args.len < 2: quit "usage: aither mute <name>"
    sendCmd("mute " & args[1])
  of "unmute":
    if args.len < 2: quit "usage: aither unmute <name>"
    sendCmd("unmute " & args[1])
  of "solo":
    if args.len < 2: quit "usage: aither solo <name> [fade]"
    let extra = if args.len >= 3: " " & args[2] else: ""
    sendCmd("solo " & args[1] & extra)
  of "clear":
    let extra = if args.len >= 2: " " & args[1] else: ""
    sendCmd("clear" & extra)
  of "list":
    sendCmd("list")
  of "scope":
    let extra = if args.len >= 2: " " & args[1] else: ""
    sendCmd("scope" & extra)
  of "retrigger":
    if args.len < 2: quit "usage: aither retrigger <name>"
    sendCmd("retrigger " & args[1])
  of "parts":
    if args.len < 2: quit "usage: aither parts <voice>"
    sendCmd("parts " & args[1])
  of "part":
    if args.len < 4:
      quit "usage: aither part <voice> <name> <play|stop|mute|unmute|gain> [args]"
    sendCmd("part " & args[1 .. ^1].join(" "))
  of "kill":
    sendCmd("kill")
  else:
    echo "unknown command: " & args[0]
    quit 1
