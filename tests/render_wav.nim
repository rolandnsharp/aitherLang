## `aither render`: a patch to a WAV, stereo or five channels (mix plus
## the beam X/Y/Z) when it has a scope part.

import std/[os, streams, math]
import ../render

let dir = getTempDir() / "aither_render_" & $getCurrentProcessId()
createDir(dir)

writeFile(dir / "tone.aither", "play a:\n  [sin(TAU * phasor(440)) * 0.5, 0.25]\na\n")
let r = renderPatchToWav(dir / "tone.aither", 0.1, dir / "tone.wav")
doAssert not r.hasScope
let back = readWav(dir / "tone.wav", 48000)
doAssert back.left.len == 4800 and back.right.len == 4800, $back.left.len
doAssert abs(back.right[100] - 0.25) < 1e-6, $back.right[100]
var peak = 0.0
for x in back.left: peak = max(peak, abs(x))
doAssert peak > 0.49 and peak <= 0.5 + 1e-6, $peak

proc channelsOf(path: string): int =
  let f = newFileStream(path, fmRead)
  defer: f.close()
  f.setPosition(22)
  int(f.readUint16())

writeFile(dir / "scoped.aither",
  "play a:\n  sin(TAU * phasor(220)) * 0.3\nplay scope:\n  [sin(TAU * phasor(2)), cos(TAU * phasor(2))]\nplay beam:\n  0.5 + 2 * t\na\n")
let s = renderPatchToWav(dir / "scoped.aither", 0.05, dir / "scoped.wav", withScope = true)
doAssert s.hasScope and s.beam.x.len == 2400
doAssert channelsOf(dir / "scoped.wav") == 5, $channelsOf(dir / "scoped.wav")
doAssert abs(s.beam.y[0] - 1.0) < 1e-6 and abs(s.beam.z[0] - 0.5) < 1e-9, $s.beam.y[0] & " " & $s.beam.z[0]
doAssert s.beam.z[^1] <= 1.0, "beam intensity is clamped to 0..1"
# Without --scope the same patch writes two channels.
discard renderPatchToWav(dir / "scoped.aither", 0.05, dir / "scoped2.wav")
doAssert channelsOf(dir / "scoped2.wav") == 2
# The mix excludes the scope parts: a is the only heard part.
var mixPeak = 0.0
for x in s.audio.left: mixPeak = max(mixPeak, abs(x))
doAssert mixPeak <= 0.3 + 1e-9, $mixPeak
removeDir(dir)
echo "render wav ok"
