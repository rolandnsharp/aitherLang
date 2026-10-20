/* vis: a field of colour. An aither patch run at every pixel, every frame.

     ./aither music.aither | tools/play hw:rockchipes8388c 20 | tools/vis fields/waves.aither
     tools/vis fields/waves.aither                  no music: input() and level() are 0, t the clock

   An audio patch is f(t), run at every sample; a field patch is
   f(pixel, t), run at every pixel of every frame on the GPU, compiled by
   `aither shader` (shader.nim). Its value at a pixel is a pair, shown as
   tv shows one: hue from the phase, brightness from the magnitude.
   pixel() is where the pixel is (x across -1..1, y up, square pixels);
   input() is the music's pair at the sample being heard, level() its
   loudness then, low() its loudness below 150 Hz (the kick), trace(x)
   the last 1024 pairs from a trigger, spectrum(f) its FFT at f Hz. A
   picture's memory is $state in a def a draw calls: a field, one pair a
   pixel, frame to frame, a texture here (at most 3); each frame the
   picture and every field are written in one pass, reading last
   frame's.

   Saving the patch reloads it within a frame; an error is printed and the
   old field keeps running. Stdin is copied to stdout unchanged. The field
   is computed at half the screen's size (--size WxH to choose) and drawn
   full screen (--window for a window); f toggles, q or Escape quits.
   --frames FILE writes every frame as raw RGB instead (--size, default
   640x360), and prints the ffmpeg line that makes it a video.
   See VIS.md. Build: make tools/vis (needs sdl3). */

#include "display.h"
#include <libgen.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

/* The field's pairs as colour: tv's domain colouring. */
static const char *ShowFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 uv;\n"
    "uniform highp sampler2D uPic;\n"
    "out vec4 o;\n"
    "vec3 hue(float h) {\n"
    "  return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "  vec2 z = texture(uPic, uv).rg;\n"
    "  float m = length(z);\n"
    "  vec3 col = vec3(0.0);\n"
    "  if (m > 0.0) col = hue(atan(z.y, z.x) / 6.2831853) * min(m, 1.0);\n"
    "  o = vec4(col, 1.0);\n"
    "}\n";

static char aither[4096];

/* The patch as a fragment shader, from `aither shader`; NULL (and the
   reason on stderr) if it doesn't compile. */
static char *compile_patch(const char *patch) {
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "'%s' shader '%s'", aither, patch);
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *src = malloc(cap);
    for (size_t k; (k = fread(src + n, 1, cap - n - 1, p)) > 0;) {
        n += k;
        if (n + 1 >= cap) src = realloc(src, cap *= 2);
    }
    src[n] = 0;
    int st = pclose(p);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { free(src); return NULL; }
    return src;
}

static GLuint try_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "vis: the GPU won't take it: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* The picture's program, or 0: the old one keeps running. *fields is
   how many fields it keeps (the compiler says, on its second line). */
static GLuint load(const char *patch, int *fields) {
    char *fs = compile_patch(patch);
    if (!fs) return 0;
    const char *at = strstr(fs, "// fields: ");
    *fields = at ? atoi(at + 11) : 0;
    GLuint f = try_shader(GL_FRAGMENT_SHADER, fs);
    free(fs);
    if (!f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, try_shader(GL_VERTEX_SHADER, GL_FULLSCREEN_VS));
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "vis: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* A pair a pixel: 32-bit floats where this GPU can draw into them and
   blend between them (a wave equation needs them), else 16-bit. */
static GLuint rg_texture(int w, int h, int precise) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    if (precise) glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, w, h, 0, GL_RG, GL_FLOAT, NULL);
    else glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, w, h, 0, GL_RG, GL_HALF_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* When the patch's voice started (its start_t), asked of the engine
   (`started <voice>` on its socket, AITHER_SOCK as for any command):
   the picture's start_t and phasors count from it, so they keep step
   with the sound through sends and retriggers. -1 if there's no
   answer (no engine, no such voice): keep what we had. */
static double voice_start(const char *voice) {
    const char *path = getenv("AITHER_SOCK");
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path ? path : "/tmp/aither.sock");
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct timeval tv = {0, 50000};                   /* never hold up a frame for long */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    double r = -1;
    char q[300], reply[128];
    snprintf(q, sizeof q, "started %s\n", voice);
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0 && write(s, q, strlen(q)) == (ssize_t)strlen(q)) {
        ssize_t n = read(s, reply, sizeof reply - 1);
        if (n > 3) {
            reply[n] = 0;
            if (!strncmp(reply, "OK ", 3)) r = atof(reply + 3);
        }
    }
    close(s);
    return r;
}

