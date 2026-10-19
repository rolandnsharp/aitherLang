/* card: one clock for output and input. The sound card's capture and
   playback, with a command in between.
     tools/card [device] [periods] -- aither lockin.aither
     tools/card hw:rockchipes8388c -- sh -c 'aither fx.aither | tools/scope'
   Opens capture and playback on one ALSA device, links them so they
   start on the same frame, and runs the command with the card on both
   ends of it: each period, what the ADC heard goes to the command's
   stdin and one block read back from its stdout goes to the DAC.
   aither is lockstep (one block out per block in), so the pipes never
   hold more than a block and the loop delay is fixed: output computed
   from input frame n plays at frame n + periods × 512, plus whatever
   the converters add. Measure that once per card with a cable from
   out to in and write it in the patch, in seconds.

   The card sets the pace and the rate. card asks for AITHER_RATE
   (48000 if unset), takes the nearest rate the card can do (44100 on
   the uConsole's es8388), and starts the command with AITHER_RATE set
   to it and AITHER_BUFFER set to the period. A period that isn't the
   engine's block deadlocks the two, so the command can't choose its
   own. `periods` (default 2) is the silence playback starts with: the
   time the command has to answer, one period less than that.

   Before the card starts, card sends one block of silence and waits
   for the answer, so a patch that is still compiling can't overrun the
   card. The engine's `t` is therefore one block (512 frames) ahead of
   the card's frame count, always. On an xrun card estimates the lost
   frames from the clock, to whole periods, and feeds the command that
   many periods of silence (their output is dropped), so `t` keeps
   counting the card's frames. ALSA doesn't report the count, so the
   estimate can be a period out. Then it restarts with the same delay.

   What card plays (the command's output, unclamped) is also written
   to its own stdout when that is a pipe, never blocking: a slow reader
   loses blocks, the sound doesn't. The DAC gets it clamped to ±1.
   On stderr: the wall-clock time of frame 0, and the xruns at exit.
   Exits when the command does, with its status.
   Build: cc -O2 tools/card.c -o tools/card -lasound */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <alsa/asoundlib.h>

#define PERIOD 512   /* frames: the engine's default block */

static unsigned rate;
static snd_pcm_format_t fmt[2];   /* [capture, playback] */

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

/* Exact period, a buffer of `periods` periods, 32-bit if the card has
   it (an ADC's low bits are what sensing is about), else 16. Starts
   only when told: snd_pcm_start on the linked pair. */
static int setup(snd_pcm_t *pcm, int which, unsigned periods) {
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    fmt[which] = SND_PCM_FORMAT_S32_LE;
    if (snd_pcm_hw_params_set_format(pcm, hw, fmt[which]) < 0) {
        fmt[which] = SND_PCM_FORMAT_S16_LE;
        if (snd_pcm_hw_params_set_format(pcm, hw, fmt[which]) < 0) return -1;
    }
    if (snd_pcm_hw_params_set_channels(pcm, hw, 2) < 0) return -2;
    unsigned r = rate;
    if (snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0) < 0) return -3;
    if (which == 0) rate = r;
    else if (r != rate) return -3;        /* capture and playback must agree */
    snd_pcm_uframes_t p = PERIOD;
    if (snd_pcm_hw_params_set_period_size(pcm, hw, p, 0) < 0) return -4;
    snd_pcm_uframes_t b = (snd_pcm_uframes_t)PERIOD * periods;
    if (snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &b) < 0) return -5;
    if (snd_pcm_hw_params(pcm, hw) < 0) return -6;
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, (snd_pcm_uframes_t)-1 / 2);
    snd_pcm_sw_params_set_avail_min(pcm, sw, PERIOD);
    return snd_pcm_sw_params(pcm, sw) < 0 ? -7 : 0;
}

