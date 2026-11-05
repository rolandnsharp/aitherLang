/* aither_rt.h: the runtime every patch exported by `aither export` runs
 * on, written beside it as aither_rt.c. One per program, however many
 * patches it holds: their filters, delays and reverbs are here, and so is
 * MIDI's state, which every exported patch reads, as every voice in an
 * engine does. Build it with the patches:
 *
 *     cc -O2 -ffp-contract=off -c aither_rt.c country.c
 *
 * -ffp-contract=off keeps the arithmetic aither's own (no fused
 * multiply-adds), so an exported patch plays what the engine plays, sample
 * for sample. Without it the sound is the same to the ear, not to the bit.
 *
 * Hand the patches their keys and knobs here, from anything: a MIDI
 * device, a game, a script. The values are MIDI's (0..127); they take
 * effect on the next sample a patch renders. Call them from one thread
 * at a time (a MIDI callback, the game loop); they may run beside a
 * render on another. */
#ifndef AITHER_RT_H
#define AITHER_RT_H

/* Controller n (0..127) to value (0..127). 0..31 pair with 32..63 into 14
 * bits, as in the engine (SPEC.md, "MIDI input"): coarse on n, fine on
 * n + 32. */
void aither_cc(int n, int value);
/* Controller n (0..31) to a 14-bit value (0..16383): both halves. */
void aither_cc14(int n, int value);
/* A key down (velocity 1..127) or up. */
void aither_note_on(int note, int velocity);
void aither_note_off(int note);
/* Raw MIDI 1.0, one message (note on/off, controller, channel pressure,
 * pitch bend), on its channel: a plugin's or a device's MIDI as it comes,
 * MPE included. */
void aither_midi(const unsigned char* msg, int len);

/* A held note's own values, by key, at full resolution: what a game
 * gives one instance (an arrow, a footstep) and nothing else hears. A
 * patch reads them in its voices: midi_voice_cc(n, k), midi_voice_bend,
 * midi_voice_press (poly hands it press and slide, CC 74). Set them right
 * after the note-on; a note that isn't held ignores them. */
void aither_note_cc(int note, int cc, double value);      /* 0..1 */
void aither_note_bend(int note, double semitones);
void aither_note_pressure(int note, double value);        /* 0..1 */

/* Every key up, every knob at 0, as a fresh engine has them. */
void aither_midi_reset(void);

#endif
