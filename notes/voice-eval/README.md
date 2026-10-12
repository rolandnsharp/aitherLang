# Voice evaluation (2026-10-11)

- `wer.py` + `score_voice.sh` / `score_pair.sh`: render the singer alone
  (lab, `~/aither-tools/towav`), split into the 8 lines, transcribe with
  whisper.cpp small.en (`~/whisper` on lab), word error rate averaged over
  three lead-ins. espeak speaking the lines: 6%.
- Live singer (patches/didge_scope.aither, single-release stops): ~30%.
- `didge_scope_vL_learned_vowels.aither`: vowels learned from Hillenbrand
  et al. 1995 (`learn_vowels.py`, 139 talkers, tract-normalised, start/
  mid/end), true diphthongs EY/OW (ids 37/38, `phon_v2_diphthongs.py`),
  a `vTract` knob: 35% (helps some lines, hurts "coding"). Not live.
- Next (agreed with Roland): a Gnuspeech-inspired singer of our own, not a
  clone: a physical waveguide tract (distinctive regions, like the didge),
  posture targets + transition rules, learned consonant loci and durations
  from LibriSpeech (dev-clean + MFA phone alignments, downloading on lab
  in ~/voice-learn), and live voice knobs (tract, register, breath, press,
  vibrato, robot blend, harmony).

## Physical singer, in progress (2026-10-11 evening)

- `fit_tract.py` (runs on lab, ~/voice-learn): fits 8-region tract areas
  (17 cm, lossy cylinders, lip radiation) to each learned vowel's F1-F3;
  writes /tmp/vowel_areas.npy on lab and prints areas + achieved formants.
- `gen_tube.py` -> `tube8.aither`: the aitherLang def, an 8-region
  Kelly-Lochbaum waveguide (delay pairs, scattering k from areas, glottis
  0.75, lips -0.85 lowpassed, frication injected at `place`, `tlen` scale).
- Next: render sustained vowels through tube8 with the fitted areas,
  measure formants (parselmouth, ~/voice-learn/venv), calibrate; then
  consonants as gestures over a moving vowel shape (labial region 8,
  alveolar 7, postalveolar 6, velar 4-5; velum branch for m/n/ng); then
  the score arrays (vowel substrate + gestures) and whisper scoring.

## Tube singer (2026-10-11 evening): 72% WER

`didge_scope_vT4_tube_singer.aither` (voice only; `phon_v3_tube.py` writes
its score: stop runs whole, `lyrV` = the vowel each slot heads for). The
tube's areas glide toward the target vowel; consonants are gestures
(place gPl, degree gDg, velum gVl) multiplied onto them; nose branch;
lowpassed turbulence injected at the constriction; per-vowel gains
(vGain) so open vowels lead; tube waves clamped (a nasal-to-silence
glitch spiked). WER path: 92% (noise too loud) -> 99% -> 73% (levels
equalised) -> 72%. Vowels and rhythm come through; consonants don't yet.
wer.py now normalises by RMS, not peak. The Klatt singer (30%) stays live.

## tube24 (2026-10-12)

24 sections, one sample each (17.5 cm at 48 kHz), shape from 8 control
points (linear), gesture (place, deg) and frication built in; the glottis
and lips are area junctions (a 0.15 cm^2 slit; 25 cm^2 of open air), waves
clamped at +-50. Vowel shapes from `fit_tract24.py` (larynx end held to
0.3-2.5 cm^2; a wide larynx broke the time-domain tube). Measured with a
60 Hz voice (`tube24_test60.aither`): median F1 16% high, F2 3%, F3 3%;
"hood" now right; "oo" (lips ~0.2 cm^2) still wrong and "er" F3 too high.
Next: model lip radiation identically in the fit and the tube (the
time-domain lip junction and the fit's piston load disagree at small
openings), then recalibrate F1, then bring tube24 into the singer.

## Tube singer on tube24 (2026-10-12): 62% WER

`didge_scope_vT24_tube_singer.aither`: tube24 with the exact-pole vowel
shapes, gestures passed as (place, degree). The big fix was lp1's units
again: the turbulence was `lp1(lp1(noise(), 4500), 4500)`, really ~700 Hz,
so every s/sh/f was a dull rumble heard as "h". Now `lp1(noise(), 31300)`
(~8 kHz) with frication and aspiration scaled to natural levels (s/sh -4,
f -18, t/p -6, k -8 dB re open vowels). WER: tube8 72% -> tube24 69% ->
62%. vTract has no effect on tube24 yet (fixed one-sample sections).

## Pink Trombone translation (2026-10-12)

`gen_trombone.py` -> `trombone.aither`: Neil Thapen's Pink Trombone 1.1
tract (MIT; original in third_party/), translated to aitherLang: 48
sections (17.5 cm; PT uses 44 = 16 cm) run twice per sample, a 31-section
nose through a 3-way velum junction, turbulence where the gesture makes a
narrow-but-open channel (PT's thinness x openness), release transients.
With our learned vowel shapes and no refit: F1 2%, F2 4%, F3 2% median
(Praat now reads "oo" correctly too). `didge_scope_vP_trombone_singer.aither`
adds PT's LF glottis (tenseness knob vTense) and glottal-cycle breath:
82% -> 73% WER after fixing a noise-placement off-by-one (f was silent).
Our tube24 singer is still ahead on Whisper (52%).
