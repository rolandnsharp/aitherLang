/* scope: a vector display for aither's pairs.

     aither patch.aither | scope | play        see it and hear it
     aither trace.aither | scope               see it
     ... | scope --term                        in the terminal (over ssh)
     ... | scope --frames out.rgb --size 720   raw RGB frames for ffmpeg

   Each pair is a point: x and y in -1..1, y up, square. The beam joins
   one point to the next with the same energy per sample, so it is bright
   where it lingers and faint where it moves fast, and the phosphor fades
   it. A point outside the square blanks the beam. Stdin is copied to
   stdout unchanged, so scope can sit anywhere in a chain. It opens full
   screen (--window for a window); f toggles, q or Escape closes it at once.

     --palette green|amber|cyan|blue|white|red   the phosphor (green, P31)
     --decay F    what is left of a trace after 1/60 s, 0..1 (0.85; 0.2 for --ridge)
     --ridge F    a ridge plot instead: the stream folded at a period of F
                  Hz (108bpm, or auto: locked to the pitch, as tv --fold),
                  each period a ridge, the newest in front, drawn in
                  perspective with the ones behind hidden where the nearer
                  stand in front of them: Craft's 1970 plot of successive
                  pulses of the pulsar CP 1919. A ridge's height is x.
     --helix F    a helix instead: the pair in three dimensions, its real
                  part toward you and its imaginary part up, time along an
                  axis across the screen, the last 8 periods of F (as
                  --ridge, auto too), ending on a period, so a steady sound
                  stands still. A spin is a corkscrew, conj its mirror. The
                  real part's shadow on the floor, the imaginary's on the
                  back wall: the cosine and the sine.
     --gain G     ridges G times taller, a helix G times wider (1)

   Drawn on the GPU (OpenGL ES 3); without one, Mesa draws the same thing
   on the CPU. The rate, for the beam's energy and to pace a source that
   isn't real-time, is AITHER_RATE, like the engine's.
   See notes/displays-2026-10-16.md. Build: make tools/scope (needs sdl3). */

#include "display.h"

/* The phosphors: P31, P12, P7, P11, P4, P22-R. */
static const struct { const char *name; float rgb[3]; } Palettes[] = {
    {"green", {0.30f, 1.00f, 0.45f}},
    {"amber", {1.00f, 0.62f, 0.10f}},
    {"cyan",  {0.35f, 0.95f, 1.00f}},
    {"blue",  {0.30f, 0.55f, 1.00f}},
    {"white", {0.92f, 0.94f, 1.00f}},
    {"red",   {1.00f, 0.28f, 0.22f}},
};

#define MAXPTS (1 << 16)    /* points drawn per frame at most: the newest */
#define Q0     1.5e5        /* the beam's energy per second at 720 px: a figure that
                               repeats (a 220:330 Lissajous) sits mid-range */
#define BLOOM  0.6f         /* how much of the blurred copy glows around the trace */

/* The beam: each segment is one instance, a quad around the path from a
   (this sample) to b (the next), reaching four beam radii past it. */
static const char *BeamVS =
    "#version 300 es\n"
    "layout(location = 0) in vec2 corner;\n"
    "layout(location = 1) in vec2 a;\n"
    "layout(location = 2) in vec2 b;\n"
    "uniform vec2 uSize;\n"
    "uniform float uSigma;\n"
    "out vec2 vA;\n"
    "out vec2 vB;\n"
    "out vec2 vP;\n"
    "void main() {\n"
    "  vec2 pa = (a * 0.5 + 0.5) * uSize, pb = (b * 0.5 + 0.5) * uSize;\n"
    "  vec2 d = pb - pa;\n"
    "  float len = length(d);\n"
    "  vec2 u = len > 1e-4 ? d / len : vec2(1.0, 0.0);\n"
    "  float r = 4.0 * uSigma;\n"
    "  vec2 p = (corner.x < 0.0 ? pa - u * r : pb + u * r) + vec2(-u.y, u.x) * (corner.y * r);\n"
    "  vA = pa; vB = pb; vP = p;\n"
    "  bool on = all(lessThanEqual(abs(a), vec2(1.0))) && all(lessThanEqual(abs(b), vec2(1.0)));\n"
    "  gl_Position = on ? vec4(p / uSize * 2.0 - 1.0, 0.0, 1.0) : vec4(2.0, 2.0, 0.0, 1.0);\n"
    "}\n";

