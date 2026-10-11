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
