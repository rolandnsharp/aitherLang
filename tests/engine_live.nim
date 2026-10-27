## The live engine, as a separate process: the control socket, the pipe,
## start and stop. Each check pins a bug found 2026-10-17 that no other
## test could see, because the others never start an engine.
##
## Runs ./aither (make builds it first). A watchdog fails the test after
## 60 s rather than let a regression hang it, since several of these
## bugs were hangs.

import std/[os, osproc, strutils, net, streams, times, exitprocs, math]
from std/posix import Pid, SIGKILL

const Exe = "./aither"
let dir = getTempDir() / ("aither-live-" & $getCurrentProcessId())
createDir(dir)
let sock = dir / "s.sock"
putEnv("AITHER_SOCK", sock)
putEnv("AITHER_RATE", "48000")       # not the shell's (44100 on the uConsole)
putEnv("AITHER_BUFFER", "512")
writeFile(dir / "quiet.aither", "0\n")
writeFile(dir / "half.aither", "input() * 0.5\n")

# Whatever happens, leave no engine running and no directory behind.
var started: seq[int]
addExitProc(proc() =
  for pid in started: discard posix.kill(Pid(pid), SIGKILL)
  removeDir(dir))

proc watchdog() {.thread.} =
  sleep(60_000)
  stderr.writeLine "engine_live: timed out (a hang is the regression)"
  quit(1)
var wd: Thread[void]
createThread(wd, watchdog)

proc ask(cmd: string): string =
  ## One command over the socket, its reply ("" if no engine answers).
  var s = newSocket(AF_UNIX, SOCK_STREAM, IPPROTO_IP)
  try:
    s.connectUnix(sock)
    s.send(cmd & "\n")
    result = s.recv(65536, timeout = 5000)
  except CatchableError:
    result = ""
  finally:
    s.close()

proc waitUp() =
  for _ in 0 ..< 200:
    if ask("list").startsWith("OK"): return
    sleep(50)
  doAssert false, "the engine never answered on " & sock

proc start(patch: string; args: seq[string] = @[]): Process =
  result = startProcess(Exe, args = @[dir / patch] & args, options = {})
  started.add result.processID

proc waitExit(p: Process; ms = 10_000): int =
  let t0 = epochTime()
  while p.running and epochTime() - t0 < ms / 1000: sleep(20)
  doAssert not p.running, "the engine didn't exit within " & $ms & " ms"
  p.waitForExit()

# --- 1. A live engine answers. (Its stdin is closed: input() is silence,
# and it runs until its stdout pipe fills, then waits on it.)
var a = start("quiet.aither")
a.inputStream.close()
waitUp()

# --- 2. A client that connects and hangs up without a command, and one
# that hangs up before reading its reply. Nim's send with SafeDisconn
# retried a closed peer forever and froze the command loop.
block hangUps:
  var s = newSocket(AF_UNIX, SOCK_STREAM, IPPROTO_IP)
  s.connectUnix(sock); s.close()
  doAssert ask("list").startsWith("OK"), "after a silent hang-up"
  var s2 = newSocket(AF_UNIX, SOCK_STREAM, IPPROTO_IP)
  s2.connectUnix(sock); s2.send("list\n"); s2.close()
  sleep(200)
  doAssert ask("list").startsWith("OK"), "after a hang-up mid-reply"

# --- 3. A second engine on the same socket says one is running and stops.
block second:
  let b = start("quiet.aither")
  let code = b.waitExit()
  let err = b.errorStream.readAll()
  doAssert code != 0 and "already running" in err, "second engine: " & $code & " " & err
  b.close()
  doAssert ask("list").startsWith("OK"), "the first engine is untouched"

# --- 4. An engine killed outright leaves its socket file; the next one
# clears it. fileExists is false for a socket, so it never did.
discard posix.kill(Pid(a.processID), SIGKILL)
discard a.waitForExit()
a.close()
doAssert ask("list") == "", "the killed engine is gone"
var c = start("quiet.aither")
c.inputStream.close()
waitUp()

# --- 5. `kill` ends it; "bye" goes to stderr, and stdout is whole float
# pairs to the last byte (it used to end in the text "bye\n").
doAssert "OK" in ask("kill")
discard c.waitExit()
let outBytes = c.outputStream.readAll()
let errText = c.errorStream.readAll()
c.close()
doAssert outBytes.len mod 8 == 0, "stdout ends mid-pair: " & $outBytes.len & " bytes"
doAssert "bye" notin outBytes, "bye leaked into the float stream"
doAssert "bye" in errText, "bye belongs on stderr: " & errText

