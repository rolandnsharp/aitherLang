/* pad: the uConsole's gamepad as eight MIDI knobs.
     tools/pad &                 then: aither midi connect <the aither-pad port>
   Hold A, B, X or Y and press the d-pad: up/down moves that button's
   first knob, left/right its second. Knobs are 0..127 and keep their
   value when you let go. A press steps by 6 (about 20 presses across
   the range); holding the d-pad for 300 ms sweeps, faster the longer it
   is held (our own repeat: this keyboard sends none). Sent as MIDI CC 20..27 on a virtual ALSA
   sequencer port called "aither-pad"; a patch reads them with
   midi_cc(20) etc.
       A: cc20 up/down  cc21 left/right      B: cc22 cc23
       X: cc24 cc25                          Y: cc26 cc27
   Each button is also a note, sent as it goes down and up: A 36, B 37,
   X 38, Y 39 (midi_trig(36) strikes on A). A patch that plays every note
   it hears, as midi_keyboard does, hears these too.
   The d-pad is the keyboard's arrow keys, so while a button is held the
   keyboard is grabbed (arrows don't reach the terminal); release and it
   is yours again. Buttons come from the gamepad device.

     tools/pad --notes           every key a note, no knobs: for games
   With --notes the d-pad is four more notes, up 40, down 41, left 42,
   right 43, sent as they go down and up whether a button is held or not,
   so anything can be held with anything (midi_note(42) is left, held).
   Nothing is grabbed: the arrows reach whatever has the focus too (the
   scope's window, full screen, ignores them).
   Build: cc -O2 tools/pad.c -o tools/pad -lasound */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <linux/input.h>
#include <alsa/asoundlib.h>

#define PAD "/dev/input/by-id/usb-ClockworkPI_uConsole_20230713-event-joystick"
#define KBD "/dev/input/by-id/usb-ClockworkPI_uConsole_20230713-event-kbd"
#define FIRST_CC 20

static const char *names[4] = {"A", "B", "X", "Y"};

static int bank_of(int code) {            /* the gamepad's button codes */
    switch (code) { case 289: return 0; case 290: return 1; case 288: return 2; case 291: return 3; }   /* A B X Y, read off the uConsole 2026-10-29 */
    return -1;
}

/* A key as a note, as it goes down (on) and up. */
static void send_note(snd_seq_t *seq, int port, int note, int on) {
    snd_seq_event_t e;
    snd_seq_ev_clear(&e);
    snd_seq_ev_set_source(&e, port);
    snd_seq_ev_set_subs(&e);
    snd_seq_ev_set_direct(&e);
    if (on) snd_seq_ev_set_noteon(&e, 0, note, 100);
    else snd_seq_ev_set_noteoff(&e, 0, note, 0);
    snd_seq_event_output_direct(seq, &e);
}

