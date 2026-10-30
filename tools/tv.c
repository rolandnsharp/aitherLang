/* tv: a raster display for aither's pairs.

     AITHER_RATE=1152000 aither visuals.aither | tv --size 160x120
     aither visuals.aither | tv --size 32x24      (62.5 frames a second at 48 kHz)
     ... | tv --term                              in the terminal (over ssh)
     ... | tv --frames out.rgb                    every frame, raw RGB, for ffmpeg

   A frame is w*h samples in raster order: rows top to bottom, each row
   left to right, counted from the first sample. A pair is one pixel's
   complex value, shown by domain colouring: hue from its phase (the
   positive reals red, then yellow, green, cyan, blue, magenta going
   round), brightness from its magnitude, black at 0 and full at 1. A
   patch finds its pixel with pixel(w, h) and pixel_index(w, h), and must
   agree with tv on the size. tv has no memory: persistence and feedback
   are state arrays in the patch.

   A frame is shown whole, never half-drawn, at the sample being heard
   (as scope does): each picture one moment of a patch's field. With
   --scan rows are drawn as they arrive instead, over the frame before,
   as a CRT draws: smooth for a big, slow picture. Pixels are crisp
   squares, or soft with --smooth.
   Stdin is copied to stdout unchanged. It opens full screen (--window
   for a window); f toggles, q or Escape closes it at once. The rate is AITHER_RATE, like the engine's, so the
   frame rate is the rate divided by w*h.
   The readouts, besides the raster (TV.md has them in full):
   --fold F    each row one period of F Hz (108bpm: of a beat), the
               newest at the bottom, scrolling up at the period's own
               rate: a note in tune with F stands in columns, one a
               little off leans, and beats drift down. --fold auto finds
               the pitch and holds the fold on it.
   --average   with --fold: under it, the mean of the last second's
               periods, drawn as a curve: what repeats builds up and
               what doesn't cancels (epoch folding).
   --spectrum F  each row the spectrum from -F to +F Hz, scrolling: both
               sides, as a pair has them, spins turning backwards on the
               left. Brightness is level (70 dB), hue the pitch class
               (C red; every octave the same colour).
   --recur S   a recurrence plot of the last S seconds: bright where the
               signal at one moment (across) is near itself at another
               (up), hue the pair's phase there.
   With these --size is columns x rows (256x192; --recur 256x256).
   --gain scales the pairs before they're coloured.
   See notes/displays-2026-10-16.md. Build: make tools/tv (needs sdl3). */

#include "display.h"

/* The picture: each pixel's complex value as colour. */
static const char *PicFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 uv;\n"
    "uniform highp sampler2D uPic;\n"
    "uniform float uGain;\n"
    "out vec4 o;\n"
    "vec3 hue(float h) {\n"                          /* h in turns: 0 red, 1/3 green, 2/3 blue */
    "  return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "  vec2 z = texture(uPic, vec2(uv.x, 1.0 - uv.y)).rg * uGain;\n"   /* row 0 is the top */
    "  float m = length(z);\n"
    "  vec3 col = vec3(0.0);\n"
    "  if (m > 0.0) col = hue(atan(z.y, z.x) / 6.2831853) * min(m, 1.0);\n"   /* 0, or not a number: black */
    "  o = vec4(col, 1.0);\n"
    "}\n";

/* The scrolling readouts: the same colouring, the rows a ring that
   scrolls. uScroll is where the top of the screen is in it, in rows over
   the ring's height. With uSplit > 0 the bottom uSplit of the screen is
   the average, a curve: each column filled up to its magnitude. */
