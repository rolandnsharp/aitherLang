## aither engine — audio callback + socket CLI for live coding

import std/[os, net, strutils, math, locks, tables]
import parser, eval, miniaudio

const
  SampleRate    = 48000'u32
  Channels      = 2'u32
  BufferFrames  = 512'u32
  MaxVoices     = 16
  SocketPath    = "/tmp/aither.sock"
  DefaultFadeMs = 20.0

const Stdlib = staticRead("stdlib.aither")

type
  Slot = object
    name:       string
    voice:      eval.Voice
    active:     bool
    muted:      bool
    fadeGain:   float64
    fadeDelta:  float64

var
  slots: array[MaxVoices, Slot]
  slotCount: int
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

    var mix = 0.0
    for v in 0 ..< sc:
      if not slots[v].active or slots[v].muted: continue
      var sample = 0.0
      try:
        sample = slots[v].voice.tick(t)
      except CatchableError:
        sample = 0.0
      slots[v].fadeGain = clamp(
        slots[v].fadeGain + slots[v].fadeDelta, 0.0, 1.0)
      if slots[v].fadeGain <= 0.0 and slots[v].fadeDelta < 0.0:
        slots[v].active = false
      mix += sample * slots[v].fadeGain

    let clipped = cfloat(tanh(mix))
    output[i * 2]     = clipped
    output[i * 2 + 1] = clipped

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
  let combined = Stdlib & "\n" & userSrc
  let baseName = splitFile(filename).name

  stderr.write "loading " & extractFilename(filename) & " ... "

  var program: parser.Node
  try:
    program = parseProgram(combined)
  except ParseError as e:
    stderr.writeLine "parse FAIL"
    return "parse error: " & e.msg

  let idx = findSlot(baseName)
  acquire(mtx)
  let now = timeSec + timeFrac
  if idx >= 0:
    # Hot-reload: keep existing voice's vars + callsite state.
    # start_t persists across hot-swaps so timed sweeps keep their timeline,
    # but resets when a stopped voice is being re-triggered.
    slots[idx].voice.program = program
    slots[idx].voice.callSiteCounter = 0
    slots[idx].voice.funcs.clear()
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
    let voice = newVoice(program, float64(SampleRate))
    voice.startT = now
    slots[slotCount] = Slot(
      name: baseName, voice: voice, active: true,
      fadeGain: (if fadeIn > 0.0: 0.0 else: 1.0),
      fadeDelta: (if fadeIn > 0.0: fadeDeltaFor(fadeIn) else: 0.0))
    inc slotCount
    stderr.writeLine "ok (new " & baseName & ")"
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

proc parseFloatArg(s: string; fallback: float64 = 0.0): float64 =
  if s.len == 0: return fallback
  try: return parseFloat(s)
  except ValueError: return fallback

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
  of "kill":
    sendCmd("kill")
  else:
    echo "unknown command: " & args[0]
    quit 1