# --- 6. Lockstep: one block out for each block in, nothing held back.
# stdio read ahead on stdin, so it answered only every second block and
# tools/card deadlocked.
block lockstep:
  let d = start("half.aither")
  waitUp()
  const Frames = 512
  var blockIn = newString(Frames * 8)
  for i in 0 ..< Frames:
    var l = 0.8'f32
    var r = -0.4'f32
    copyMem(addr blockIn[i * 8], addr l, 4)
    copyMem(addr blockIn[i * 8 + 4], addr r, 4)
  for n in 1 .. 5:
    d.inputStream.write(blockIn)
    d.inputStream.flush()
    let got = d.outputStream.readStr(Frames * 8)      # hangs if not lockstep
    doAssert got.len == Frames * 8, "block " & $n & ": " & $got.len & " bytes"
    var l, r: float32
    copyMem(addr l, unsafeAddr got[(Frames - 1) * 8], 4)
    copyMem(addr r, unsafeAddr got[(Frames - 1) * 8 + 4], 4)
    doAssert abs(l - 0.4) < 1e-6 and abs(r + 0.2) < 1e-6,
      "block " & $n & " is input() * 0.5: " & $l & " " & $r
  doAssert "OK" in ask("kill")
  d.inputStream.close()
  discard d.waitExit()
  d.close()

# --- 7. Offline, a patch that doesn't call input() doesn't read stdin, so
# an idle pipe there (ssh, a script) can't make it wait.
block offlineIdleStdin:
  let e = start("quiet.aither", @["1"])               # stdin: an open, idle pipe
  let t0 = epochTime()
  let bytes = e.outputStream.readAll()                # until it exits (or the watchdog)
  let code = e.waitExit()
  e.close()
  doAssert code == 0 and epochTime() - t0 < 10, "offline render waited"
  doAssert bytes.len == 48000 * 8, "one second of pairs: " & $bytes.len

# --- 8. The stream is gapless: every sample written is one tick of t,
# however busy the control socket is. mixBlock used to tryAcquire the
# lock and, when a command held it, write a block of silence without
# advancing the clock; tv counts pixels by samples received and the
# patch by t, so one such block sheared the picture for good.
var spamDone: bool
var slowestMs: int                     # the slowest reply while the engine ran flat out
proc spam() {.thread.} =
  {.cast(gcsafe).}:
    for i in 0 ..< 400:
      let t0 = epochTime()
      discard ask(["status", "levels", "list"][i mod 3])
      slowestMs = max(slowestMs, int((epochTime() - t0) * 1000))
    spamDone = true

block gapless:
  writeFile(dir / "clock.aither", "[((t * sr) % 65536) / 65536, 0]\n")
  let g = start("clock.aither")
  g.inputStream.close()
  waitUp()
  var th: Thread[void]
  createThread(th, spam)
  const Chunk = 4096
  var prev = -1
  var samples, gaps = 0
  while not spamDone or samples < 48000 * 4:
    let got = g.outputStream.readStr(Chunk * 8)
    doAssert got.len == Chunk * 8, "the stream ended early"
    for i in 0 ..< Chunk:
      var c: float32
      copyMem(addr c, unsafeAddr got[i * 8], 4)
      let tick = int(round(c * 65536))
      if prev >= 0 and (tick - prev + 65536) mod 65536 != 1: inc gaps
      prev = tick
    samples += Chunk
  joinThread(th)
  doAssert gaps == 0, $gaps & " gaps in " & $samples & " samples while the socket was busy"
  # --- 9. ...and commands still get in. This test reads as fast as it
  # can, so the engine runs flat out, like an overloaded set: the audio
  # thread took the lock back the moment it let go, and a command could
  # wait seconds (lockForCommand lets it in first).
  doAssert slowestMs < 500, "a command waited " & $slowestMs & " ms for the engine"
  doAssert "OK" in ask("kill")
  discard g.waitExit()
  g.close()

# --- 10. `sweep`: a voice's own time, read and set over the socket. The
# patch plays its sweep (in thousands of seconds) as its left side.
block sweepCmd:
  writeFile(dir / "sw.aither", "[sweep / 1000, 0]\n")
  let w = start("sw.aither")
  w.inputStream.close()
  waitUp()
  proc lastLeft(): float32 =
    let got = w.outputStream.readStr(512 * 8)
    doAssert got.len == 512 * 8, "the stream ended early"
    copyMem(addr result, unsafeAddr got[511 * 8], 4)
  discard lastLeft()
  let r = ask("sweep sw")
  doAssert r.startsWith("OK"), "sweep sw: " & r
  let at = parseFloat(r.splitLines()[1])
  doAssert at >= 0 and at < 30, "a new voice's sweep starts near 0: " & r
  doAssert ask("sweep sw 500").startsWith("OK")
  var v = 0'f32
  for _ in 0 ..< 200:                   # blocks already in the pipe come first
    v = lastLeft()
    if v > 0.49: break
  doAssert v > 0.499 and v < 0.52, "after sweep 500 the patch reads about 500 s: " & $v
  doAssert ask("sweep nobody").startsWith("ERR"), "an unknown voice"
  doAssert ask("sweep sw soon").startsWith("ERR"), "seconds must be a number"
  doAssert "OK" in ask("kill")
  discard w.waitExit()
  w.close()

echo "engine_live ok"
quit(0)       # through the exit procs: the watchdog thread is still alive