static const char *FoldFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 uv;\n"
    "uniform highp sampler2D uPic, uAvg;\n"
    "uniform float uScroll, uGain, uSplit;\n"
    "out vec4 o;\n"
    "vec3 hue(float h) {\n"
    "  return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);\n"
    "}\n"
    "vec3 colour(vec2 z) {\n"
    "  float m = length(z);\n"
    "  return m > 0.0 ? hue(atan(z.y, z.x) / 6.2831853) * min(m, 1.0) : vec3(0.0);\n"
    "}\n"
    "void main() {\n"
    "  float down = 1.0 - uv.y;\n"
    "  if (down > 1.0 - uSplit) {\n"
    "    float v = (1.0 - down) / uSplit;\n"                   /* 0 at the bottom */
    "    vec2 z = texture(uAvg, vec2(uv.x, 0.5)).rg * uGain;\n"
    "    float m = length(z);\n"
    "    vec3 c = m > 0.0 ? hue(atan(z.y, z.x) / 6.2831853) : vec3(0.0);\n"
    "    o = vec4(v < min(m, 1.0) ? c : (down < 1.0 - uSplit + 0.004 ? vec3(0.25) : vec3(0.0)), 1.0);\n"
    "    return;\n"
    "  }\n"
    "  down /= 1.0 - uSplit;\n"
    "  o = vec4(colour(texture(uPic, vec2(uv.x, fract(uScroll + down))).rg * uGain), 1.0);\n"
    "}\n";

/* --fold: the row starting at sample `start`, P samples long (P kept
   fractional, so a note doesn't drift from rounding); each column the
   loudest pair of its share, as a waveform overview keeps its peaks (a
   mean would cancel a tone folded at a slow beat). 0 until those
   samples have come. */
static int fold_row(double start, double P, int W, float *span, float *row) {
    uint64_t a = (uint64_t)floor(start), b = (uint64_t)floor(start + P) + 1;
    size_t n = (size_t)(b - a);
    if (stream_range(span, a, b) != n) return 0;
    for (int c = 0; c < W; c++) {
        int i0 = (int)(start - a + c * P / W), i1 = (int)ceil(start - a + (c + 1) * P / W);
        if (i1 > (int)n) i1 = (int)n;
        if (i1 <= i0) i1 = i0 + 1;
        int at = i0;
        float most = -1;
        for (int i = i0; i < i1; i++) {
            float m = span[2 * i] * span[2 * i] + span[2 * i + 1] * span[2 * i + 1];
            if (m > most) { most = m; at = i; }
        }
        row[2 * c] = span[2 * at];
        row[2 * c + 1] = span[2 * at + 1];
    }
    return 1;
}

/* --fold auto, finding the pitch: the period of the YIN method (de
   Cheveigné and Kawahara, JASA 2002) on the pairs before `end`: the
   difference |z[n] - z[n + τ]|² summed, normalised by its running mean,
   and the first dip under 0.2, its depth interpolated. 0 if nothing in
   40 Hz .. 2 kHz repeats. d keeps the normalised differences. */
#define YIN_N 1024
static double yin_period(uint64_t end, double rate, float *buf, double *d) {
    int lo = (int)(rate / 2000), hi = (int)(rate / 40);
    uint64_t n = YIN_N + hi + 1;
    if (end < n || stream_range(buf, end - n, end) != n) return 0;
    double run = 0;
    d[0] = 1;
    for (int t = 1; t <= hi; t++) {
        double s = 0;
        for (int i = 0; i < YIN_N; i++) {
            double x = buf[2 * i] - buf[2 * (i + t)], y = buf[2 * i + 1] - buf[2 * (i + t) + 1];
            s += x * x + y * y;
        }
        run += s;
        d[t] = run > 0 ? s * t / run : 1;
    }
    int best = 0;
    for (int t = lo; t < hi; t++) {
        if (d[t] < 0.2) {
            while (t + 1 < hi && d[t + 1] < d[t]) t++;
            best = t;
            break;
        }
    }
    if (!best) return 0;
    double a = d[best - 1], b = d[best], c = d[best + 1], den = a - 2 * b + c;
    return best + (den > 0 ? 0.5 * (a - c) / den : 0);
}

