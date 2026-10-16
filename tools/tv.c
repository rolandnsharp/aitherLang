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
   (as scope does); pixels are crisp squares, or soft with --smooth.
   Stdin is copied to stdout unchanged. q or Escape turns the tube off,
   f is full screen. The rate is AITHER_RATE, like the engine's, so the
   frame rate is the rate divided by w*h.
   See notes/displays-2026-10-16.md. Build: make tools/tv (needs sdl3). */

#include "display.h"

/* The picture: each pixel's complex value as colour, and the tube turning
   on and off as scope's does (squeezed toward the centre, brighter, with
   the raster's glow filling what is left). */
static const char *PicFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 uv;\n"
    "uniform highp sampler2D uPic;\n"
    "uniform vec2 uCrt;\n"
    "uniform float uFlash;\n"
    "uniform vec2 uPx;\n"
    "out vec4 o;\n"
    "vec3 hue(float h) {\n"                          /* h in turns: 0 red, 1/3 green, 2/3 blue */
    "  return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "  vec2 c = (uv - 0.5) / uCrt + 0.5;\n"
    "  vec3 col = vec3(0.0);\n"
    "  if (all(greaterThanEqual(c, vec2(0.0))) && all(lessThanEqual(c, vec2(1.0)))) {\n"
    "    vec2 z = texture(uPic, vec2(c.x, 1.0 - c.y)).rg;\n"   /* row 0 is the top */
    "    float m = length(z);\n"
    "    if (m > 0.0)\n"                                       /* 0, or not a number: black */
    "      col = hue(atan(z.y, z.x) / 6.2831853) * min(m, 1.0) / max(uCrt.x * uCrt.y, 0.25);\n"
    "  }\n"
    "  vec2 e = max(abs(uv - 0.5) * uPx - max(uCrt * uPx * 0.5, vec2(1.2)), 0.0) / 1.5;\n"
    "  col += vec3(uFlash * exp(-dot(e, e)));\n"
    "  o = vec4(min(col, vec3(1.0)), 1.0);\n"
    "}\n";

static void usage(void) {
    fputs("usage: aither visuals.aither | tv [--size 64x48] [--smooth] [--term | --frames FILE]\n"
          "  float pairs in on stdin, the same out on stdout; q or Escape quits, f full screen\n",
          stderr);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    int W = 64, H = 48, dest = -1, smooth = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &W, &H) != 2) { usage(); return 2; }
        } else if (!strcmp(argv[i], "--smooth")) smooth = 1;
        else if (!strcmp(argv[i], "--term")) dest = DEST_TERM;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { dest = DEST_FRAMES; path = argv[++i]; }
        else { usage(); return 2; }
    }
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
    if (display_open(&d, "tv", dest, path, 0) < 0) return 1;
    uint64_t N = (uint64_t)W * (uint64_t)H;
    if (dest == DEST_FRAMES)
        fprintf(stderr, "tv: %dx%d rgb24 frames to %s, %.6g a second at AITHER_RATE %g\n"
                "  ffmpeg -f rawvideo -pix_fmt rgb24 -s %dx%d -r %.6g -i %s out.mp4\n",
                W, H, path, stream.rate / (double)N, stream.rate, W, H, stream.rate / (double)N, path);

    GLuint prog = gl_program(GL_FULLSCREEN_VS, PicFS);
    GLint uCrt = glGetUniformLocation(prog, "uCrt"), uFlash = glGetUniformLocation(prog, "uFlash"),
          uPx = glGetUniformLocation(prog, "uPx");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uPic"), 0);
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
                    glUniform2f(uCrt, 1, 1);
                    glUniform1f(uFlash, 0);
                    glUniform2f(uPx, (float)W, (float)H);
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

    double t0 = now_s(), prev = t0, off = -1;
    int64_t shown = -1;                       /* the last frame on the screen */
    for (;;) {
        double t = now_s(), dt = t - prev;
        if (dt > 0.25) dt = 0.25;
        prev = t;
        uint64_t now = stream_now(dt);
        if ((display_poll(&d) || (stream.eof && now >= stream.head)) && off < 0) off = t;

        /* The newest frame that is complete at the sample being heard. */
        if (now >= N) {
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
        float cx = 1, cy = 1, flash = 0;
        if (off >= 0) crt_off(t - off, &cx, &cy, &flash);
        else if (t - t0 < CRT_ON) crt_on(t - t0, &cx, &cy, &flash);
        display_target(&d, iw, ih);
        glUseProgram(prog);
        glBindVertexArray(empty);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform2f(uCrt, cx, cy);
        glUniform1f(uFlash, flash);
        glUniform2f(uPx, (float)iw, (float)ih);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        display_present(&d);

        if (off >= 0 && t - off > CRT_OFF) break;
    }
    display_close(&d);
    return 0;
}
