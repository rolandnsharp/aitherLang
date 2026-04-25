## CLI dispatch for `aither <subcommand> ...`. Pure routing — every
## subcommand reads argv, then either calls the in-process engine
## (`start`) for the daemon side, or shells a command across the
## socket (`send`, `stop`, `list`, ...), or runs offline analysis
## (`audit`). No business logic lives here.

import std/[os, strutils]
import engine, render, analysis, cli_output

proc audit(path: string; seconds: float64) =
  ## Offline render → spectral analysis → text. Doesn't touch the
  ## engine; safe to run with no daemon.
  if not fileExists(path):
    stderr.writeLine "audit: file not found: " & path
    quit 1
  var lBuf, rBuf: seq[float64]
  try:
    (lBuf, rBuf) = renderPatch(path, seconds, 48000)
  except CatchableError as e:
    stderr.writeLine "audit: " & e.msg
    quit 1
  let mono = monoMix(lBuf, rBuf)
  let summary = analyze(mono, 48000.0)
  echo formatAudit(path, seconds, 48000.0, summary)

let args = commandLineParams()
if args.len == 0:
  echo "usage: aither <start|send|stop|mute|unmute|solo|clear|list|" &
       "scope|parts|retrigger|midi|audit|spectrum|kill> [args]"
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
  echo "  parts <voice>               list named parts (play blocks) with gain + state"
  echo "  midi list                   show ALSA seq ports"
  echo "  midi connect <spec>         subscribe to a specific port (e.g. '28:0')"
  echo "  midi disconnect             drop MIDI and re-open the sequencer"
  echo "  audit <patch> [seconds]     offline render + spectral summary (default 1.0s)"
  echo "  spectrum [voice]            spectral summary of the engine's recent buffer"
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
of "midi":
  if args.len < 2: quit "usage: aither midi list|connect <spec>|disconnect"
  let rest = args[1 .. ^1].join(" ")
  sendCmd("midi " & rest)
of "audit":
  if args.len < 2: quit "usage: aither audit <patch> [seconds]"
  let seconds =
    if args.len >= 3:
      try: parseFloat(args[2])
      except ValueError: quit "audit: seconds must be numeric"
    else: 1.0
  audit(args[1], seconds)
of "spectrum":
  let extra = if args.len >= 2: " " & args[1] else: ""
  sendCmd("spectrum" & extra)
of "kill":
  sendCmd("kill")
else:
  echo "unknown command: " & args[0]
  quit 1
