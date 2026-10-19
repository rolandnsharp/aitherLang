# Demos

Field theory you can watch and hear. Each demo is one patch whose output
is drawn by `tv` and played at the same time: the picture is the sound.
`tv` passes the stream on unchanged, so it sits between the engine and
the speaker:

```
./aither demos/<demo>.aither | tools/tv --size WxH --smooth | tools/play hw:rockchipes8388c 20
```

At the uConsole's 44.1 kHz (`AITHER_RATE` in `.bashrc`), a W×H picture
is drawn 44100 / (W·H) times a second, and every pixel steps its field
once a frame, so a demo costs about one cell update per sample.
`./aither send demos/<demo>.aither` hot-swaps an edited demo as usual.
`f` in `tv` is full screen, `q` closes it. Headphones help.

| Demo | Size | What it shows |
|---|---|---|
| `phased_array` | 32x24 | Two antennas, one wave field (Maxwell). The phase between them turns, and the beam swings with nothing moving: phased arrays, Steinmetz phasors. Hue is each pixel's phase, brightness its power. You hear the beam's angle as pitch. |
| `ridge` | 48x32 | A radio wave meets a hill (Maxwell, Huygens, Fresnel). Waves from a mast bend over the ridge into the valley behind it, its radio shadow; the frequency sweeps, and longer waves reach further in. Ground reflections interfere near the mast. You hear the wavelength as pitch. |
| `chladni` | 40x40 | A square drumhead driven at one point, held on one resonance at a time (Chladni's sand figures, 1787). Each mode rings up into a standing wave: two colours, its halves moving opposite ways, and black nodal lines where the sand would gather. Every 20 s a hand stills it and the drive steps to the next mode; (m, n) and (n, m) ring together and mix into figures neither has alone. |