/* The trace: TRACE_N pairs, from a rising zero-crossing of the real part
   (as a scope triggers), so a steady tone stands still. The spectrum: a
   SPEC_N-point FFT of the pair as one complex signal, Hann-windowed,
   each bin's phase measured from sample 0 rather than from the window's
   start (here, exactly, for the bin's own frequency; the shader takes
   off the rest for the frequency it asks for, a few hertz at most, from
   uT0), so a steady tone keeps its phase frame to frame and two close
   tones turn at their difference; a full-scale tone reads 1. */
#define TRACE_N 1024
#define SPEC_N 4096

static void fft(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {                /* bit-reversed order */
        int b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j |= b;
        if (i < j) {
            double tr = re[i], ti = im[i];
            re[i] = re[j]; im[i] = im[j]; re[j] = tr; im[j] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -6.283185307179586 / len, wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int p = i + k, q = p + len / 2;
                double xr = re[q] * cr - im[q] * ci, xi = re[q] * ci + im[q] * cr;
                re[q] = re[p] - xr; im[q] = im[p] - xi;
                re[p] += xr; im[p] += xi;
                double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

/* Both, for the music at sample `now`, into the two textures; *t0 is
   when the spectrum's window starts, in seconds. */
static void readouts(uint64_t now, GLuint traceTex, GLuint specTex, double *t0) {
    static float buf[2 * SPEC_N], out[2 * SPEC_N];
    static double re[SPEC_N], im[SPEC_N];
    uint64_t from = now > 2 * TRACE_N ? now - 2 * TRACE_N : 0;
    size_t n = stream_range(buf, from, now);
    memset(out, 0, sizeof out);
    if (n >= TRACE_N) {
        size_t start = n - TRACE_N;
        for (size_t k = n - TRACE_N; k >= 1; k--)
            if (buf[2 * (k - 1)] < 0 && buf[2 * k] >= 0) { start = k; break; }
        memcpy(out, buf + 2 * start, sizeof(float) * 2 * TRACE_N);
    }
    glBindTexture(GL_TEXTURE_2D, traceTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TRACE_N, 1, GL_RG, GL_FLOAT, out);

    uint64_t s0 = now > SPEC_N ? now - SPEC_N : 0;
    *t0 = s0 / stream.rate;
    n = stream_range(buf, s0, now);
    memset(out, 0, sizeof out);
    if (n == SPEC_N) {
        for (int i = 0; i < SPEC_N; i++) {
            double w = 0.5 - 0.5 * cos(6.283185307179586 * i / SPEC_N);
            re[i] = buf[2 * i] * w;
            im[i] = buf[2 * i + 1] * w;
        }
        fft(re, im, SPEC_N);
        uint64_t off = s0 % SPEC_N;
        for (int k = 0; k < SPEC_N; k++) {
            double a = -6.283185307179586 * (double)(((uint64_t)k * off) % SPEC_N) / SPEC_N;
            double c = cos(a), s = sin(a);
            out[2 * k] = (float)((re[k] * c - im[k] * s) / (SPEC_N / 2));
            out[2 * k + 1] = (float)((re[k] * s + im[k] * c) / (SPEC_N / 2));
        }
    }
    glBindTexture(GL_TEXTURE_2D, specTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SPEC_N, 1, GL_RG, GL_FLOAT, out);
}

static double mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec * 1e-9;
}

static void usage(void) {
    fputs("usage: [aither music.aither |] vis field.aither [--size WxH] [--window | --frames FILE]\n"
          "  a field patch at every pixel; music pairs on stdin, the same out on stdout;\n"
          "  saving the patch reloads it; f toggles full screen, q or Escape quits\n", stderr);
}

int main(int argc, char **argv) {
    const char *patch = NULL;
    int RW = 0, RH = 0, windowed = 0, dest = DEST_WINDOW;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &RW, &RH) != 2 || RW < 1 || RH < 1) { usage(); return 2; }
        } else if (!strcmp(argv[i], "--window")) windowed = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { dest = DEST_FRAMES; path = argv[++i]; }
        else if (argv[i][0] != '-' && !patch) patch = argv[i];
        else { usage(); return 2; }
    }
    if (!patch) { usage(); return 2; }

    /* The compiler: $AITHER, or the aither next to tools/. */
    const char *env = getenv("AITHER");
    if (env) snprintf(aither, sizeof aither, "%s", env);
    else {
        char self[4096];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        self[n > 0 ? n : 0] = 0;
        snprintf(aither, sizeof aither, "%s/../aither", dirname(self));
    }

    struct stat in0;                          /* music is a pipe or a file; a terminal or /dev/null, none */
    int music = fstat(0, &in0) == 0 && (S_ISFIFO(in0.st_mode) || S_ISREG(in0.st_mode));
    if (music) stream_start();
    Display d = {0};
    d.windowed = windowed;
    d.nopace = 1;
    if (display_open(&d, "vis", dest, path, 0) < 0) return 1;
    int sw = 1280, sh = 720;
    if (dest == DEST_WINDOW) {
        SDL_SyncWindow(d.win);
        display_size(&d, &sw, &sh);
    }
    if (!RW) { RW = sw / 2; RH = sh / 2; }
    if (dest == DEST_FRAMES)
        fprintf(stderr, "vis: %dx%d rgb24 frames to %s\n"
                "  ffmpeg -f rawvideo -pix_fmt rgb24 -s %dx%d -r 60 -i %s out.mp4\n", RW, RH, path, RW, RH, path);

    int nf = 0;
    GLuint prog = load(patch, &nf);
    if (!prog) return 1;
    double seen = mtime(patch), checked = 0, asked = -1, start = 0;
    char voice[256];                           /* the voice is the patch's name, as send names it */
    {
        char tmp[4096];
        snprintf(tmp, sizeof tmp, "%s", patch);
        snprintf(voice, sizeof voice, "%s", basename(tmp));
        char *dot = strrchr(voice, '.');
        if (dot) *dot = 0;
    }
    GLuint show = gl_program(GL_FULLSCREEN_VS, ShowFS);
    GLuint empty;
    glGenVertexArrays(1, &empty);

    /* The picture, and each field now and last frame: a pair a pixel.
       fbo[cur] writes the picture and fields[k][cur]; fields[k][cur ^ 1]
       is last frame's. Rebuilt (and so started afresh) when a reload
       changes how many fields there are. */
    GLuint pic, fields[3][2] = {{0}}, fbo[2] = {0};
    int built = -1, fresh = 1;
    int precise = gl_has("GL_EXT_color_buffer_float") && gl_has("GL_OES_texture_float_linear");
    pic = rg_texture(RW, RH, 0);
    GLuint traceTex = rg_texture(TRACE_N, 1, precise), specTex = rg_texture(SPEC_N, 1, precise);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);   /* specTex: negative f wraps to the top bins */

    static float buf[2 * 2048];
    double t0 = now_s(), prev = t0;
    int cur = 0;
    unsigned frame = 0;
    for (;;) {
        double wall = now_s(), dt = wall - prev;
        if (dt > 0.25) dt = 0.25;
        if (dest == DEST_FRAMES) { dt = 1.0 / 60; wall = t0 + frame * dt; }   /* a recording runs at 60, however long it takes */
        prev = wall;
        if (display_poll(&d)) break;

        /* The music at the sample being heard: the pair, and the loudness
           over the last 2048 samples (46 ms at 44.1 kHz). */
        double t = wall - t0, level = 0, low = 0, t0spec = 0;
        float in[2] = {0, 0};
        if (music) {
            uint64_t now;
            if (dest == DEST_FRAMES) {            /* a recording: frame k shows the music at k/60 s */
                now = (uint64_t)(frame * stream.rate / 60);
                while (stream.head < now && !stream.eof && !display_quit) sleep_s(0.002);
                if (stream.head < now) break;
            } else {
                now = stream_now(dt);
                if (stream.eof && now >= stream.head) break;
            }
            t = now / stream.rate;
            uint64_t from = now > 2048 ? now - 2048 : 0;
            size_t n = stream_range(buf, from, now);
            for (size_t i = 0; i < n; i++) level += buf[2 * i] * buf[2 * i] + buf[2 * i + 1] * buf[2 * i + 1];
            /* low: the same window through two one-pole low-passes at
               150 Hz (settled within a few ms), its last 1024 samples. */
            double a = 1 - exp(-6.2831853 * 150 / stream.rate), l1 = 0, l2 = 0;
            size_t lown = 0;
            for (size_t i = 0; i < n; i++) {
                double m = (buf[2 * i] + buf[2 * i + 1]) * 0.5;
                l1 += a * (m - l1);
                l2 += a * (l1 - l2);
                if (i + 1024 >= n) { low += l2 * l2; lown++; }
            }
            if (lown) low = sqrt(low / lown);
            if (n) {
                level = sqrt(level / n);
                in[0] = buf[2 * (n - 1)];
                in[1] = buf[2 * (n - 1) + 1];
            }
            readouts(now, traceTex, specTex, &t0spec);
        }

        if (music && wall - asked > 0.25) {
            asked = wall;
            double s = voice_start(voice);
            if (s >= 0) start = s;
        }

        /* Saved? Load it; an error leaves the old field running. */
        if (wall - checked > 0.25) {
            checked = wall;
            double m = mtime(patch);
            if (m != seen) {
                seen = m;
                int n = 0;
                GLuint p = load(patch, &n);
                if (p) { glDeleteProgram(prog); prog = p; nf = n; fprintf(stderr, "vis: %s loaded\n", patch); }
            }
        }

        if (nf != built) {
            for (int k = 0; k < 3; k++)
                for (int i = 0; i < 2; i++)
                    if (fields[k][i]) { glDeleteTextures(1, &fields[k][i]); fields[k][i] = 0; }
            if (fbo[0]) glDeleteFramebuffers(2, fbo);
            glGenFramebuffers(2, fbo);
            for (int k = 0; k < nf; k++) { fields[k][0] = rg_texture(RW, RH, precise); fields[k][1] = rg_texture(RW, RH, precise); }
            for (int i = 0; i < 2; i++) {
                glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pic, 0);
                GLenum bufs[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
                for (int k = 0; k < nf; k++)
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1 + k, GL_TEXTURE_2D, fields[k][i], 0);
                glDrawBuffers(nf + 1, bufs);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    fprintf(stderr, "vis: %d fields at %dx%d won't fit this GPU\n", nf, RW, RH);
                    return 1;
                }
            }
            built = nf;
            fresh = 1;
        }

        /* The picture and its fields: every pixel, reading last frame's. */
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[cur]);
        glViewport(0, 0, RW, RH);
        glUseProgram(prog);
        glUniform1f(glGetUniformLocation(prog, "uT"), (float)t);
        glUniform1f(glGetUniformLocation(prog, "uRate"), music ? (float)stream.rate : (float)env_rate());
        glUniform1f(glGetUniformLocation(prog, "uFresh"), (float)fresh);
        glUniform1f(glGetUniformLocation(prog, "uPx"), 2.0f / RW);
        glUniform1f(glGetUniformLocation(prog, "uStart"), (float)start);
        glUniform1f(glGetUniformLocation(prog, "uT0"), (float)t0spec);
        glUniform1i(glGetUniformLocation(prog, "uTrace"), 4);
        glUniform1i(glGetUniformLocation(prog, "uSpec"), 5);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, traceTex);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, specTex);
        glUniform1f(glGetUniformLocation(prog, "uLevel"), (float)level);
        glUniform1f(glGetUniformLocation(prog, "uLow"), (float)low);
        glUniform1f(glGetUniformLocation(prog, "uFrame"), (float)frame);
        glUniform2f(glGetUniformLocation(prog, "uInput"), in[0], in[1]);
        glUniform2f(glGetUniformLocation(prog, "uSpan"), 1.0f, (float)RH / RW);
        for (int k = 0; k < nf; k++) {
            char name[8];
            snprintf(name, sizeof name, "uF%d", k);
            glUniform1i(glGetUniformLocation(prog, name), k + 1);
            glActiveTexture(GL_TEXTURE1 + k);
            glBindTexture(GL_TEXTURE_2D, fields[k][cur ^ 1]);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(empty);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        fresh = 0;

        /* On screen, as big as fits, its shape kept. */
        if (dest == DEST_FRAMES) display_target(&d, RW, RH);
        else {
            display_size(&d, &sw, &sh);
            double k = fmin((double)sw / RW, (double)sh / RH);
            display_target(&d, (int)(RW * k), (int)(RH * k));
        }
        glUseProgram(show);
        glUniform1i(glGetUniformLocation(show, "uPic"), 0);
        glBindTexture(GL_TEXTURE_2D, pic);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        display_present(&d);
        cur ^= 1;
        frame++;
    }
    display_close(&d);
    return 0;
}
