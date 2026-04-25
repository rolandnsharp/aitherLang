## End-to-end audit pipeline — render an inline patch, run analysis,
## format. The CLI's `audit` subcommand calls these same procs.

import std/[strutils]
import ../render, ../analysis, ../cli_output

# --- 1. Pure 440 Hz sine — formatted output mentions 440 in the
# top peaks AND the fundamental.
block sineAudit:
  let (l, r) = renderPatchSrc("sin(TAU * phasor(440)) * 0.3", 1.0, 48000)
  let mono = monoMix(l, r)
  let summary = analyze(mono, 48000.0)
  let txt = formatAudit("<inline>", 1.0, 48000.0, summary)
  doAssert "audit: <inline>" in txt, "header missing: " & txt
  doAssert "440" in txt, "no 440 in output: " & txt
  doAssert "RMS:" in txt
  doAssert "Top peaks:" in txt

# --- 2. Two-tone — both freqs surface in formatted text.
block twoToneAudit:
  let src = """
let s1 = sin(TAU * phasor(440)) * 0.3
let s2 = sin(TAU * phasor(880)) * 0.3
s1 + s2
"""
  let (l, r) = renderPatchSrc(src, 0.5, 48000)
  let mono = monoMix(l, r)
  let summary = analyze(mono, 48000.0)
  let txt = formatAudit("<inline>", 0.5, 48000.0, summary)
  doAssert "440" in txt, "440 missing: " & txt
  doAssert "880" in txt, "880 missing: " & txt

# --- 3. Silent input → all -inf (or -240) numbers, no top peaks.
block silentAudit:
  let buf = newSeq[float64](24000)     # all zeros
  let summary = analyze(buf, 48000.0)
  let txt = formatAudit("<silent>", 0.5, 48000.0, summary)
  doAssert "non-tonal" in txt, "silent should show 'non-tonal': " & txt
  doAssert summary.peaks.len == 0, "silent input should have no peaks"

# --- 4. Header format pinned: `audit: <path> (<sec>s @ <sr> Hz)`.
block headerFormat:
  let summary = SpectrumSummary(sr: 48000.0)
  let txt = formatAudit("foo.aither", 2.0, 48000.0, summary)
  doAssert txt.startsWith("audit: foo.aither (2.0s @ 48000 Hz)\n"),
    "header byte-format: " & txt.split('\n')[0]

echo "audit ok"