/* --fold auto, holding it: how many columns the new row's pattern has slid
   from the row before (positive: right, the fold too short), or NAN when
   the two rows don't look alike enough to say. */
static double row_slide(const float *a, const float *b, int W) {
    int S = W / 16;
    double e[2 * 64 + 1], power = 0;
    if (S > 64) S = 64;
    for (int c = 0; c < W; c++) power += a[2 * c] * a[2 * c] + a[2 * c + 1] * a[2 * c + 1] + b[2 * c] * b[2 * c] + b[2 * c + 1] * b[2 * c + 1];
    if (power <= 0) return NAN;
    int best = 0;
    for (int s = -S; s <= S; s++) {
        double sum = 0;
        for (int c = S; c < W - S; c++) {
            double x = b[2 * c] - a[2 * (c - s)], y = b[2 * c + 1] - a[2 * (c - s) + 1];
            sum += x * x + y * y;
        }
        e[s + S] = sum;
        if (sum < e[best + S]) best = s;
    }
    if (e[best + S] > 0.3 * power * (W - 2 * S) / W || best == -S || best == S) return NAN;
    double l = e[best + S - 1], m = e[best + S], r = e[best + S + 1], den = l - 2 * m + r;
    return best + (den > 0 ? 0.5 * (l - r) / den : 0);
}

/* --spectrum: an FFT of the pairs as complex numbers, n a power of 2. */
static void fft(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len, wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                double *ur = re + i + k, *ui = im + i + k, *vr = re + i + k + len / 2, *vi = im + i + k + len / 2;
                double xr = *vr * cr - *vi * ci, xi = *vr * ci + *vi * cr;
                *vr = *ur - xr; *vi = *ui - xi;
                *ur += xr; *ui += xi;
                double t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

/* --spectrum: the row ending at sample `end`: a Hann-windowed FFT of the
   last SPEC_N pairs, column c the bin nearest its frequency, from -F to
   +F. Brightness is the level, 0 at -70 dB and full at 0 dB (a spin of
   amplitude 1). Hue is the pitch class of the frequency the bin truly
   holds, from how far its phase turned since the row before (the phase
   vocoder's instantaneous frequency): C red, and round the wheel by
   semitones, every octave the same colour, all of a tone's bins alike. */
#define SPEC_N 4096
static int spectrum_row(uint64_t end, double rate, double F, int W, float *span, double *re, double *im, float *row) {
    static double pre[SPEC_N], pim[SPEC_N];
    static uint64_t pend;
    if (end < SPEC_N || stream_range(span, end - SPEC_N, end) != SPEC_N) return 0;
    for (int i = 0; i < SPEC_N; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * (i + 0.5) / SPEC_N);
        re[i] = span[2 * i] * w;
        im[i] = span[2 * i + 1] * w;
    }
    fft(re, im, SPEC_N);
    double hop = pend && end > pend ? (double)(end - pend) : 0;
    for (int c = 0; c < W; c++) {
        double f = -F + (c + 0.5) * 2 * F / W;
        long b = lround(f * SPEC_N / rate);
        int k = (int)(((b % SPEC_N) + SPEC_N) % SPEC_N);
        double x = re[k], y = im[k], m = hypot(x, y) / (SPEC_N / 2.0);
        double L = m > 0 ? fmax(0, fmin(1, 1 + 20 * log10(m) / 70)) : 0;
        double held = b * rate / SPEC_N;
        if (hop > 0) {
            double turn = atan2(y * pre[k] - x * pim[k], x * pre[k] + y * pim[k]);
            double dev = turn - 2 * M_PI * b * hop / SPEC_N;
            dev -= 2 * M_PI * floor(dev / (2 * M_PI) + 0.5);
            held += dev * rate / (2 * M_PI * hop);
        }
        double h = fabs(held) > 20 ? 2 * M_PI * 12 * log2(fabs(held) / 16.3516) / 12 : 0;   /* from C */
        row[2 * c] = (float)(L * cos(h));
        row[2 * c + 1] = (float)(L * sin(h));
    }
    memcpy(pre, re, sizeof pre);
    memcpy(pim, im, sizeof pim);
    pend = end;
    return 1;
}

