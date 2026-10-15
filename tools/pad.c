/* pad: the uConsole's gamepad as eight MIDI knobs.
     tools/pad &                 then: aither midi connect <the aither-pad port>
   Hold A, B, X or Y and press the d-pad: up/down moves that button's
   first knob, left/right its second. Knobs are 0..127 and keep their
   value when you let go. A press steps by 6 (about 20 presses across
   the range); holding the d-pad sweeps, faster the longer it is held
   (our own repeat: this keyboard sends none). Sent as MIDI CC 20..27 on a virtual ALSA
   sequencer port called "aither-pad"; a patch reads them with
   midi_cc(20) etc.
       A: cc20 up/down  cc21 left/right      B: cc22 cc23
       X: cc24 cc25                          Y: cc26 cc27
   The d-pad is the keyboard's arrow keys, so while a button is held the
   keyboard is grabbed (arrows don't reach the terminal); release and it
   is yours again. Buttons come from the gamepad device.
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
    switch (code) { case 288: return 0; case 290: return 1; case 289: return 2; case 291: return 3; }
    return -1;
}

int main(int argc, char **argv) {
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
        int r = poll(pf, 2, (held >= 0 && arrow) ? 45 : -1);
        if (r < 0) break;
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
            if (read(fp, &ev, sizeof ev) == sizeof ev && ev.type == EV_KEY) {
                int b = bank_of(ev.code);
                if (b >= 0) {
                    if (ev.value == 1) { held = b; arrow = 0; ioctl(fk, EVIOCGRAB, 1); }
                    else if (ev.value == 0 && held == b) { held = -1; arrow = 0; ioctl(fk, EVIOCGRAB, 0); }
                }
            }
        }
        if (pf[1].revents & POLLIN) {
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
