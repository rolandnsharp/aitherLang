/* play: the speaker on the end of aither's pipe.
     aither set.aither | tools/play [device] [latency-ms]
     aither set.aither | tools/play plughw:rockchipes8388c      (the uConsole)
     AITHER_RATE=44100 aither x.aither | tools/play hw:rockchipes8388c
   Reads raw interleaved float32 pairs from stdin and writes them to an
   ALSA device as 16-bit. Address the card by name (`cat
   /proc/asound/cards`): card numbers can change between boots. A
   `plughw:` device resamples when the card can't do the engine's rate
   (the uConsole's es8388 only does 44.1 kHz); `hw:` is exact and needs
   AITHER_RATE to match. Latency defaults to 50 ms (the plug layer
   can't go much lower); 20 is fine on hw:. Anything past ±1 is clamped
   here, hard: the engine doesn't limit the mix, so a patch that wants
   a soft limiter ends with `|> tanh`. The card is asked for exact
   periods of PERIOD frames, a power of two (the engine's block), and a
   buffer of the latency rounded up to whole periods: on the uConsole's
   es8388, periods ALSA picked from a latency alone (441 frames for 40 ms)
   clicked with no underrun, as tools/card's exact 512 never did. If the
   card ran dry while playing, play says how often when it exits, on
   Ctrl-C as well. Exits when the pipe closes. Like tv and scope it copies its input to stdout unchanged
   (when stdout isn't a terminal), after the card has it, so it can sit
   anywhere in a chain: music.aither | play | picture.aither | tv. If
   the reader goes away, play stops copying and keeps playing.
   It runs on the audio core at real-time priority when allowed
   (realtime.c). Build: make tools/play */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <alsa/asoundlib.h>

#define FRAMES 256   /* one write: 5.3 ms at 48 kHz */
#define PERIOD 512   /* frames: the card's period, the engine's default block */

static volatile sig_atomic_t stop;               /* Ctrl-C: stop reading, then report */
static void on_int(int sig) { (void)sig; stop = 1; }

int aither_realtime(const char *who);   /* realtime.c: audio core, SCHED_FIFO */

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "default";
    unsigned latency_ms = argc > 2 ? (unsigned)atoi(argv[2]) : 50;
    const char *rate_env = getenv("AITHER_RATE");
    unsigned rate = rate_env ? (unsigned)atoi(rate_env) : 48000;
    if (rate == 0) rate = 48000;
    if (latency_ms == 0) latency_ms = 50;

    snd_pcm_t *pcm;
    int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) { fprintf(stderr, "play: open %s: %s\n", dev, snd_strerror(err)); return 1; }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    unsigned r = rate;
    snd_pcm_uframes_t period = PERIOD;
    snd_pcm_uframes_t periods = ((snd_pcm_uframes_t)latency_ms * rate / 1000 + PERIOD - 1) / PERIOD;
    if (periods < 2) periods = 2;
    snd_pcm_uframes_t buffer = period * periods;
    const char *step = "access";
    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) >= 0 &&
        (step = "format", err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) >= 0 &&
        (step = "channels", err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) >= 0 &&
        (step = "rate", err = snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0)) >= 0 &&
        (step = "period", err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0)) >= 0 &&
        (step = "buffer", err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) >= 0)
        step = "hw_params", err = snd_pcm_hw_params(pcm, hw);
    if (err < 0) { fprintf(stderr, "play: %s on %s: %s\n", step, dev, snd_strerror(err)); return 1; }
    if (r != rate) fprintf(stderr, "play: %s plays %u Hz, not %u: use plughw: or set AITHER_RATE\n", dev, r, rate);
    struct sigaction sa = {0};
    sa.sa_handler = on_int;                        /* no SA_RESTART: a read in progress returns */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    aither_realtime("play");

    int pass = !isatty(1);                         /* copy the stream on, as tv and scope do */
    signal(SIGPIPE, SIG_IGN);
    static float in[FRAMES * 2];
    static short out[FRAMES * 2];
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long underruns = 0;
    double first = 0, last = 0;                    /* when the card ran dry, s from the start */
    while (!stop) {
        size_t want = sizeof in, got = 0;
        while (got < want && !stop) {              /* a pipe delivers in pieces */
            ssize_t n = read(0, (char *)in + got, want - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (size_t)n;
        }
        long frames = (long)(got / (2 * sizeof(float)));
        if (frames == 0) break;                    /* pipe closed */
        for (long i = 0; i < frames * 2; i++) {
            float v = in[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            out[i] = (short)(v * 32767.0f);
        }
        for (long done = 0; done < frames; ) {
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, out + 2 * done, frames - done);
            if (w == -EPIPE) {                     /* underrun: count it, carry on */
                clock_gettime(CLOCK_MONOTONIC, &t);
                last = (double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
                if (!underruns++) first = last;
                snd_pcm_prepare(pcm);
                continue;
            }
            if (w == -EINTR) { if (stop) goto end; continue; }
            if (w < 0) { fprintf(stderr, "play: write: %s\n", snd_strerror((int)w)); goto end; }
            done += w;
        }
        /* Then on down the chain, exactly as it came; a reader that has
           gone ends the copying, not the playing. */
        for (size_t o = 0; pass && o < got; ) {
            ssize_t n = write(1, (char *)in + o, got - o);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { pass = 0; break; }
            o += (size_t)n;
        }
    }
end:
    if (stop) snd_pcm_drop(pcm); else snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    if (underruns)
        fprintf(stderr, "play: %ld underrun%s (the card ran dry), first at %.2f s, last at %.2f s\n",
                underruns, underruns == 1 ? "" : "s", first, last);
    return 0;
}