int main(int argc, char **argv) {
    int notes = argc > 1 && !strcmp(argv[1], "--notes");
    if (notes) { argc--; argv++; }
    const char *pad = argc > 1 ? argv[1] : PAD;
    const char *kbd = argc > 2 ? argv[2] : KBD;
    int fp = open(pad, O_RDONLY), fk = open(kbd, O_RDONLY);
    if (fp < 0) { perror(pad); return 1; }
    if (fk < 0) { perror(kbd); return 1; }

    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { fprintf(stderr, "pad: no ALSA sequencer\n"); return 1; }
    snd_seq_set_client_name(seq, "aither-pad");
    int port = snd_seq_create_simple_port(seq, "pad",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) { fprintf(stderr, "pad: no port\n"); return 1; }
    fprintf(stderr, "aither-pad: client %d port %d  (aither midi connect %d:%d)\n",
            snd_seq_client_id(seq), port, snd_seq_client_id(seq), port);

    int knob[8] = {0};
    int held = -1;            /* which button is down, or -1 */
    int arrow = 0;            /* d-pad key currently down, or 0 */
    int ticks = 0;            /* repeat ticks so far in this d-pad hold */

    struct pollfd pf[2] = {{fp, POLLIN, 0}, {fk, POLLIN, 0}};
    for (;;) {
        /* a tap is one step: the sweep waits until the d-pad has been held
           300 ms, as a keyboard's repeat does (at 45 ms every tap swept too) */
        int r = poll(pf, 2, (held >= 0 && arrow) ? (ticks == 0 ? 300 : 45) : -1);
        if (r < 0) break;
        /* the gamepad or keyboard gone (unplugged, the session ended): poll
           would report it at once, every time, and the loop would spin */
        if ((pf[0].revents | pf[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) break;
        struct input_event ev;
        if (r == 0 && held >= 0 && arrow) {         /* d-pad held: sweep */
            ticks++;
            int which = (arrow == KEY_UP || arrow == KEY_DOWN) ? 0 : 1;
            int dir = (arrow == KEY_UP || arrow == KEY_RIGHT) ? +1 : -1;
            int step = ticks > 40 ? 4 : ticks > 15 ? 2 : 1;
            int k = held * 2 + which;
            int v = knob[k] + dir * step;
            knob[k] = v < 0 ? 0 : v > 127 ? 127 : v;
            snd_seq_event_t e;
            snd_seq_ev_clear(&e);
            snd_seq_ev_set_source(&e, port);
            snd_seq_ev_set_subs(&e);
            snd_seq_ev_set_direct(&e);
            snd_seq_ev_set_controller(&e, 0, FIRST_CC + k, knob[k]);
            snd_seq_event_output_direct(seq, &e);
            fprintf(stderr, "\r%s %s  cc%d = %3d   ", names[held], which ? "<>" : "^v", FIRST_CC + k, knob[k]);
            continue;
        }
        if (pf[0].revents & POLLIN) {
            if (read(fp, &ev, sizeof ev) != sizeof ev) break;   /* the gamepad is gone */
            if (ev.type == EV_KEY) {
                int b = bank_of(ev.code);
                if (b >= 0 && (ev.value == 0 || ev.value == 1))
                    send_note(seq, port, 36 + b, ev.value);   /* the button as a note */
                if (b >= 0 && !notes) {
                    if (ev.value == 1) { held = b; arrow = 0; ioctl(fk, EVIOCGRAB, 1); }
                    else if (ev.value == 0 && held == b) { held = -1; arrow = 0; ioctl(fk, EVIOCGRAB, 0); }
                }
            }
        }
        if (pf[1].revents & POLLIN) {
            if (notes) {                                  /* the d-pad as notes 40..43 */
                if (read(fk, &ev, sizeof ev) == sizeof ev && ev.type == EV_KEY && (ev.value == 0 || ev.value == 1)) {
                    int n = ev.code == KEY_UP ? 40 : ev.code == KEY_DOWN ? 41 : ev.code == KEY_LEFT ? 42 : ev.code == KEY_RIGHT ? 43 : 0;
                    if (n) send_note(seq, port, n, ev.value);
                }
                continue;
            }
            if (read(fk, &ev, sizeof ev) == sizeof ev && ev.type == EV_KEY && held >= 0) {
                int which = -1, dir = 0;
                switch (ev.code) {
                    case KEY_UP:    which = 0; dir = +1; break;
                    case KEY_DOWN:  which = 0; dir = -1; break;
                    case KEY_RIGHT: which = 1; dir = +1; break;
                    case KEY_LEFT:  which = 1; dir = -1; break;
                }
                if (which < 0) continue;
                if (ev.value == 0) { if (arrow == ev.code) arrow = 0; continue; }   /* released */
                if (ev.value == 2) continue;                                      /* a repeat we didn't ask for */
                arrow = ev.code; ticks = 0;
                int step = 6;                                                     /* one press */
                int k = held * 2 + which;
                int v = knob[k] + dir * step;
                knob[k] = v < 0 ? 0 : v > 127 ? 127 : v;
                snd_seq_event_t e;
                snd_seq_ev_clear(&e);
                snd_seq_ev_set_source(&e, port);
                snd_seq_ev_set_subs(&e);
                snd_seq_ev_set_direct(&e);
                snd_seq_ev_set_controller(&e, 0, FIRST_CC + k, knob[k]);
                snd_seq_event_output_direct(seq, &e);
                fprintf(stderr, "\r%s %s  cc%d = %3d   ", names[held], which ? "<>" : "^v", FIRST_CC + k, knob[k]);
            }
        }
    }
    ioctl(fk, EVIOCGRAB, 0);
    snd_seq_close(seq);
    return 0;
}