/* The scrolling readouts, --fold and --spectrum: rows, the newest at the
   bottom, scrolling up at the sample being heard (or, for --frames, 30
   pictures a second of the stream's own time). For --fold, P is the
   period in samples, 0 for auto; for --spectrum (F > 0) it is the time
   between rows. */
static int scroll(Display *d, int W, int H, double P, double F, int average, double gain, int smooth, int dest) {
    int spectrum = F > 0, lock = !spectrum && P == 0;
    double rate = stream.rate;
    if (lock) P = rate / 110;                 /* till it hears a pitch */
    if (rate / 40 + 2 > RING / 4 || P > RING / 4) {
        fputs("tv: a fold that long is more than the stream keeps\n", stderr);
        display_close(d);
        return 2;
    }
    if (spectrum) average = 0;
    GLuint prog = gl_program(GL_FULLSCREEN_VS, FoldFS);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uPic"), 0);
    glUniform1i(glGetUniformLocation(prog, "uAvg"), 1);
    GLint uScroll = glGetUniformLocation(prog, "uScroll");
    glUniform1f(glGetUniformLocation(prog, "uGain"), (float)gain);
    glUniform1f(glGetUniformLocation(prog, "uSplit"), average ? 0.3f : 0.0f);
    GLuint empty;
    glGenVertexArrays(1, &empty);
    if (smooth && !gl_has("GL_OES_texture_float_linear")) smooth = 0;
    size_t most = (size_t)(rate / 40) + YIN_N + SPEC_N + 4;   /* the longest span read */
    float *zero = calloc((size_t)W * H * 2, sizeof(float));
    float *rows[2] = { calloc((size_t)W * 2, sizeof(float)), calloc((size_t)W * 2, sizeof(float)) };
    float *avg = calloc((size_t)W * 2, sizeof(float));
    float *span = calloc(most * 2, sizeof(float));
    double *dif = calloc(most, sizeof(double));
    double *re = calloc(SPEC_N, sizeof(double)), *im = calloc(SPEC_N, sizeof(double));
    GLuint tex[2];
    glGenTextures(2, tex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, W, i ? 1 : H, 0, GL_RG, GL_FLOAT, zero);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, i ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }

    int frames = dest == DEST_FRAMES;
    if (frames)
        fprintf(stderr, "tv: %dx%d rgb24 frames to a file, %d a second\n"
                "  ffmpeg -f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i FILE out.mp4\n",
                W, H, HEADLESS_FPS, W, H, HEADLESS_FPS);
    int64_t r = 0;                            /* the next row, counted from the start */
    double start = 0;                         /* and the sample it starts at */
    double heard = 0;                         /* the pitch last heard, in samples, and */
    int again = 0;                            /* how many times running it was heard away from the fold */
    double ref = P;                           /* the period found: the hold keeps near it */
    uint64_t nextLook = 0;
    uint64_t k = 0;                           /* --frames: the picture */
    double prev = now_s();
    for (;;) {
        uint64_t now;
        if (frames) {
            now = (uint64_t)((k + 1) * rate / HEADLESS_FPS);
            while (stream.head < now && !stream.eof && !display_quit) sleep_s(0.002);
            if (stream.head < now || display_quit) break;
            k++;
        } else {
            double t = now_s(), dt = t - prev;
            if (dt > 0.25) dt = 0.25;
            prev = t;
            now = stream_now(dt);
            if (display_poll(d) || (stream.eof && now >= stream.head)) break;
        }

        /* Fell more than a screen behind: skip to the last screenful. */
        if (now > start + (H + 1) * P) {
            double skip = floor((now - start) / P) - H;
            start += skip * P;
            r += (int64_t)skip;
        }
        /* Every row complete by now. */
        glBindTexture(GL_TEXTURE_2D, tex[0]);
        int added = 0;
        while (start + P + 1 <= now) {
            if (lock && start >= nextLook) {
                /* The pitch, ten times a second. The fold holds while the sound
                   still repeats at it (a harmonic of it alone does); when it
                   doesn't, the new pitch is jumped to once heard twice. */
                double p = yin_period((uint64_t)start, rate, span, dif);
                nextLook = (uint64_t)start + (uint64_t)(rate / 10);
                int ti = (int)ref;
                double stays = p > 0 && ti + 1 < (int)(rate / 40) ? dif[ti] + (dif[ti + 1] - dif[ti]) * (ref - ti) : 1;
                if (p > 0 && fabs(p / ref - 1) > 0.03 && stays > 0.35) {
                    again = heard > 0 && fabs(p / heard - 1) < 0.03 ? again + 1 : 1;
                    heard = p;
                    if (again >= 2 || ref == rate / 110) {
                        P = ref = p;
                        again = 0;
                        fprintf(stderr, "tv: folding at %.2f Hz\n", rate / P);
                    }
                } else again = 0;
            }
            float *row = rows[r & 1];
            if (spectrum ? !spectrum_row((uint64_t)(start + P), rate, F, W, span, re, im, row)
                         : !fold_row(start, P, W, span, row)) break;
            if (lock && r > 0) {
                /* The hold: nudge the fold so the pattern stops sliding, and
                   keep within 3% of the pitch found. */
                double s = row_slide(rows[(r - 1) & 1], row, W);
                if (s == s) {
                    P += 0.25 * s * P / W;
                    if (P > ref * 1.03) P = ref * 1.03;
                    if (P < ref / 1.03) P = ref / 1.03;
                }
            }
            if (average) {
                /* The last second's periods, averaged as they come. */
                float a = (float)(1 - exp(-P / rate));
                for (int c = 0; c < 2 * W; c++) avg[c] += (row[c] - avg[c]) * a;
                added = 1;
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (int)(r % H), W, 1, GL_RG, GL_FLOAT, row);
            start += P;
            r++;
        }
        if (added) {
            glBindTexture(GL_TEXTURE_2D, tex[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, 1, GL_RG, GL_FLOAT, avg);
        }

        /* The bottom edge is part way through the newest complete row, a
           row behind the sample being heard, so the scroll is smooth. */
        double pos = r + (now - start) / P;
        double top = fmod(pos - 1 - H, H) / H;
        if (top < 0) top += 1;
        if (frames) display_target(d, W, H);
        else {
            int w, h;
            display_size(d, &w, &h);
            double s = fmin((double)w / W, (double)h / H);
            int iw = (int)(W * s), ih = (int)(H * s);
            display_target(d, iw < 1 ? 1 : iw, ih < 1 ? 1 : ih);
        }
        glUseProgram(prog);
        glUniform1f(uScroll, (float)top);
        glBindVertexArray(empty);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex[1]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex[0]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        display_present(d);
    }
    display_close(d);
    return 0;
}

