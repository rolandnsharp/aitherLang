/* Per-note expression and MPE (SPEC.md, "MIDI input"): each held note
 * carries its channel, bend, pressure and controllers. Two notes of one
 * key on two channels are two voices; an MPE configuration message makes
 * member channels bend 48 semitones and adds the master's bend; what a
 * channel says before a note-on is the note's first value; a program sets
 * one note's values by key at full resolution; raw MIDI bytes do what the
 * writers do; and poly hands a voice its press and slide. */

#include "check.h"

static double hz(double note) { return 440.0 * pow(2.0, (note - 69.0) / 12.0); }

TEST(midi_mpe) {
  midi_reset_all();

  /* One key on two channels: two voices, and each lets go alone. */
  midi_note_on_ch(1, 60, 100);
  midi_note_on_ch(2, 60, 80);
  CHECK(n_midi_voice_freq(1) > 0 && n_midi_voice_freq(2) > 0, "two voices for one key on two channels");
  midi_note_off_ch(1, 60);
  CHECK(n_midi_voice_gate(1) == 0.0 && n_midi_voice_gate(2) == 1.0, "the channel 2 note let go, channel 3's held");
  midi_reset_all();

  /* Without a zone, a channel bends two semitones (MIDI's default); RPN 0
   * sets it, here to twelve. */
  midi_note_on_ch(0, 69, 100);
  midi_bend_ch(0, 16383);
  NEAR(n_midi_voice_freq(1), hz(69 + 2.0 * 8191 / 8192), 1e-9, "n_midi_voice_freq(1)");
  midi_cc_ch(0, 101, 0);
  midi_cc_ch(0, 100, 0);
  midi_cc_ch(0, 6, 12);
  NEAR(n_midi_voice_bend(1), 12.0 * 8191 / 8192, 1e-12, "n_midi_voice_bend(1)");
  NEAR(n_midi_freq(), hz(69 + 12.0 * 8191 / 8192), 1e-9, "n_midi_freq()");
  midi_reset_all();

  /* An MPE lower zone of 15 members (RPN 6 on channel 1): each member
   * bends 48 and its notes take the master's 2 on top. */
  midi_cc_ch(0, 101, 0);
  midi_cc_ch(0, 100, 6);
  midi_cc_ch(0, 6, 15);
  midi_bend_ch(1, 8192 + 2048);                   /* an eighth up, sent before the note: its first value */
  midi_pressure_ch(1, 64);
  midi_cc_ch(1, 74, 127);
  midi_note_on_ch(1, 60, 100);
  midi_note_on_ch(2, 64, 100);
  NEAR(n_midi_voice_bend(1), 12.0, 1e-12, "n_midi_voice_bend(1)");
  NEAR(n_midi_voice_freq(1), hz(72), 1e-9, "n_midi_voice_freq(1)");
  NEAR(n_midi_voice_press(1), 64.0 / 127.0, 1e-12, "n_midi_voice_press(1)");
  NEAR(n_midi_voice_cc(1, 74), 1.0, 1e-12, "n_midi_voice_cc(1");
  NEAR(n_midi_voice_bend(2), 0.0, 1e-12, "n_midi_voice_bend(2)");
  NEAR(n_midi_voice_press(2), 0.0, 1e-12, "n_midi_voice_press(2)");
  NEAR(n_midi_voice_cc(2, 74), 0.0, 1e-12, "n_midi_voice_cc(2");
  midi_bend_ch(0, 16383);                         /* the master: every member note */
  NEAR(n_midi_voice_bend(1), 12.0 + 2.0 * 8191 / 8192, 1e-12, "n_midi_voice_bend(1)");
  NEAR(n_midi_voice_bend(2), 2.0 * 8191 / 8192, 1e-12, "n_midi_voice_bend(2)");
  midi_note_off_ch(1, 60);
  midi_bend_ch(1, 0);                             /* after the note let go: its tail keeps its bend */
  NEAR(n_midi_voice_bend(1), 12.0 + 2.0 * 8191 / 8192, 1e-12, "n_midi_voice_bend(1)");
  midi_reset_all();

  /* A program sets one held note's values, at full resolution. */
  midi_note_on(40, 100);
  midi_note_on(41, 100);
  midi_note_cc(41, 20, 0.123456789);
  midi_note_bend(41, -0.5);
  midi_note_pressure(41, 0.25);
  NEAR(n_midi_voice_cc(2, 20), 0.123456789, 1e-15, "n_midi_voice_cc(2");
  NEAR(n_midi_voice_cc(1, 20), 0.0, 1e-15, "n_midi_voice_cc(1");
  NEAR(n_midi_voice_freq(2), hz(40.5), 1e-9, "n_midi_voice_freq(2)");
  NEAR(n_midi_voice_press(2), 0.25, 1e-15, "n_midi_voice_press(2)");
  midi_reset_all();

  /* Raw bytes: a note on channel 5, its bend and pressure. */
  unsigned char on[3] = {0x94, 62, 90}, bend[3] = {0xE4, 0, 96}, press[2] = {0xD4, 127}, off[3] = {0x84, 62, 0};
  midi_message(on, 3);
  midi_message(bend, 3);
  midi_message(press, 2);
  NEAR(n_midi_voice_bend(1), (96 * 128 - 8192) / 8192.0 * 2.0, 1e-12, "n_midi_voice_bend(1)");
  NEAR(n_midi_voice_press(1), 1.0, 1e-12, "n_midi_voice_press(1)");
  midi_message(off, 3);
  CHECK(n_midi_voice_gate(1) == 0.0, "the raw note-off let go");
  midi_reset_all();

  /* poly hands each voice its press and slide; the voice loop holds. */
  midi_cc_ch(0, 101, 0);
  midi_cc_ch(0, 100, 6);
  midi_cc_ch(0, 6, 15);
  midi_pressure_ch(1, 127);
  midi_note_on_ch(1, 60, 100);
  midi_cc_ch(2, 74, 127);
  midi_note_on_ch(2, 62, 100);
  Voice* v = must_load("poly(4, (freq, gate, vel, key, press, slide) => press * 10 + slide)", 48000.0);
  if (v) {
    double l, r;
    tick_at(v, 0.0, &l, &r);
    NEAR(l, 11.0, 1e-12, "l");
  }
  Voice* w = must_load("sum(4, n => midi_voice_cc(n, 74) * 100 + midi_voice_press(n))", 48000.0);
  if (w) {
    double l, r;
    tick_at(w, 0.0, &l, &r);
    NEAR(l, 101.0, 1e-12, "l");
  }
  midi_reset_all();
}
