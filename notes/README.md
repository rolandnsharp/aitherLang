# notes

Research notebooks, speculation and parked plans. None of this is the
language: for that, see the [README](../README.md), [SPEC](../SPEC.md)
and [GUIDE](../GUIDE.md). Several of these record ideas that were
tried and dropped; each doc says so near the top where that's the case.

**Directions to chase**
- [SOUND_FRONTIERS.md](SOUND_FRONTIERS.md) — unexplored regions of
  additive synthesis.
- [GAME_AUDIO_ROADMAP.md](GAME_AUDIO_ROADMAP.md) — aither as a
  procedural game-audio engine. Parked.
- [learnable_filters.md](learnable_filters.md) — filters trained from
  input/output pairs. Parked.
- [sensing-2026-10-16.md](sensing-2026-10-16.md) — aither between a
  sensor and a display: EMF, natural radio and lightning, cosmic rays,
  RF/5G, electrical work, video at audio rate. Brainstorm.
- [card-2026-10-16.md](card-2026-10-16.md) — a duplex tool, one clock
  for output and input: a fixed, known loop delay for lock-in, sweeps
  and the metal detector. Proposed, not built; no engine change.

**Decisions**
- [pair-output-2026-10-15.md](pair-output-2026-10-15.md) — the output
  is a pair, a float is `[s, s]`, and the sink on the end of the pipe
  (speaker, visualiser, radio, file) gives the numbers their meaning.
  One session is heard or seen or transmitted, not all at once. What
  it means for the `scope` merge.
- [displays-2026-10-16.md](displays-2026-10-16.md) — `scope` (vector)
  and `tv` (raster, domain colouring) as filters on the pair stream;
  two live-coded engines, one for sound and one for visuals; no
  effects in the displays. Both built.

**Design notebooks**
- [cybernetics.md](cybernetics.md) — the cybernetic-synthesis tradition
  (Tudor, al-Mukabala) and what it means for aither.
- [pole.md](pole.md) — a regime-switching resonator primitive, with the
  negative findings that ended it.
- [bachPolyphase.md](bachPolyphase.md) — Bach polyphony as polyphase
  mathematics.
- [yingyang.md](yingyang.md) — a lens on the pair operations.
- [philosophicalSpeculations.md](philosophicalSpeculations.md) —
  half-formed bets about where `f(state) → sample` could go.
- [ken.md](ken.md) — running notes from the Ken Wheeler / Theoria
  Apophasis videos.
- [MANIFESTO.md](MANIFESTO.md) — a draft of what the project might
  become.

**Hardware**
- [MINILAB3.md](MINILAB3.md) — Arturia MiniLab 3 MIDI mapping.
