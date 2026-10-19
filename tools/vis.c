/* vis: a field of colour. An aither patch run at every pixel, every frame.

     ./aither music.aither | tools/play hw:rockchipes8388c 20 | tools/vis fields/waves.aither
     tools/vis fields/waves.aither                  no music: input() and level() are 0, t the clock

   An audio patch is f(t), run at every sample; a field patch is
   f(pixel, t), run at every pixel of every frame on the GPU, compiled by
   `aither shader` (shader.nim). Its value at a pixel is a pair, shown as
   tv shows one: hue from the phase, brightness from the magnitude.
   pixel() is where the pixel is (x across -1..1, y up, square pixels);
   input() is the music's pair at the sample being heard, level() its
   loudness then, low() its loudness below 150 Hz (the kick); last(p) is last frame's value at p, the field's memory.

   Saving the patch reloads it within a frame; an error is printed and the
   old field keeps running. Stdin is copied to stdout unchanged. The field
   is computed at half the screen's size (--size WxH to choose) and drawn
   full screen (--window for a window); f toggles, q or Escape quits.
   --frames FILE writes every frame as raw RGB instead (--size, default
   640x360), and prints the ffmpeg line that makes it a video.
   See VIS.md. Build: make tools/vis (needs sdl3). */

#include "display.h"
#include <libgen.h>
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

/* The field's program, or 0: the old one keeps running. */
static GLuint load(const char *patch) {
    char *fs = compile_patch(patch);
    if (!fs) return 0;
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

    GLuint prog = load(patch);
    if (!prog) return 1;
    double seen = mtime(patch), checked = 0;
    GLuint show = gl_program(GL_FULLSCREEN_VS, ShowFS);
    GLuint empty;
    glGenVertexArrays(1, &empty);

    /* The field now and last frame, two pairs a pixel. */
    GLuint tex[2], fbo[2];
    for (int i = 0; i < 2; i++) fbo[i] = gl_target(&tex[i], RW, RH, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_LINEAR);

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
        double t = wall - t0, level = 0, low = 0;
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
        }

        /* Saved? Load it; an error leaves the old field running. */
        if (wall - checked > 0.25) {
            checked = wall;
            double m = mtime(patch);
            if (m != seen) {
                seen = m;
                GLuint p = load(patch);
                if (p) { glDeleteProgram(prog); prog = p; fprintf(stderr, "vis: %s loaded\n", patch); }
            }
        }

        /* The field: every pixel, reading last frame's. */
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[cur]);
        glViewport(0, 0, RW, RH);
        glUseProgram(prog);
        glUniform1f(glGetUniformLocation(prog, "uT"), (float)t);
        glUniform1f(glGetUniformLocation(prog, "uDt"), (float)dt);
        glUniform1f(glGetUniformLocation(prog, "uLevel"), (float)level);
        glUniform1f(glGetUniformLocation(prog, "uLow"), (float)low);
        glUniform1f(glGetUniformLocation(prog, "uFrame"), (float)frame);
        glUniform2f(glGetUniformLocation(prog, "uInput"), in[0], in[1]);
        glUniform2f(glGetUniformLocation(prog, "uSpan"), 1.0f, (float)RH / RW);
        glUniform1i(glGetUniformLocation(prog, "uLast"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex[cur ^ 1]);
        glBindVertexArray(empty);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        /* On screen, as big as fits, its shape kept. */
        if (dest == DEST_FRAMES) display_target(&d, RW, RH);
        else {
            display_size(&d, &sw, &sh);
            double k = fmin((double)sw / RW, (double)sh / RH);
            display_target(&d, (int)(RW * k), (int)(RH * k));
        }
        glUseProgram(show);
        glUniform1i(glGetUniformLocation(show, "uPic"), 0);
        glBindTexture(GL_TEXTURE_2D, tex[cur]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        display_present(&d);
        cur ^= 1;
        frame++;
    }
    display_close(&d);
    return 0;
}