static int full_read(int fd, void *buf, size_t n) {
    for (size_t got = 0; got < n; ) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int full_write(int fd, const void *buf, size_t n) {
    for (size_t put = 0; put < n; ) {
        ssize_t w = write(fd, (const char *)buf + put, n - put);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}

static void to_float(const void *in, snd_pcm_format_t f, float *out) {
    for (int i = 0; i < PERIOD * 2; i++)
        out[i] = f == SND_PCM_FORMAT_S32_LE ? ((const int32_t *)in)[i] / 2147483648.0f
                                            : ((const int16_t *)in)[i] / 32768.0f;
}

static void from_float(const float *in, snd_pcm_format_t f, void *out) {
    for (int i = 0; i < PERIOD * 2; i++) {
        float v = in[i];
        if (!(v > -1.0f)) v = -1.0f;      /* NaN goes to -1, not to UB */
        if (v > 1.0f) v = 1.0f;
        if (f == SND_PCM_FORMAT_S32_LE) ((int32_t *)out)[i] = (int32_t)(v * 2147483647.0);
        else ((int16_t *)out)[i] = (int16_t)(v * 32767.0f);
    }
}

int aither_realtime(const char *who);   /* realtime.c: audio core, SCHED_FIFO */

int main(int argc, char **argv) {
    int dash = 1;
    while (dash < argc && strcmp(argv[dash], "--") != 0) dash++;
    if (dash >= argc - 1) {
        fprintf(stderr, "usage: tools/card [device] [periods] -- command ...\n"
                        "  tools/card hw:rockchipes8388c -- aither lockin.aither\n");
        return 2;
    }
    const char *dev = dash > 1 ? argv[1] : "hw:rockchipes8388c";
    unsigned periods = dash > 2 ? (unsigned)atoi(argv[2]) : 2;
    if (periods < 1) periods = 2;
    const char *rate_env = getenv("AITHER_RATE");
    rate = rate_env ? (unsigned)atoi(rate_env) : 48000;
    if (rate == 0) rate = 48000;

    snd_pcm_t *cap, *pb;
    int err;
    if ((err = snd_pcm_open(&cap, dev, SND_PCM_STREAM_CAPTURE, 0)) < 0 ||
        (err = snd_pcm_open(&pb, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "card: open %s: %s\n", dev, snd_strerror(err));
        return 1;
    }
    /* Room for the priming silence and a period either side of it. */
    unsigned bufp = periods + 2 < 4 ? 4 : periods + 2;
    if ((err = setup(cap, 0, bufp)) < 0 || (err = setup(pb, 1, bufp)) < 0) {
        static const char *why[] = {"", "no 16- or 32-bit format", "no stereo",
                                    "rate (capture and playback must share one)",
                                    "a period of 512 frames", "buffer size",
                                    "hw params", "sw params"};
        fprintf(stderr, "card: %s: can't set %s\n", dev, why[-err]);
        return 1;
    }
    if ((err = snd_pcm_link(cap, pb)) < 0) {
        fprintf(stderr, "card: %s: can't link capture to playback: %s\n", dev, snd_strerror(err));
        return 1;
    }

    /* The command, with the card's rate and period. */
    int to_child[2], from_child[2];
    if (pipe(to_child) || pipe(from_child)) { perror("card: pipe"); return 1; }
    char rs[16], ps[16];
    snprintf(rs, sizeof rs, "%u", rate);
    snprintf(ps, sizeof ps, "%d", PERIOD);
    pid_t pid = fork();
    if (pid < 0) { perror("card: fork"); return 1; }
    if (pid == 0) {
        dup2(to_child[0], 0);
        dup2(from_child[1], 1);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        setenv("AITHER_RATE", rs, 1);
        setenv("AITHER_BUFFER", ps, 1);
        execvp(argv[dash + 1], argv + dash + 1);
        fprintf(stderr, "card: %s: %s\n", argv[dash + 1], strerror(errno));
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    aither_realtime("card");            /* after the fork: the command chooses its own */
    int cin = to_child[1], cout = from_child[0];
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);            /* Ctrl-C ends the command; card follows */

    /* Our stdout gets what is played, if it's a pipe; never blocks. */
    int pass = !isatty(1);
    if (pass) fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK);

    static float in[PERIOD * 2], out[PERIOD * 2], silence[PERIOD * 2];
    static int32_t raw[PERIOD * 2];   /* big enough for either format */
    const size_t block = sizeof in;

    long xruns = 0;
    int started = 0;

    /* Handshake: the command is ready when it answers a block. */
    if (full_write(cin, silence, block) || full_read(cout, out, block)) goto done;

restart:
    snd_pcm_drop(cap);                  /* linked: stops both */
    snd_pcm_prepare(cap);               /* and prepares both */
    from_float(silence, fmt[1], raw);
    for (unsigned i = 0; i < periods; i++)
        if (snd_pcm_writei(pb, raw, PERIOD) != PERIOD) {
            fprintf(stderr, "card: can't prime playback\n");
            goto done;
        }
    if ((err = snd_pcm_start(cap)) < 0) {
        fprintf(stderr, "card: start: %s\n", snd_strerror(err));
        goto done;
    }
    double t_last = now_s();            /* when the last period arrived */
    if (!started) {
        started = 1;
        struct timespec wall;
        clock_gettime(CLOCK_REALTIME, &wall);
        struct tm tm;
        gmtime_r(&wall.tv_sec, &tm);
        char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
        fprintf(stderr, "card: %s at %u Hz, period %d, loop %u periods (%.1f ms + converters); "
                        "frame 0 at %s.%03ldZ\n", dev, rate, PERIOD, periods,
                periods * PERIOD * 1000.0 / rate, ts, wall.tv_nsec / 1000000);
    }
    for (;;) {
        snd_pcm_sframes_t n = snd_pcm_readi(cap, raw, PERIOD);
        if (n == -EPIPE) goto xrun;
        if (n < 0) { fprintf(stderr, "card: read: %s\n", snd_strerror((int)n)); goto done; }
        if (n != PERIOD) continue;      /* can't happen with avail_min a period */
        t_last = now_s();
        to_float(raw, fmt[0], in);
        if (full_write(cin, in, block) || full_read(cout, out, block)) goto done;
        from_float(out, fmt[1], raw);
        n = snd_pcm_writei(pb, raw, PERIOD);
        if (n == -EPIPE) goto xrun;
        if (n < 0) { fprintf(stderr, "card: write: %s\n", snd_strerror((int)n)); goto done; }
        if (pass) {
            /* Pipe writes up to 4096 bytes are all or nothing, and 4096
               is whole pairs, so a dropped chunk never splits a pair. */
            for (size_t off = 0; off < block; off += 4096) {
                size_t len = block - off < 4096 ? block - off : 4096;
                ssize_t w = write(1, (char *)out + off, len);
                if (w < 0 && errno == EPIPE) { pass = 0; break; }
                if (w < 0) break;       /* full: drop the rest of this block */
            }
        }
    }
xrun:
    /* Every period that went by unread, up to the restart, is silence
       to the command and its answer is dropped; then the same delay. */
    xruns++;
    snd_pcm_drop(cap);
    while ((long)((now_s() - t_last) * rate / PERIOD + 0.5) > 0) {
        if (full_write(cin, silence, block) || full_read(cout, out, block)) goto done;
        t_last += (double)PERIOD / rate;
    }
    goto restart;
done:
    close(cin);
    close(cout);
    snd_pcm_drop(cap);
    snd_pcm_close(pb);
    snd_pcm_close(cap);
    int status = 0;
    waitpid(pid, &status, 0);
    if (xruns)
        fprintf(stderr, "card: %ld xrun%s (the command or the system was late); "
                        "each restarted with the same loop delay\n",
                xruns, xruns == 1 ? "" : "s");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
