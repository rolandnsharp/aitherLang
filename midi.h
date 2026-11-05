/* MIDI input as signals. Note-ons, note-offs and CCs arrive on their own
 * thread (ALSA seq, alsa_midi.c) and update one process-wide state; a
 * patch's code reads it every sample through the n_midi_* natives. The
 * state is the engine's, not a voice's, so a hot reload keeps a held
 * note and a knob's last position. */

#ifndef AITHER_MIDI_H
#define AITHER_MIDI_H

/* Notes held at once. A new one past this evicts the oldest. */
#define MIDI_MAX_POLYPHONY 16

/* The writer side: the MIDI thread, a script, an export's host, a test.
 * Channels are 0..15; the forms without one are channel 0 (MIDI's 1). */
void  midi_note_on(int note, int velocity);
void  midi_note_off(int note);
void  midi_cc(int cc, int value);
void  midi_note_on_ch(int ch, int note, int velocity);   /* velocity 0 lets go */
void  midi_note_off_ch(int ch, int note);
void  midi_cc_ch(int ch, int cc, int value);
void  midi_bend_ch(int ch, int value);                   /* 0..16383, 8192 centred */
void  midi_pressure_ch(int ch, int value);               /* channel pressure 0..127 */
/* A held note's own values, by key, at full resolution. */
void  midi_note_cc(int note, int cc, double value);      /* 0..1 */
void  midi_note_bend(int note, double semitones);
void  midi_note_pressure(int note, double value);        /* 0..1 */
/* One raw MIDI 1.0 message: notes, controllers, pressure, bend. */
void  midi_message(const unsigned char* m, int len);
void  midi_reset_all(void);

/* The sequencer. Text is malloc'd. */
int   midi_open(const char* client_name);      /* 1 on success */
char* midi_list_ports(void);                   /* "client:port\tname" lines, "" if none */
char* midi_auto_connect(void);                 /* who it subscribed to, "" if nobody */
int   midi_connect(const char* spec);          /* "28:0" or "Minilab3:0"; 1 on success */
void  midi_start_thread(void);
void  midi_shutdown(void);
int   midi_subscription_active(void);
const char* midi_last_connect_info(void);      /* "" if never connected */
/* Resubscribe if the input thread had silently died: NULL if there was
 * nothing to do, else "ok: ..." or "fail: ..." (malloc'd). */
char* midi_resubscribe_if_dropped(void);

/* The natives a patch calls (voice.c registers them with TCC). */
double n_midi_cc(int n);
double n_midi_note(int n);
double n_midi_freq(void);
double n_midi_gate(void);
double n_midi_vel(void);
double n_midi_voice_freq(int n);
double n_midi_voice_gate(int n);
double n_midi_voice_vel(int n);
double n_midi_voice_key(int n);
double n_midi_trig(void* state, long at, int n);
double n_midi_voice_cc(int n, int k);
double n_midi_voice_press(int n);
double n_midi_voice_bend(int n);
double n_midi_voice_on(int n);

#endif
