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