/* --recur: a recurrence plot (Eckmann, Kamphorst and Ruelle, Europhysics
   Letters 1987) of the last S seconds, W moments of it, redrawn every
   picture. A moment's state is its pair and the pair a moment before, so
   a signal passing the same value going the other way isn't counted as
   back. Pixel (i, j) is bright as moment i's state is near moment j's,
   against the window's own size, and its hue is the pair's phase there:
   where round its cycle the signal was when it came back. Time runs across and up, so the
   diagonal is each moment with itself: a period draws lines parallel to
   it, chaos short broken ones, noise scattered dots. */
static int recur(Display *d, int W, double S, double gain, int smooth, int dest) {
    double rate = stream.rate, step = S * rate / W;
    uint64_t span = (uint64_t)ceil(step * (W + 1)) + 2;
    if (span > RING / 4) {
        fputs("tv: a recurrence that long is more than the stream keeps\n", stderr);
        display_close(d);
        return 2;
    }
    GLuint prog = gl_program(GL_FULLSCREEN_VS, PicFS);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uPic"), 0);
    glUniform1f(glGetUniformLocation(prog, "uGain"), (float)gain);
    GLuint empty;
    glGenVertexArrays(1, &empty);
    if (smooth && !gl_has("GL_OES_texture_float_linear")) smooth = 0;
    float *buf = calloc(span * 2, sizeof(float));
    float *pic = calloc((size_t)W * W * 2, sizeof(float));
    float *z = calloc((size_t)(W + 1) * 2, sizeof(float));
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, W, W, 0, GL_RG, GL_FLOAT, pic);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    int frames = dest == DEST_FRAMES;
    if (frames)
        fprintf(stderr, "tv: %dx%d rgb24 frames to a file, %d a second\n"
                "  ffmpeg -f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i FILE out.mp4\n",
                W, W, HEADLESS_FPS, W, W, HEADLESS_FPS);
    uint64_t k = 0, last = 0;
    double prev = now_s();
    for (;;) {
        uint64_t now;
        if (frames) {
            now = (uint64_t)((k + 1) * rate / HEADLESS_FPS);
            while (stream.head < now && !stream.eof && !display_quit) sleep_s(0.002);
            if (stream.head < now || display_quit) break;
            k++;
        } else {
            double t = now_s(), dt = t - prev;
            if (dt > 0.25) dt = 0.25;
            prev = t;
            now = stream_now(dt);
            if (display_poll(d) || (stream.eof && now >= stream.head)) break;
        }
        if (now >= span && now != last && stream_range(buf, now - span, now) == span) {
            last = now;
            /* The W + 1 moments, the last at now; and the window's size. */
            double size = 0;
            for (int i = 0; i <= W; i++) {
                int at = (int)(span - 1 - (W - i) * step);
                z[2 * i] = buf[2 * at];
                z[2 * i + 1] = buf[2 * at + 1];
                size += z[2 * i] * z[2 * i] + z[2 * i + 1] * z[2 * i + 1];
            }
            double eps = 0.1 * 2 * (size / (W + 1)) + 1e-12;    /* near: within a third of its spread */
            for (int j = 0; j < W; j++) {
                const float *b = z + 2 * (j + 1);
                float *out = pic + (size_t)(W - 1 - j) * W * 2;   /* up: later */
                for (int i = 0; i < W; i++) {
                    const float *a = z + 2 * (i + 1);
                    double dx = a[0] - b[0], dy = a[1] - b[1], px = a[-2] - b[-2], py = a[-1] - b[-1];
                    double near = exp(-(dx * dx + dy * dy + px * px + py * py) / eps);
                    double tx = a[0] + b[0], ty = a[1] + b[1];
                    double m = hypot(tx, ty);
                    out[2 * i] = m > 0 ? (float)(near * tx / m) : (float)near;
                    out[2 * i + 1] = m > 0 ? (float)(near * ty / m) : 0;
                }
            }
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, W, GL_RG, GL_FLOAT, pic);
        }
        if (frames) display_target(d, W, W);
        else {
            int w, h;
            display_size(d, &w, &h);
            int s = w < h ? w : h;
            display_target(d, s < 1 ? 1 : s, s < 1 ? 1 : s);
        }
        glUseProgram(prog);
        glBindVertexArray(empty);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        display_present(d);
    }
    display_close(d);
    return 0;
}

