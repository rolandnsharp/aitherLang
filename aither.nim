## CLI dispatch for `aither <subcommand> ...`. Pure routing — every
## subcommand reads argv, then either calls the in-process engine
## (`start`) for the daemon side, or shells a command across the
## socket (`send`, `stop`, `list`, ...), or runs offline analysis
## (`audit`). No business logic lives here.

import std/[os, strutils]
import engine, render, analysis, cli_output

proc audit(path: string; secondsArg: float64; inputPath: string;
           skip: float64) =
  ## Offline render → spectral analysis + per-part levels → text.
  ## Doesn't touch the engine; safe to run with no daemon. With a WAV,
  ## `input()` in the patch reads it, and the render runs its full
  ## length unless a duration was given. `skip` seconds run first and
  ## aren't measured, to check a later section.
  if not fileExists(path):
    stderr.writeLine "audit: file not found: " & path
    quit 1
  var r: Rendered
  var seconds = secondsArg
  try:
    var input: Stereo = (@[], @[])
    if inputPath.len > 0:
      input = readWav(inputPath, 48000)
      if seconds <= 0.0: seconds = float64(input.left.len) / 48000.0 - skip
    if seconds <= 0.0: seconds = 1.0
    r = renderPatchFull(path, seconds, 48000, input, skip)
  except CatchableError as e:
    stderr.writeLine "audit: " & e.msg.splitLines()[0]
    quit 1
  let mono = monoMix(r.audio.left, r.audio.right)
  let summary = analyze(mono, 48000.0)
  let at = if skip > 0.0: " from " & formatFloat(skip, ffDecimal, 1) & "s" else: ""
  echo formatAudit(path & at, seconds, 48000.0, summary)
  let parts = formatPartLevels(r.parts)
  if parts.len > 0: echo parts

let args = commandLineParams()
if args.len == 0:
  echo "usage: aither <start|send|stop|mute|unmute|solo|clear|list|" &
       "scope|parts|check|retrigger|midi|audit|try|spectrum|kill> [args]"
  echo ""
  echo "  start                       launch engine"
  echo "  send <file> [fade]          load patch (instant or fade-in seconds)"
  echo "  stop <name> [fade]          fade out & remove voice"
  echo "  mute <voice> [play] [fade]  silence whole voice or one play block"
  echo "  unmute <voice> [play] [fade] resume voice or play block"
  echo "  solo <voice> [play] [fade]  fade out other voices, or other plays in this voice"
  echo "  clear [fade]                stop all voices"
  echo "  list                        show active voices"
  echo "  scope [name]                per-voice RMS/peak/clips/envelope"
  echo "  retrigger <name>            reset start_t so the composition plays from the top"
  echo "  parts <voice>               list parts (play blocks): state, gain, rms, peak"
  echo "  check <voice>               level, quiet parts, rollbacks and guard trips"
  echo "  midi list                   show ALSA seq ports"
  echo "  midi connect <spec>         subscribe to a specific port (e.g. '28:0')"
  echo "  midi disconnect             drop MIDI and re-open the sequencer"
  echo "  audit <patch> [seconds] [--from S] [in.wav]"
  echo "                              offline render from t=0: spectrum + per-part levels"
  echo "                              (default 1s; --from runs S seconds first, unmeasured;"
  echo "                              a 48 kHz WAV feeds input() and sets the length)"
  echo "  try <patch> [seconds] [--from S]"
  echo "                              render the patch as a send would land it NOW: the"
  echo "                              live voice's state and time, without touching the"
  echo "                              sound (default 2s). Catches NaN/runaways first"
  echo "  spectrum [voice]            spectral summary of the engine's recent buffer"
  echo "  kill                        shut down engine"
  quit 0

case args[0]
of "start":
  startEngine()
of "send":
  if args.len < 2: quit "usage: aither send <file> [fade]"
  let extra = if args.len >= 3: " " & args[2] else: ""
  let resp = queryCmd("send " & absolutePath(args[1]) & extra)
  # The meter in the OK line is read the instant the swap lands, before
  # any new sound has played. Listen for a moment (AITHER_SEND_WAIT
  # seconds, default 1, 0 to skip) and report what actually happened:
  # level, quiet parts, rollback or guard trips, the master.
  if not resp.startsWith("OK"):
    echo resp
    quit 1
  echo "OK"
  let wait = try: parseFloat(getEnv("AITHER_SEND_WAIT", "1")) except ValueError: 1.0
  if wait > 0.0:
    sleep(int(wait * 1000))
    let check = queryCmd("check " & splitFile(args[1]).name)
    echo (if check.startsWith("OK\n"): check[3 .. ^1] else: check)
of "stop":
  if args.len < 2: quit "usage: aither stop <name> [fade]"
  let extra = if args.len >= 3: " " & args[2] else: ""
  sendCmd("stop " & args[1] & extra)
of "mute":
  if args.len < 2: quit "usage: aither mute <voice> [play] [fade]"
  let extra = if args.len >= 3: " " & args[2 .. ^1].join(" ") else: ""
  sendCmd("mute " & args[1] & extra)
of "unmute":
  if args.len < 2: quit "usage: aither unmute <voice> [play] [fade]"
  let extra = if args.len >= 3: " " & args[2 .. ^1].join(" ") else: ""
  sendCmd("unmute " & args[1] & extra)
of "solo":
  if args.len < 2: quit "usage: aither solo <voice> [play] [fade]"
  let extra = if args.len >= 3: " " & args[2 .. ^1].join(" ") else: ""
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
of "check":
  if args.len < 2: quit "usage: aither check <voice>"
  sendCmd("check " & splitFile(args[1]).name)
of "midi":
  if args.len < 2: quit "usage: aither midi list|connect <spec>|disconnect"
  let rest = args[1 .. ^1].join(" ")
  sendCmd("midi " & rest)
of "audit", "try":
  let usage = "usage: aither " & args[0] & " <patch> [seconds] [--from S]" &
              (if args[0] == "audit": " [input.wav]" else: "")
  if args.len < 2: quit usage
  # A number is the duration; anything else is the input recording.
  var seconds = 0.0
  var skip = 0.0
  var inputPath = ""
  var i = 2
  while i < args.len:
    let a = args[i]
    if a == "--from" and i + 1 < args.len:
      try: skip = parseFloat(args[i + 1])
      except ValueError: quit usage
      i += 2
      continue
    try: seconds = parseFloat(a)
    except ValueError:
      if inputPath.len > 0 or args[0] == "try": quit usage
      inputPath = a
    inc i
  if args[0] == "audit":
    audit(args[1], seconds, inputPath, skip)
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
  quit 1