/* The light a gaussian spot leaves at vP as it sweeps from vA to vB in
   one sample: the spot integrated along the path, the same energy per
   sample however long the path is. Off the square (or not a number) a
   point blanks the beam, above. */
static const char *BeamFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 vA;\n"
    "in vec2 vB;\n"
    "in vec2 vP;\n"
    "uniform float uSigma;\n"
    "uniform float uEnergy;\n"
    "out vec4 o;\n"
    "float erf1(float x) {\n"                       /* Abramowitz-Stegun 7.1.26 */
    "  float s = sign(x);\n"
    "  x = abs(x);\n"
    "  float t = 1.0 / (1.0 + 0.3275911 * x);\n"
    "  return s * (1.0 - ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t\n"
    "                      - 0.284496736) * t + 0.254829592) * t * exp(-x * x));\n"
    "}\n"
    "void main() {\n"
    "  vec2 d = vB - vA, q = vP - vA;\n"
    "  float len = length(d), s2 = uSigma * uSigma, light;\n"
    "  if (len < 0.01) {\n"
    "    light = uEnergy * exp(-dot(q, q) / (2.0 * s2)) / (6.2831853 * s2);\n"
    "  } else {\n"
    "    vec2 u = d / len;\n"
    "    float along = dot(q, u), across = dot(q, vec2(-u.y, u.x)), k = 0.70710678 / uSigma;\n"
    "    light = uEnergy / len * 0.5 * (erf1(along * k) - erf1((along - len) * k))\n"
    "          * exp(-across * across / (2.0 * s2)) / (2.5066283 * uSigma);\n"
    "  }\n"
    "  o = vec4(light, 0.0, 0.0, 0.0);\n"
    "}\n";

/* The fade: drawn with blending (zero, constant colour), so the phosphor
   is multiplied in place by the decay. */
static const char *FadeFS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "out vec4 o;\n"
    "void main() { o = vec4(0.0); }\n";

/* A quarter-size copy for the bloom: each pixel the mean of 4x4. */
static const char *DownFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform highp sampler2D uSrc;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  ivec2 b = ivec2(gl_FragCoord.xy) * 4, m = textureSize(uSrc, 0) - 1;\n"
    "  float s = 0.0;\n"
    "  for (int y = 0; y < 4; y++)\n"
    "    for (int x = 0; x < 4; x++)\n"
    "      s += texelFetch(uSrc, min(b + ivec2(x, y), m), 0).r;\n"
    "  o = vec4(min(s / 16.0, 1000.0), 0.0, 0.0, 0.0);\n"
    "}\n";

/* A gaussian blur, one direction at a time. */
static const char *BlurFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 uv;\n"
    "uniform highp sampler2D uSrc;\n"
    "uniform vec2 uStep;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  float w[4];\n"
    "  w[0] = 0.1945946; w[1] = 0.1216216; w[2] = 0.054054; w[3] = 0.016216;\n"
    "  float s = texture(uSrc, uv).r * 0.227027;\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    vec2 off = uStep * float(i + 1);\n"
    "    s += (texture(uSrc, uv + off).r + texture(uSrc, uv - off).r) * w[i];\n"
    "  }\n"
    "  o = vec4(s, 0.0, 0.0, 0.0);\n"
    "}\n";

/* The picture: light into the phosphor's colour, white-hot where it
   burns. */
static const char *CompFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 uv;\n"
    "uniform highp sampler2D uAccum;\n"
    "uniform highp sampler2D uBloom;\n"
    "uniform vec3 uPhos;\n"
    "uniform float uBloomK;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  float light = texture(uAccum, uv).r + uBloomK * texture(uBloom, uv).r;\n"
    "  vec3 col = uPhos * (1.0 - exp(-light));\n"
    "  col = mix(col, vec3(1.0), smoothstep(2.5, 14.0, light) * 0.85);\n"
    "  o = vec4(col + uPhos * 0.012, 1.0);\n"
    "}\n";

/* --ridge: RIDGES periods, RIDGE_W points each, one a 24th of a second,
   the newest in front. Depth d is how many 24ths of a second ago a ridge
   ended, so they recede smoothly whatever the period. Stacked up the
   screen evenly as Craft's are, a little narrower further back, and
   drawn front to back against a
   floating horizon: the highest any nearer ridge has reached at each
   place across the screen. A point under it is hidden, and the beam
   blanks there (a point off the square). */