static void usage(void) {
    fputs("usage: aither visuals.aither | tv [--size 64x48] [--smooth] [--scan] [--gain G] [--window | --term | --frames FILE]\n"
          "       ... | tv [--fold HZ|BPMbpm|auto [--average] | --spectrum HZ | --recur SECONDS] ...\n"
          "  float pairs in on stdin, the same out on stdout; full screen unless --window;\n"
          "  --scan draws rows as they arrive, not whole frames (not for --frames);\n"
          "  --fold folds at a period, a row each, scrolling up; --average adds their mean;\n"
          "  --spectrum scrolls the spectrum, -HZ to +HZ; --recur is a recurrence plot;\n"
          "  f toggles full screen, q or Escape quits\n",
          stderr);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    int W = 64, H = 48, dest = -1, smooth = 0, windowed = 0, scan = 0, sized = 0;
    double foldHz = 0, gain = 1, specHz = 0, recurS = 0;
    int average = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &W, &H) != 2) { usage(); return 2; }
            sized = 1;
        } else if (!strcmp(argv[i], "--fold") && i + 1 < argc) {
            char *end;
            if (!strcmp(argv[i + 1], "auto")) { foldHz = -1; i++; continue; }
            foldHz = strtod(argv[++i], &end);
            if (!strcmp(end, "bpm")) foldHz /= 60;
            else if (*end) { usage(); return 2; }
            if (!(foldHz > 0)) { usage(); return 2; }
        } else if (!strcmp(argv[i], "--average")) average = 1;
        else if (!strcmp(argv[i], "--spectrum") && i + 1 < argc) {
            specHz = atof(argv[++i]);
            if (!(specHz > 0)) { usage(); return 2; }
        } else if (!strcmp(argv[i], "--recur") && i + 1 < argc) {
            recurS = atof(argv[++i]);
            if (!(recurS > 0)) { usage(); return 2; }
        } else if (!strcmp(argv[i], "--gain") && i + 1 < argc) {
            gain = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--smooth")) smooth = 1;
        else if (!strcmp(argv[i], "--window")) windowed = 1;
        else if (!strcmp(argv[i], "--scan")) scan = 1;
        else if (!strcmp(argv[i], "--term")) dest = DEST_TERM;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { dest = DEST_FRAMES; path = argv[++i]; }
        else { usage(); return 2; }
    }
    if ((foldHz != 0) + (specHz > 0) + (recurS > 0) > 1) {
        fputs("tv: one readout at a time: --fold, --spectrum or --recur\n", stderr);
        return 2;
    }
    if ((foldHz != 0 || specHz > 0) && !sized) { W = 256; H = 192; }
    if (recurS > 0) { if (!sized) W = 256; H = W; }
    if (W < 1 || H < 1 || (long)W * H > RING / 4) {
        fprintf(stderr, "tv: a picture has from 1 to %d pixels (512x512)\n", RING / 4);
        return 2;
    }
    if (isatty(0)) {
        fputs("tv: stdin is a terminal. Pipe pairs into me:\n"
              "  aither visuals.aither | tv --size 64x48\n", stderr);
        return 2;
    }
    if (dest < 0) dest = (getenv("WAYLAND_DISPLAY") || getenv("DISPLAY")) ? DEST_WINDOW : DEST_TERM;

    stream_start();
    Display d = {0};
    d.halfblock = 1;                          /* a raster wants colours, not dots */
    d.nopace = 1;                             /* frames: every one, as it comes */
    d.windowed = windowed;
    if (display_open(&d, "tv", dest, path, 0) < 0) return 1;
    uint64_t N = (uint64_t)W * (uint64_t)H;
    if (foldHz != 0) return scroll(&d, W, H, foldHz > 0 ? stream.rate / foldHz : 0, 0, average, gain, smooth, dest);
    if (specHz > 0) return scroll(&d, W, H, stream.rate / 100, specHz, 0, gain, smooth, dest);   /* 100 rows a second */
    if (recurS > 0) return recur(&d, W, recurS, gain, smooth, dest);
    if (dest == DEST_FRAMES)
        fprintf(stderr, "tv: %dx%d rgb24 frames to %s, %.6g a second at AITHER_RATE %g\n"
                "  ffmpeg -f rawvideo -pix_fmt rgb24 -s %dx%d -r %.6g -i %s out.mp4\n",
                W, H, path, stream.rate / (double)N, stream.rate, W, H, stream.rate / (double)N, path);

    GLuint prog = gl_program(GL_FULLSCREEN_VS, PicFS);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uPic"), 0);
    glUniform1f(glGetUniformLocation(prog, "uGain"), (float)gain);
    GLuint empty;
    glGenVertexArrays(1, &empty);

    /* The picture: two floats a pixel, the pairs exactly as they came. */
    float *buf = calloc(N * 2, sizeof(float));
    if (smooth && !gl_has("GL_OES_texture_float_linear")) {
        fputs("tv: no smooth scaling of float pictures on this GPU; crisp it is\n", stderr);
        smooth = 0;
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, W, H, 0, GL_RG, GL_FLOAT, buf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (dest == DEST_FRAMES) {
        /* Every frame in order, at the picture's own size: a recording is
           the stream's frames and nothing else. */
        uint64_t next = 0;
        for (;;) {
            uint64_t head = stream.head;
            if ((next + 1) * N <= head) {
                if (head > RING && next * N < head - RING) next = (head - RING + N - 1) / N;  /* fell behind */
                if (stream_range(buf, next * N, (next + 1) * N) == N) {
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RG, GL_FLOAT, buf);
                    display_target(&d, W, H);
                    glUseProgram(prog);
                    glBindVertexArray(empty);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    display_present(&d);
                }
                next++;
                continue;
            }
            if (stream.eof || display_quit) break;
            sleep_s(0.002);
        }
        display_close(&d);
        return 0;
    }

    double prev = now_s();
    int64_t shown = -1;                       /* the last frame on the screen */
    int64_t shownRow = -1;                    /* --scan: the last row on it, counted from the start */
    for (;;) {
        double t = now_s(), dt = t - prev;
        if (dt > 0.25) dt = 0.25;
        prev = t;
        uint64_t now = stream_now(dt);
        if (display_poll(&d) || (stream.eof && now >= stream.head)) break;

        /* --scan: every row complete at the sample being heard, each in
           its place over the frame before (at most a frame's worth). */
        if (scan && now >= (uint64_t)W) {
            int64_t r = (int64_t)(now / W) - 1;
            int64_t a = shownRow + 1;
            if (a < r - H + 1) a = r - H + 1;
            glBindTexture(GL_TEXTURE_2D, tex);
            while (a <= r) {
                int y = (int)(a % H);
                int64_t n = r - a + 1;
                if (n > H - y) n = H - y;
                if (stream_range(buf, (uint64_t)a * W, (uint64_t)(a + n) * W) == (uint64_t)(n * W))
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, W, (int)n, GL_RG, GL_FLOAT, buf);
                a += n;
            }
            shownRow = r;
        }

        /* The newest frame that is complete at the sample being heard. */
        if (!scan && now >= N) {
            int64_t f = (int64_t)(now / N) - 1;
            if (f > shown) {
                if (stream_range(buf, (uint64_t)f * N, (uint64_t)(f + 1) * N) == N) {
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RG, GL_FLOAT, buf);
                }
                shown = f;
            }
        }

        /* As big as fits, the picture's shape kept. */
        int w, h;
        display_size(&d, &w, &h);
        double k = fmin((double)w / W, (double)h / H);
        int iw = (int)(W * k), ih = (int)(H * k);
        if (iw < 1) iw = 1;
        if (ih < 1) ih = 1;
        display_target(&d, iw, ih);
        glUseProgram(prog);
        glBindVertexArray(empty);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        display_present(&d);
    }
    display_close(&d);
    return 0;
}
