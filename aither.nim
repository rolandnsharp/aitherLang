## CLI dispatch for `aither`. Pure routing — the bare command (or one
## given patch files and an optional duration) runs the in-process
## engine; every named subcommand either shells a command across the
## socket (`send`, `stop`, `list`, ...) or runs offline analysis
## (`audit`). No business logic lives here.

import std/[os, strutils, times, terminal]
import engine, render, analysis, cli_output

proc readRawStdin(): Stereo =
  ## `input()` for an offline run: raw float32 pairs on stdin, the same
  ## format the engine writes, so `aither x.aither 10 > a.raw` and
  ## `aither audit fx.aither < a.raw` chain. Nothing when stdin is a
  ## terminal.
  if isatty(stdin): return (@[], @[])
  var buf = newSeq[float32](8192)
  var all: seq[float32]
  while true:
    let n = stdin.readBuffer(addr buf[0], buf.len * sizeof(float32))
    if n <= 0: break
    all.add buf[0 ..< n div sizeof(float32)]
  let frames = all.len div 2
  result.left.setLen(frames); result.right.setLen(frames)
  for i in 0 ..< frames:
    result.left[i] = float64(all[2 * i])
    result.right[i] = float64(all[2 * i + 1])

proc audit(path: string; secondsArg: float64; skip: float64) =
  ## Offline render → spectral analysis + per-part levels → text.
  ## Doesn't touch the engine; safe to run with no engine. With raw
  ## pairs on stdin, `input()` in the patch reads them, and the render
  ## runs their full length unless a duration was given. `skip` seconds
  ## run first and aren't measured, to check a later section.
  # The rate is AITHER_RATE's, as for the engine (44.1 kHz on the
  # uConsole), so a check hears what the speaker plays; 48 kHz unset.
  let rate = int(envFloat("AITHER_RATE", 48000.0))
  if not fileExists(path):
    stderr.writeLine "audit: file not found: " & path
    quit 1
  var r: Rendered
  var seconds = secondsArg
  try:
    let input = if patchCallsInput(path): readRawStdin() else: (@[], @[])
    if input.left.len > 0 and seconds <= 0.0:
      seconds = float64(input.left.len) / float64(rate) - skip
    if seconds <= 0.0: seconds = 1.0
    r = renderPatchFull(path, seconds, rate, input, skip)
  except CatchableError as e:
    stderr.writeLine "audit: " & e.msg.splitLines()[0]
    quit 1
  let mono = monoMix(r.audio.left, r.audio.right)
  let summary = analyze(mono, float64(rate))
  let at = if skip > 0.0: " from " & formatFloat(skip, ffDecimal, 1) & "s" else: ""
  echo formatAudit(path & at, seconds, float64(rate), summary)
  let parts = formatPartLevels(r.parts)
  if parts.len > 0: echo parts

proc sendPatch(path, extra: string): bool =
  ## `send`, then listen for a moment (AITHER_SEND_WAIT seconds, default
  ## 1, 0 to skip) and report what actually happened: level, quiet parts,
  ## rollback or guard trips, the master. The meter in the OK line alone
  ## is read the instant the swap lands, before any new sound has played.
  let resp = queryCmd("send " & absolutePath(path) & extra)
  if not resp.startsWith("OK"):
    echo resp
    return false
  echo "OK"
  let wait = try: parseFloat(getEnv("AITHER_SEND_WAIT", "1")) except ValueError: 1.0
  if wait > 0.0:
    sleep(int(wait * 1000))
    let check = queryCmd("check " & splitFile(path).name)
    echo (if check.startsWith("OK\n"): check[3 .. ^1] else: check)
  true