#define RIDGES  48
#define RIDGE_W 192
#define HBINS   720
typedef struct { float z[RIDGE_W * 2]; uint64_t end; int used; } Ridge;

static size_t ridge_points(const Ridge *rg, int newest, uint64_t now, double rate, double gain, float *pts) {
    static float horizon[HBINS];
    for (int b = 0; b < HBINS; b++) horizon[b] = -2;
    size_t m = 0;
    pts[m * 2] = 2; pts[m * 2 + 1] = 2; m++;
    for (int k = 0; k < RIDGES; k++) {
        const Ridge *r = &rg[(newest - k + RIDGES) % RIDGES];
        if (!r->used) continue;
        double d = (double)(now - r->end) * 24 / rate;
        if (d > RIDGES) continue;
        double z = 1 + d * 0.012, base = -0.85 + 1.5 * d / RIDGES;
        float ys[RIDGE_W], xs[RIDGE_W];
        for (int c = 0; c < RIDGE_W; c++) {
            double X = (2.0 * c / (RIDGE_W - 1) - 1) * 0.9;
            xs[c] = (float)(X / z);
            ys[c] = (float)(base + 0.3 * gain * r->z[2 * c] / z);
        }
        for (int c = 0; c < RIDGE_W; c++) {
            int b = (int)((xs[c] * 0.5f + 0.5f) * (HBINS - 1));
            int seen = b >= 0 && b < HBINS && ys[c] > horizon[b];
            pts[m * 2] = seen ? xs[c] : 2;
            pts[m * 2 + 1] = seen ? ys[c] : 2;
            m++;
        }
        pts[m * 2] = 2; pts[m * 2 + 1] = 2; m++;
        /* This ridge now hides what is behind it. */
        for (int c = 0; c + 1 < RIDGE_W; c++) {
            int b0 = (int)((xs[c] * 0.5f + 0.5f) * (HBINS - 1)), b1 = (int)((xs[c + 1] * 0.5f + 0.5f) * (HBINS - 1));
            for (int b = b0; b <= b1; b++) {
                if (b < 0 || b >= HBINS) continue;
                float t = b1 > b0 ? (float)(b - b0) / (b1 - b0) : 0;
                float y = ys[c] + (ys[c + 1] - ys[c]) * t;
                if (y > horizon[b]) horizon[b] = y;
            }
        }
    }
    return m;
}

/* --helix: the window's pairs as a curve in space, seen from in front, a
   little to the right and above. World: time along x (oldest left), the
   imaginary part up, the real part toward the viewer. The floor and the
   back wall catch the shadows; drawn with a third as many segments,
   each carrying a sample's energy over three samples' length, they glow
   a third as bright, and the axis fainter still. */
#define HELIX_PERIODS 8
static void helix_view(double t, double y, double z, double aspect, float *out) {
    double a = -0.5, b = 0.5, D = 5.5;
    double X = t * cos(a) + z * sin(a), Z = -t * sin(a) + z * cos(a);
    double Y = y * cos(b) - Z * sin(b), Z2 = y * sin(b) + Z * cos(b);
    double k = D / (D - Z2) * 0.8;
    out[0] = (float)(X * k / aspect);
    out[1] = (float)(Y * k + 0.12);
}
static size_t helix_points(const float *win, size_t N, double aspect, double gain, float *pts) {
    double L = 2.6 * aspect / 1.78, R = 0.55 * gain, floorY = -1.3 * 0.55, wallZ = -1.3 * 0.55;
    size_t m = 0, step = N > 6000 ? (N + 5999) / 6000 : 1;
#define BLANK() (pts[2 * m] = 2, pts[2 * m + 1] = 2, m++)
    BLANK();
    for (int set = 0; set < 3; set++) {
        size_t st = set ? step * 3 : step;
        for (size_t i = 0; i < N; i += st) {
            double t = ((double)i / (N - 1) - 0.5) * L, x = win[2 * i] * R, y = win[2 * i + 1] * R;
            if (set == 0) helix_view(t, y, x, aspect, pts + 2 * m);
            else if (set == 1) helix_view(t, floorY, x, aspect, pts + 2 * m);
            else helix_view(t, y, wallZ, aspect, pts + 2 * m);
            m++;
        }
        BLANK();
    }
    for (int i = 0; i <= 24; i++) {                 /* the axis: no real part, no imaginary */
        helix_view(((double)i / 24 - 0.5) * L, 0, 0, aspect, pts + 2 * m);
        m++;
    }
    BLANK();
#undef BLANK
    return m;
}

