# Demos

Field theory you can watch and hear, sized 16:9 to fill the uConsole's
screen. Most demos are one patch whose output is drawn by `tv` and
played at the same time: the picture is the sound.
`tv` passes the stream on unchanged, so it sits between the engine and
the speaker:

```
./aither demos/<demo>.aither | tools/tv --size WxH --smooth | tools/play hw:rockchipes8388c 20
```

At the uConsole's 44.1 kHz (`AITHER_RATE` in `.bashrc`), a W×H picture
is drawn 44100 / (W·H) times a second, and every pixel steps its field
once a frame, so a demo costs about one cell update per sample.
`./aither send demos/<demo>.aither` hot-swaps an edited demo as usual.
`tv` opens full screen (`--window` for a window); `f` toggles, `q`
closes it. Headphones help.

`plate` is the other kind: a picture that listens to music. `play`
hands the stream on after the speaker has it, so the picture is just the
next thing down the pipe, its `input()` the music you hear (a second
engine needs its own socket):

```
./aither demos/chorale.aither | tools/play hw:rockchipes8388c 20 | AITHER_SOCK=/tmp/aither-picture.sock ./aither demos/plate.aither | tools/tv --size 48x27 --smooth
```

| Demo | Size | What it shows |
|---|---|---|
| `phased_array` | 48x27 | Two antennas, one wave field (Maxwell). The phase between them turns, and the beam swings with nothing moving: phased arrays, Steinmetz phasors. Hue is each pixel's phase, brightness its power. You hear the beam's angle as pitch. |
| `ridge` | 64x36 | A radio wave meets a hill (Maxwell, Huygens, Fresnel). Waves from a mast bend over the ridge into the valley behind it, its radio shadow; the frequency sweeps, and longer waves reach further in. Ground reflections interfere near the mast. You hear the wavelength as pitch. |
| `chladni` | 48x27 | A rectangular plate driven near a corner, held on one resonance at a time (Chladni's sand figures, 1787). Each mode rings up into a standing wave: two colours, its halves moving opposite ways, and black nodal lines where the sand would gather. Every 30 s a hand stills it and the drive steps on: four single modes, then four pitches between two modes that ring almost together, which mix into figures neither has alone. |
| `plate` (with `chorale`) | 48x27 | A plate that listens. `chorale` plays a slow I IV vi V in just intonation, pad and struck bells; `plate` hears it through `input()` and rings one mode per note, found by a lock-in detector tuned to that note (as a radio finds a station). A chord is its notes' figures laid over each other; colour is the note's place in the octave; each figure turns at its note's pitch slowed 256 times, so consonant notes fall into step. Resonance and consonance, seen. Run with the pipe above. |
