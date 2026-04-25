## render.nim regression tests — pure offline render produces the
## expected sample count and energy without depending on the engine.

import std/[math, os]
import ../render

# --- 1. Pure 440 Hz sine for 1s @ 48kHz → 48000 samples per channel,
# peak ≈ 1.0, no NaN.
block sineRender:
  let (l, r) = renderPatchSrc("sin(TAU * phasor(440))", 1.0, 48000)
  doAssert l.len == 48000, "left has " & $l.len & " samples"
  doAssert r.len == 48000, "right has " & $r.len & " samples"
  var peak = 0.0
  for v in l:
    if v != v: doAssert false, "NaN in left"
    if abs(v) > peak: peak = abs(v)
  doAssert peak > 0.95 and peak <= 1.0,
    "440 Hz sine peak should be ~1.0, got " & $peak

# --- 2. Mono patch mirrors to both channels (engine.tick semantics).
block monoMirrors:
  let (l, r) = renderPatchSrc("sin(TAU * phasor(220)) * 0.3", 0.1, 48000)
  for i in 0 ..< l.len:
    doAssert l[i] == r[i], "mono should mirror at sample " & $i

# --- 3. Stereo patch returns two distinct channels.
block stereoDiffers:
  const Src = """
let s1 = sin(TAU * phasor(440)) * 0.3
let s2 = sin(TAU * phasor(660)) * 0.3
[s1, s2]
"""
  let (l, r) = renderPatchSrc(Src, 0.1, 48000)
  var anyDiff = false
  for i in 0 ..< l.len:
    if l[i] != r[i]: anyDiff = true; break
  doAssert anyDiff, "stereo channels should differ"

# --- 4. Render the real backing.aither for 0.5s — non-zero, no NaN.
# Confirms the renderer handles the full stdlib + non-trivial patches.
block backingRenders:
  let (l, r) = renderPatch("patches/backing.aither", 0.5, 48000)
  doAssert l.len == 24000
  var energy = 0.0
  for v in l: energy += v * v
  for v in r: energy += v * v
  doAssert energy > 0.001,
    "backing.aither should produce energy, got " & $energy

# --- 5. Zero seconds → empty buffers (smoke test for "does it parse?").
block zeroSeconds:
  let (l, r) = renderPatchSrc("sin(TAU * phasor(220))", 0.0, 48000)
  doAssert l.len == 0
  doAssert r.len == 0

# --- 6. Missing file → IOError (specific exception, not silent).
block missingFile:
  var raised = false
  try:
    discard renderPatch("/nonexistent.aither", 0.1, 48000)
  except IOError:
    raised = true
  doAssert raised, "missing file should raise IOError"

# --- 7. monoMix produces (L+R)*0.5.
block monoMixOk:
  let l = @[1.0, 0.5, -0.2]
  let r = @[0.0, 0.5, 0.4]
  let m = monoMix(l, r)
  doAssert m == @[0.5, 0.5, 0.1], "monoMix: " & $m

echo "render ok"
