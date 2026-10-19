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
   a soft limiter ends with `|> tanh`. If the card ran dry while
   playing, play says how often when it exits. Exits when the pipe
   closes.
   It runs on the audio core at real-time priority when allowed
   (realtime.c). Build: make tools/play */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <alsa/asoundlib.h>

#define FRAMES 256   /* one write: 5.3 ms at 48 kHz */

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
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                             2, rate, 1, latency_ms * 1000);
    if (err < 0) { fprintf(stderr, "play: set_params %s: %s\n", dev, snd_strerror(err)); return 1; }
    aither_realtime("play");

    static float in[FRAMES * 2];
    static short out[FRAMES * 2];
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long underruns = 0;
    double first = 0, last = 0;                    /* when the card ran dry, s from the start */
    for (;;) {
        size_t want = sizeof in, got = 0;
        while (got < want) {                       /* a pipe delivers in pieces */
            ssize_t n = read(0, (char *)in + got, want - got);
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
            if (w < 0) { fprintf(stderr, "play: write: %s\n", snd_strerror((int)w)); goto end; }
            done += w;
        }
    }
end:
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    if (underruns)
        fprintf(stderr, "play: %ld underrun%s (the card ran dry), first at %.2f s, last at %.2f s\n",
                underruns, underruns == 1 ? "" : "s", first, last);
    return 0;
}