proc usage() =
  echo "usage: aither [patch.aither ...] [seconds]      the engine: float pairs on stdout"
  echo "       aither <send|stop|mute|unmute|solo|park|unpark|clear|list|status|levels|" &
       "parts|check|sweep|midi|audit|try|spectrum|kill> [args]"
  echo ""
  echo "  aither | aplay -f FLOAT_LE -c 2 -r 48000      play; send patches to it"
  echo "  aither set.aither | tools/play                 same, with a first voice loaded"
  echo "  aither set.aither 30 > take.raw                offline: 30 s from t=0, then exit"
  echo "  AITHER_RATE=192000 aither fm.aither | sdr-tx   the rate is yours; the reader agrees"
  echo "  arecord -f FLOAT_LE -c 2 -r 48000 | aither fx.aither | tools/play"
  echo "                                                 input() reads stdin when it is a pipe"
  echo ""
  echo "  send <file> [fade]          load patch (instant or fade-in seconds)"
  echo "  stop <name> [fade]          fade out & remove voice"
  echo "  mute <target> [fade]        fade to silence; it runs on, in step"
  echo "  unmute <target> [fade]      fade back in"
  echo "  solo <target> [fade]        mute the others: other voices, or the voice's other plays"
  echo "  park <target> [fade]        fade out, then stop running it: no CPU, state frozen"
  echo "  unpark <target> [fade]      run it again from where it stopped"
  echo "                              a target is a voice (bass) or one of its plays (bass.drone)"
  echo "  clear [fade]                stop all voices"
  echo "  list                        show active voices"
  echo "  status                      voices, parts and the master level"
  echo "  levels [name]               per-voice RMS/peak/clips/envelope"
  echo "  sweep <name> [seconds]      where the voice's sweep is; with seconds, put it there (0: from the top)"
  echo "  parts <voice>               list parts (play blocks): state, gain, rms, peak"
  echo "  check <voice>               level, quiet parts, rollbacks and guard trips"
  echo "  profile <voice> [seconds]   CPU per part, top-level code and final expression (2 s)"
  echo "  midi list                   show ALSA seq ports"
  echo "  midi connect <spec>         subscribe to a specific port (e.g. '28:0')"
  echo "  midi disconnect             drop MIDI and re-open the sequencer"
  echo "  audit <patch> [seconds] [--from S]"
  echo "                              offline render from t=0: spectrum + per-part levels"
  echo "                              (default 1s; --from runs S seconds first, unmeasured;"
  echo "                              raw pairs on stdin feed input() and set the length)"
  echo "  try <patch> [seconds] [--from S]"
  echo "                              render the patch as a send would land it NOW: the"
  echo "                              live voice's state and time, without touching the"
  echo "                              sound (default 2s). Catches NaN/runaways first"
  echo "  spectrum [voice]            spectral summary of the engine's recent buffer"
  echo "  kill                        shut down engine"

proc isNumber(s: string): bool =
  try: discard parseFloat(s); true
  except ValueError: false

let args = commandLineParams()

# The engine: no arguments, or patch files and an optional duration.
if args.len == 0 or args[0].endsWith(".aither") or isNumber(args[0]):
  var files: seq[string]
  var seconds = 0.0
  for a in args:
    if a.endsWith(".aither"): files.add a
    elif isNumber(a): seconds = parseFloat(a)
    else:
      usage()
      quit 1
  runEngine(files, seconds)
  quit 0

case args[0]
of "help", "-h", "--help":
  usage()
of "send":
  if args.len < 2: quit "usage: aither send <file> [fade]"
  let extra = if args.len >= 3: " " & args[2] else: ""
  if not sendPatch(args[1], extra): quit 1
of "stop":
  if args.len < 2: quit "usage: aither stop <name> [fade]"
  let extra = if args.len >= 3: " " & args[2] else: ""
  sendCmd("stop " & args[1] & extra)
of "mute", "unmute", "solo", "park", "unpark":
  if args.len notin 2..3: quit "usage: aither " & args[0] & " <voice>[.<play>] [fade]"
  sendCmd(args.join(" "))
of "clear":
  let extra = if args.len >= 2: " " & args[1] else: ""
  sendCmd("clear" & extra)
of "list":
  sendCmd("list")
of "status":
  # What people type: the voices and their parts, then the master level.
  sendCmd("list")
  sendCmd("levels master")
of "levels":
  let extra = if args.len >= 2: " " & args[1] else: ""
  sendCmd("levels" & extra)
of "sweep":
  if args.len < 2: quit "usage: aither sweep <name> [seconds]"
  sendCmd("sweep " & args[1] & (if args.len >= 3: " " & args[2] else: ""))
of "parts":
  if args.len < 2: quit "usage: aither parts <voice>"
  sendCmd("parts " & args[1])
of "profile":
  if args.len < 2: quit "usage: aither profile <voice> [seconds]"
  sendCmd("profile " & args[1] & (if args.len >= 3: " " & args[2] else: ""))
of "check":
  if args.len < 2: quit "usage: aither check <voice>"
  sendCmd("check " & splitFile(args[1]).name)
of "midi":
  if args.len < 2: quit "usage: aither midi list|connect <spec>|disconnect"
  let rest = args[1 .. ^1].join(" ")
  sendCmd("midi " & rest)
of "audit", "try":
  let usageLine = "usage: aither " & args[0] & " <patch> [seconds] [--from S]"
  if args.len < 2: quit usageLine
  var seconds = 0.0
  var skip = 0.0
  var i = 2
  while i < args.len:
    let a = args[i]
    if a == "--from" and i + 1 < args.len:
      try: skip = parseFloat(args[i + 1])
      except ValueError: quit usageLine
      i += 2
      continue
    try: seconds = parseFloat(a)
    except ValueError: quit usageLine
    inc i
  if args[0] == "audit":
    audit(args[1], seconds, skip)
  else:
    let resp = queryCmd("try " & absolutePath(args[1]) & " " &
                        $(if seconds > 0.0: seconds else: 2.0) & " " & $skip)
    echo (if resp.startsWith("OK\n"): resp[3 .. ^1] else: resp)
    if not resp.startsWith("OK"): quit 1
of "spectrum":
  let extra = if args.len >= 2: " " & args[1] else: ""
  sendCmd("spectrum" & extra)
of "kill":
  sendCmd("kill")
else:
  echo "unknown command: " & args[0]
  usage()
  quit 1