static void usage(void) {
    fputs("usage: aither patch.aither | scope [--palette green|amber|cyan|blue|white|red]\n"
          "                                   [--decay 0.85] [--ridge|--helix HZ|BPMbpm|auto [--gain G]]\n"
          "                                   [--window | --term | --frames FILE [--size 720]]\n"
          "  float pairs in on stdin, the same out on stdout; full screen unless --window;\n"
          "  f toggles full screen, q or Escape quits\n",
          stderr);
}

int main(int argc, char **argv) {
    const char *pal = "green", *path = NULL;
    double decay = -1, foldHz = 0, gain = 1;
    int helix = 0;
    int dest = -1, size = 720, windowed = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--palette") && i + 1 < argc) pal = argv[++i];
        else if (!strcmp(argv[i], "--decay") && i + 1 < argc) decay = atof(argv[++i]);
        else if ((!strcmp(argv[i], "--ridge") || !strcmp(argv[i], "--helix")) && i + 1 < argc) {
            char *end;
            helix = !strcmp(argv[i], "--helix");
            if (!strcmp(argv[++i], "auto")) foldHz = -1;
            else {
                foldHz = strtod(argv[i], &end);
                if (!strcmp(end, "bpm")) foldHz /= 60;
                else if (*end || !(foldHz > 0)) { usage(); return 2; }
            }
        } else if (!strcmp(argv[i], "--gain") && i + 1 < argc) gain = atof(argv[++i]);
        else if (!strcmp(argv[i], "--window")) windowed = 1;
        else if (!strcmp(argv[i], "--term")) dest = DEST_TERM;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { dest = DEST_FRAMES; path = argv[++i]; }
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) size = atoi(argv[++i]);
        else { usage(); return 2; }
    }
    const float *phos = NULL;
    for (size_t i = 0; i < sizeof Palettes / sizeof *Palettes; i++)
        if (!strcmp(pal, Palettes[i].name)) phos = Palettes[i].rgb;
    if (decay < 0) decay = foldHz != 0 ? 0.2 : 0.85;   /* ridges and helices move: a long glow would smear them */
    if (!phos || decay < 0 || decay > 1 || size < 16) { usage(); return 2; }
    if (isatty(0)) {
        fputs("scope: stdin is a terminal. Pipe pairs into me:\n"
              "  aither patch.aither | scope | play\n", stderr);
        return 2;
    }
    if (dest < 0) dest = (getenv("WAYLAND_DISPLAY") || getenv("DISPLAY")) ? DEST_WINDOW : DEST_TERM;

    stream_start();
    Display d = {0};
    d.windowed = windowed;
    if (display_open(&d, "scope", dest, path, size) < 0) return 1;
    if (dest == DEST_FRAMES) {
        int fh = foldHz != 0 ? size * 9 / 16 : size;
        fprintf(stderr, "scope: %dx%d rgb24 frames at %d fps to %s\n"
                "  ffmpeg -f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i %s out.mp4\n",
                size, fh, HEADLESS_FPS, path, size, fh, HEADLESS_FPS, path);
    }

    GLuint beam = gl_program(BeamVS, BeamFS), fade = gl_program(GL_FULLSCREEN_VS, FadeFS),
           down = gl_program(GL_FULLSCREEN_VS, DownFS), blur = gl_program(GL_FULLSCREEN_VS, BlurFS),
           comp = gl_program(GL_FULLSCREEN_VS, CompFS);
    GLint uSize = glGetUniformLocation(beam, "uSize"), uSigma = glGetUniformLocation(beam, "uSigma"),
          uEnergy = glGetUniformLocation(beam, "uEnergy"), uStep = glGetUniformLocation(blur, "uStep"),
          cPhos = glGetUniformLocation(comp, "uPhos"), cBloomK = glGetUniformLocation(comp, "uBloomK");
    glUseProgram(down);
    glUniform1i(glGetUniformLocation(down, "uSrc"), 0);
    glUseProgram(blur);
    glUniform1i(glGetUniformLocation(blur, "uSrc"), 0);
    glUseProgram(comp);
    glUniform1i(glGetUniformLocation(comp, "uAccum"), 0);
    glUniform1i(glGetUniformLocation(comp, "uBloom"), 1);

    /* Two vertex arrays: none at all for the full-screen passes, and the
       beam's: four corners, and the points as they came off the pipe,
       read twice per instance (a, and b one point on). */
    GLuint empty, beamVao, cornerBuf, ptBuf;
    glGenVertexArrays(1, &empty);
    glGenVertexArrays(1, &beamVao);
    glBindVertexArray(beamVao);
    static const float corners[8] = {-1, -1, -1, 1, 1, -1, 1, 1};
    glGenBuffers(1, &cornerBuf);
    glBindBuffer(GL_ARRAY_BUFFER, cornerBuf);
    glBufferData(GL_ARRAY_BUFFER, sizeof corners, corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glGenBuffers(1, &ptBuf);
    glBindBuffer(GL_ARRAY_BUFFER, ptBuf);
    glBufferData(GL_ARRAY_BUFFER, 16, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8, (void *)8);
    glVertexAttribDivisor(2, 1);

    float *pts = malloc((size_t)(MAXPTS + 1) * 2 * sizeof(float));
    Folder fo = {0};
    Ridge *ridges = calloc(RIDGES, sizeof(Ridge));
    float *win = malloc((size_t)MAXPTS * 2 * sizeof(float));   /* --helix: the window's pairs */
    int newest = 0;
    if (foldHz != 0) folder_init(&fo, RIDGE_W, foldHz > 0 ? stream.rate / foldHz : 0, "scope");
    float last[2] = {2, 2};                   /* off the square: the first segment is blank */
    uint64_t shown = 0;                       /* samples drawn so far */
    int Sw = 0, Sh = 0, Bw = 0, Bh = 0;
    GLuint accT = 0, accF = 0, bT[2] = {0, 0}, bF[2] = {0, 0};
    double prev = now_s();

    for (;;) {
        double t = now_s(), dt = t - prev;
        if (dt > 0.25) dt = 0.25;
        prev = t;
        if (display_poll(&d) || (stream.eof && shown >= stream.head)) break;

        /* The phosphor is a float buffer the size of the picture, so the
           faintest trail fades all the way out; the bloom works at a
           quarter of the size. The beam's picture is square; ridges fill
           the screen (in frames, 16:9). */
        int w, h;
        display_size(&d, &w, &h);
        if (foldHz != 0 && dest == DEST_FRAMES) h = w * 9 / 16;
        int s = w < h ? w : h;
        if (s < 16) s = 16;
        int sw = foldHz != 0 ? (w < 16 ? 16 : w) : s, sh = foldHz != 0 ? (h < 16 ? 16 : h) : s;
        if (sw != Sw || sh != Sh) {
            if (accF) {
                glDeleteFramebuffers(1, &accF);
                glDeleteTextures(1, &accT);
                glDeleteFramebuffers(2, bF);
                glDeleteTextures(2, bT);
            }
            Sw = sw;
            Sh = sh;
            Bw = Sw / 4 > 1 ? Sw / 4 : 1;
            Bh = Sh / 4 > 1 ? Sh / 4 : 1;
            accF = gl_target(&accT, Sw, Sh, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
            for (int i = 0; i < 2; i++) bF[i] = gl_target(&bT[i], Bw, Bh, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_LINEAR);
        }

        /* The samples up to the one being heard now, after the last one drawn. */
        uint64_t now = stream_now(dt);
        size_t n = 0;
        if (foldHz != 0 && helix) {
            /* The last periods, ending on the newest period's end. */
            folder_skip(&fo, now, HELIX_PERIODS);
            while (folder_next(&fo, now)) {}
            size_t N = (size_t)(HELIX_PERIODS * fo.P);
            uint64_t end = (uint64_t)fo.start;
            size_t m = 1;
            pts[0] = pts[1] = 2;
            if (end > N && N > 1 && N <= MAXPTS && stream_range(win, end - N, end) == N)
                m = helix_points(win, N, (double)Sw / Sh, gain, pts);
            n = m - 1;
            shown = now;
            glBindBuffer(GL_ARRAY_BUFFER, ptBuf);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m * 2 * sizeof(float)), pts, GL_STREAM_DRAW);
        } else if (foldHz != 0) {
            /* Every period done by now; one a 24th of a second is kept. */
            int stride = (int)fmax(1, lround(stream.rate / fo.P / 24));
            folder_skip(&fo, now, stride * RIDGES);
            float *row;
            while ((row = folder_next(&fo, now))) {
                if ((fo.r - 1) % stride) continue;
                newest = (newest + 1) % RIDGES;
                memcpy(ridges[newest].z, row, sizeof ridges[newest].z);
                ridges[newest].end = (uint64_t)fo.start;
                ridges[newest].used = 1;
            }
            size_t m = ridge_points(ridges, newest, now, stream.rate, gain, pts);
            n = m - 1;
            shown = now;
            glBindBuffer(GL_ARRAY_BUFFER, ptBuf);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m * 2 * sizeof(float)), pts, GL_STREAM_DRAW);
        } else if (now > shown) {
            if (now - shown > MAXPTS) { shown = now - MAXPTS; last[0] = last[1] = 2; }
            n = stream_range(pts + 2, shown, now);
            if (n < now - shown) last[0] = last[1] = 2;   /* lost some: start from a blank */
            shown = now;
        }
        if (foldHz == 0) {
            pts[0] = last[0];
            pts[1] = last[1];
            if (n) {
                last[0] = pts[2 * n];
                last[1] = pts[2 * n + 1];
                glBindBuffer(GL_ARRAY_BUFFER, ptBuf);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((n + 1) * 2 * sizeof(float)), pts, GL_STREAM_DRAW);
            }
        }

        /* 1. The phosphor fades, by the time that has passed. */
        glBindFramebuffer(GL_FRAMEBUFFER, accF);
        glViewport(0, 0, Sw, Sh);
        glEnable(GL_BLEND);
        float f = (float)pow(decay, dt * 60);
        glBlendColor(f, f, f, f);
        glBlendFunc(GL_ZERO, GL_CONSTANT_COLOR);
        glUseProgram(fade);
        glBindVertexArray(empty);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        /* 2. The beam, one segment per sample, adding its light. Its width
           and energy scale with the picture, so it looks the same at any
           size; the energy per sample with the rate, so it looks the same
           at any rate. */
        if (n) {
            float sigma = fmaxf(0.8f, 1.3f * (float)Sh / 720.0f);
            glBlendFunc(GL_ONE, GL_ONE);
            glUseProgram(beam);
            glUniform2f(uSize, (float)Sw, (float)Sh);
            glUniform1f(uSigma, sigma);
            /* Ridges and helices fade fast and are drawn afresh every frame:
               four times the energy, about what a steady trace builds up
               under the default glow. */
            double boost = foldHz != 0 ? 4 : 1;
            glUniform1f(uEnergy, (float)(boost * Q0 * (Sh / 720.0) * (sigma / 1.3) / stream.rate));
            glBindVertexArray(beamVao);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)n);
        }
        glDisable(GL_BLEND);

        /* 3. The bloom: a quarter-size copy, blurred across, then down. */
        glBindVertexArray(empty);
        glActiveTexture(GL_TEXTURE0);
        glBindFramebuffer(GL_FRAMEBUFFER, bF[0]);
        glViewport(0, 0, Bw, Bh);
        glUseProgram(down);
        glBindTexture(GL_TEXTURE_2D, accT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUseProgram(blur);
        glBindFramebuffer(GL_FRAMEBUFFER, bF[1]);
        glBindTexture(GL_TEXTURE_2D, bT[0]);
        glUniform2f(uStep, 1.0f / (float)Bw, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, bF[0]);
        glBindTexture(GL_TEXTURE_2D, bT[1]);
        glUniform2f(uStep, 0, 1.0f / (float)Bh);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        /* 4. The picture, in the phosphor's colour, wherever it goes. */
        display_target(&d, Sw, Sh);
        glUseProgram(comp);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, accT);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bT[0]);
        glUniform3f(cPhos, phos[0], phos[1], phos[2]);
        glUniform1f(cBloomK, BLOOM);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE0);
        display_present(&d);
    }
    display_close(&d);
    return 0;
}
