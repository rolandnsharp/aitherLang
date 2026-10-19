/* display.h: what scope and tv share.

   Pairs in on stdin, copied unchanged to stdout by a reader thread, so a
   slow screen never stalls the stream; a GL context, from an SDL3 window
   or from headless EGL; and the finished frame's three destinations: the
   window, the terminal (Braille, for ssh), and raw RGB frames (for
   ffmpeg). Header-only: each tool is one file that includes this first.
   See notes/displays-2026-10-16.md. */

#ifndef DISPLAY_H
#define DISPLAY_H

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <SDL3/SDL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void sleep_s(double s) {
    if (s <= 0) return;
    struct timespec t = {(time_t)s, (long)((s - (time_t)s) * 1e9)};
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}

/* The rate: what the engine runs at, so it is the engine's variable. */
static double env_rate(void) {
    const char *r = getenv("AITHER_RATE");
    double v = r ? atof(r) : 0;
    return v > 0 ? v : 48000;
}

/* ------------------------------------------------------------- the stream */

#define RING (1 << 20)        /* the pairs the screen can still reach: 22 s at 48 kHz,
                                 0.9 s at the 1.15 MHz a 160x120 tv takes */

static struct {
    float xy[RING * 2];
    volatile uint64_t head;   /* pairs that have arrived */
    volatile uint64_t passed; /* bytes passed on downstream */
    pthread_mutex_t lock;
    volatile int eof;
    volatile int pass;        /* copying stdin to stdout */
    int piped;                /* ... into a pipe, whose reader we can watch */
    double rate;
} stream = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void stream_put(const float *p, size_t n) {
    pthread_mutex_lock(&stream.lock);
    for (size_t i = 0; i < n; i++) {
        uint64_t k = (stream.head + i) % RING;
        stream.xy[2 * k] = p[2 * i];
        stream.xy[2 * k + 1] = p[2 * i + 1];
    }
    stream.head += n;
    pthread_mutex_unlock(&stream.lock);
}

static void *stream_reader(void *arg) {
    (void)arg;
    static unsigned char buf[1 << 14];
    static float pairs[(1 << 14) / 4 + 2];
    unsigned char carry[8];
    size_t nc = 0;
    double t0 = now_s(), taken = 0;
    for (;;) {
        /* A source that isn't real-time (an engine with nothing paced
           upstream) would run as fast as we read. Stay within 1 % of the
           rate, with a quarter of a second of slack; behind a real-time
           source this never waits. */
        double ahead = taken - (stream.rate * 1.01 * (now_s() - t0) + stream.rate * 0.25);
        if (ahead > 0) { sleep_s(ahead / stream.rate + 0.001); continue; }
        ssize_t n = read(0, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        /* Whole pairs into the ring first, so the screen has every sample
           before the reader downstream does; a partial pair waits for the
           next read. */
        size_t off = 0, np = 0;
        if (nc) {
            size_t need = 8 - nc, k = (size_t)n < need ? (size_t)n : need;
            memcpy(carry + nc, buf, k);
            nc += k;
            off = k;
            if (nc == 8) { memcpy(pairs, carry, 8); np = 1; nc = 0; }
        }
        size_t whole = ((size_t)n - off) / 8;
        memcpy(pairs + 2 * np, buf + off, whole * 8);
        np += whole;
        off += whole * 8;
        nc = (size_t)n - off;
        memcpy(carry, buf + off, nc);
        if (np) stream_put(pairs, np);
        taken += np;
        if (stream.pass) {                    /* then on, exactly as it came */
            for (ssize_t o = 0; o < n;) {
                ssize_t w = write(1, buf + o, (size_t)(n - o));
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) { stream.pass = 0; break; }   /* the reader downstream left */
                o += w;
                stream.passed += (uint64_t)w;
            }
        }
    }
    stream.eof = 1;
    if (stream.pass) {
        /* The end, passed on: our end of the pipe closes, and fd 1 stays
           taken (by /dev/null) so nothing else lands on it. */
        stream.pass = 0;
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) { dup2(null, 1); close(null); }
    }
    return NULL;
}

static void stream_start(void) {
    struct stat st;
    stream.rate = env_rate();
    stream.pass = !isatty(1);
    stream.piped = fstat(1, &st) == 0 && S_ISFIFO(st.st_mode);
    signal(SIGPIPE, SIG_IGN);
    pthread_t t;
    pthread_create(&t, NULL, stream_reader, NULL);
    pthread_detach(t);
}

/* The pairs from index `from` up to `to`, as many as the ring still has. */
static size_t stream_range(float *dst, uint64_t from, uint64_t to) {
    pthread_mutex_lock(&stream.lock);
    uint64_t lo = stream.head > RING ? stream.head - RING : 0;
    if (from < lo) from = lo;
    if (to > stream.head) to = stream.head;
    size_t n = to > from ? (size_t)(to - from) : 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t k = (from + i) % RING;
        dst[2 * i] = stream.xy[2 * k];
        dst[2 * i + 1] = stream.xy[2 * k + 1];
    }
    pthread_mutex_unlock(&stream.lock);
    return n;
}

/* How far the picture should have got: the sample being heard now.
   Passing the stream on into a pipe, that is what the reader downstream
   has taken (what you hear is what you see, however the samples came in
   bursts); otherwise a clock running at the rate, never ahead of what
   has arrived and never far behind it. Call once a frame. */
static uint64_t stream_now(double dt) {
    static double clock;
    double limit = (double)stream.head;
    int queued = 0;
    if (stream.pass && stream.piped && ioctl(1, FIONREAD, &queued) == 0) {
        double heard = ((double)stream.passed - queued) / 8;
        if (heard < limit) limit = heard > 0 ? heard : 0;
    }
    double lag = stream.rate * 0.25, back = stream.rate * 0.05;
    if (lag > RING / 2) lag = RING / 2;         /* at MHz rates the ring is the limit */
    if (back > RING / 8) back = RING / 8;
    clock += stream.rate * dt;
    if (clock > limit) clock = limit;
    if (clock < limit - lag) clock = limit - back;
    return (uint64_t)clock;
}

/* ------------------------------------------------------------ the screen */

enum { DEST_WINDOW, DEST_TERM, DEST_FRAMES };
#define HEADLESS_FPS 30

typedef struct {
    int dest;
    int halfblock;                /* terminal: half-blocks, two colours a cell (tv), not Braille */
    int nopace;                   /* frames: each one as it is drawn, no frame rate (tv) */
    int windowed;                 /* a window (--window), not full screen */
    SDL_Window *win;              /* the window */
    SDL_GLContext ctx;
    EGLDisplay egl;               /* headless: EGL with no surface */
    EGLContext eglctx;
    GLuint fbo, tex;              /* headless: the finished frame */
    int fw, fh;                   /* its size now */
    int size;                     /* --size for frames */
    int tty_in, tty_out;          /* the terminal we draw on */
    struct termios saved;
    int frames;                   /* where raw frames go */
    unsigned char *pix;
    double next;                  /* headless pacing */
} Display;

static volatile sig_atomic_t display_quit;
static void on_signal(int s) { (void)s; display_quit = 1; }

static int display_open(Display *d, const char *title, int dest, const char *path, int size) {
    int halfblock = d->halfblock, nopace = d->nopace, windowed = d->windowed;  /* the caller's choices survive */
    memset(d, 0, sizeof *d);
    d->dest = dest;
    d->halfblock = halfblock;
    d->nopace = nopace;
    d->windowed = windowed;
    d->size = size;
    d->tty_in = d->tty_out = d->frames = -1;
    if (dest == DEST_WINDOW) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            fprintf(stderr, "%s: no display: %s (try --term)\n", title, SDL_GetError());
            return -1;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        /* Full screen (on the uConsole, the whole screen); f toggles. */
        d->win = SDL_CreateWindow(title, 720, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                  (windowed ? 0 : SDL_WINDOW_FULLSCREEN));
        if (!d->win || !(d->ctx = SDL_GL_CreateContext(d->win))) {
            fprintf(stderr, "%s: no OpenGL ES 3 window: %s\n", title, SDL_GetError());
            return -1;
        }
        SDL_GL_SetSwapInterval(1);            /* one frame per refresh: no tearing */
        return 0;
    }
    /* Headless: EGL with no surface at all (Mesa's surfaceless platform),
       the GPU if there is one, Mesa's software rasteriser if not. */
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    d->egl = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    if (d->egl == EGL_NO_DISPLAY || !eglInitialize(d->egl, NULL, NULL)) {
        fprintf(stderr, "%s: no headless EGL\n", title);
        return -1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE}, n = 0;
    EGLConfig cfg;
    eglChooseConfig(d->egl, ca, &cfg, 1, &n);
    EGLint cx[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    d->eglctx = eglCreateContext(d->egl, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cx);
    if (d->eglctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(d->egl, EGL_NO_SURFACE, EGL_NO_SURFACE, d->eglctx)) {
        fprintf(stderr, "%s: no OpenGL ES 3 context\n", title);
        return -1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (dest == DEST_TERM) {
        d->tty_out = open("/dev/tty", O_WRONLY);
        d->tty_in = open("/dev/tty", O_RDONLY | O_NONBLOCK);
        if (d->tty_out < 0 || d->tty_in < 0) { fprintf(stderr, "%s: no terminal\n", title); return -1; }
        tcgetattr(d->tty_in, &d->saved);
        struct termios r = d->saved;
        r.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        r.c_cc[VMIN] = 0;
        r.c_cc[VTIME] = 0;
        tcsetattr(d->tty_in, TCSANOW, &r);
        const char *on = "\x1b[?1049h\x1b[?25l\x1b[2J";
        if (write(d->tty_out, on, strlen(on)) < 0) {}
    } else {
        d->frames = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (d->frames < 0) { fprintf(stderr, "%s: %s: %s\n", title, path, strerror(errno)); return -1; }
    }
    return 0;
}

/* The room there is to draw in, in pixels: the window; the terminal in
   Braille dots (2x4 a cell) or half-blocks (1x2); --size for frames. */
static void display_size(Display *d, int *w, int *h) {
    if (d->dest == DEST_WINDOW) { SDL_GetWindowSizeInPixels(d->win, w, h); return; }
    if (d->dest == DEST_TERM) {
        struct winsize ws = {0};
        if (ioctl(d->tty_out, TIOCGWINSZ, &ws) < 0 || !ws.ws_col) { ws.ws_col = 80; ws.ws_row = 24; }
        *w = ws.ws_col * (d->halfblock ? 1 : 2);
        *h = ws.ws_row * (d->halfblock ? 2 : 4);
        return;
    }
    *w = *h = d->size;
}

/* Bind where the finished frame goes: w by h pixels, centred. */
static void display_target(Display *d, int w, int h) {
    if (d->dest == DEST_WINDOW) {
        int ww, wh;
        SDL_GetWindowSizeInPixels(d->win, &ww, &wh);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, ww, wh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glViewport((ww - w) / 2, (wh - h) / 2, w, h);
        return;
    }
    if (w != d->fw || h != d->fh) {
        if (d->fbo) { glDeleteFramebuffers(1, &d->fbo); glDeleteTextures(1, &d->tex); }
        glGenTextures(1, &d->tex);
        glBindTexture(GL_TEXTURE_2D, d->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &d->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->tex, 0);
        d->pix = realloc(d->pix, (size_t)w * h * 4);
        d->fw = w;
        d->fh = h;
        if (d->dest == DEST_TERM && write(d->tty_out, "\x1b[2J", 4) < 0) {}
    }
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    glViewport(0, 0, w, h);
}

static void write_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        c += w;
        n -= (size_t)w;
    }
}

/* The frame's pixel at (x, y), y from the top (GL rows run bottom-up). */
static const unsigned char *display_px(Display *d, int x, int y) {
    return d->pix + ((size_t)(d->fh - 1 - y) * d->fw + x) * 4;
}

/* Centre a cw x ch block of cells in the terminal: where its rows start. */
static void term_origin(Display *d, int cw, int ch, int *ox, int *oy) {
    struct winsize ws = {0};
    ioctl(d->tty_out, TIOCGWINSZ, &ws);
    *ox = ws.ws_col > cw ? (ws.ws_col - cw) / 2 : 0;
    *oy = ws.ws_row > ch ? (ws.ws_row - ch) / 2 : 0;
}

static char *term_buf(size_t need) {
    static char *out;
    static size_t cap;
    if (need > cap) { cap = need; out = realloc(out, cap); }
    return out;
}

/* The frame as Braille: each character cell is 2x4 dots, lit where the
   picture is, coloured by its brightest dot. For a beam. */
static void term_braille(Display *d) {
    int cw = d->fw / 2, ch = d->fh / 4, ox, oy;
    term_origin(d, cw, ch, &ox, &oy);
    char *out = term_buf((size_t)ch * ((size_t)cw * 24 + 16) + 16);
    size_t n = 0;
    static const int bit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    for (int cy = 0; cy < ch; cy++) {
        n += (size_t)sprintf(out + n, "\x1b[%d;%dH", oy + cy + 1, ox + 1);
        int lr = -1, lg = -1, lb = -1;
        for (int cx = 0; cx < cw; cx++) {
            int bits = 0, r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 4; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const unsigned char *p = display_px(d, cx * 2 + dx, cy * 4 + dy);
                    if (p[0] * 3 + p[1] * 6 + p[2] > 240) {
                        bits |= bit[dy][dx];
                        if (p[0] > r) r = p[0];
                        if (p[1] > g) g = p[1];
                        if (p[2] > b) b = p[2];
                    }
                }
            if (!bits) { out[n++] = ' '; continue; }
            if (r != lr || g != lg || b != lb) {
                n += (size_t)sprintf(out + n, "\x1b[38;2;%d;%d;%dm", r, g, b);
                lr = r; lg = g; lb = b;
            }
            int c = 0x2800 + bits;                /* U+2800 + dots, as UTF-8 */
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    write_all(d->tty_out, out, n);
}

/* The frame as half-blocks: each cell is two pixels, the upper half
   block in the top one's colour on the bottom one's. For a raster. */
static void term_halfblock(Display *d) {
    int cw = d->fw, ch = d->fh / 2, ox, oy;
    term_origin(d, cw, ch, &ox, &oy);
    char *out = term_buf((size_t)ch * ((size_t)cw * 44 + 16) + 16);
    size_t n = 0;
    for (int cy = 0; cy < ch; cy++) {
        n += (size_t)sprintf(out + n, "\x1b[%d;%dH", oy + cy + 1, ox + 1);
        for (int cx = 0; cx < cw; cx++) {
            const unsigned char *t = display_px(d, cx, cy * 2), *u = display_px(d, cx, cy * 2 + 1);
            n += (size_t)sprintf(out + n, "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm\xe2\x96\x80",
                                 t[0], t[1], t[2], u[0], u[1], u[2]);
        }
        n += (size_t)sprintf(out + n, "\x1b[0m");
    }
    write_all(d->tty_out, out, n);
}

/* Show the finished frame. */
static void display_present(Display *d) {
    if (d->dest == DEST_WINDOW) { SDL_GL_SwapWindow(d->win); return; }
    glReadPixels(0, 0, d->fw, d->fh, GL_RGBA, GL_UNSIGNED_BYTE, d->pix);
    if (d->dest == DEST_TERM) {
        if (d->halfblock) term_halfblock(d);
        else term_braille(d);
    } else {
        static unsigned char *row;
        row = realloc(row, (size_t)d->fw * 3);
        for (int y = 0; y < d->fh; y++) {         /* top row first */
            for (int x = 0; x < d->fw; x++) memcpy(row + x * 3, display_px(d, x, y), 3);
            write_all(d->frames, row, (size_t)d->fw * 3);
        }
        if (d->nopace) return;
    }
    double t = now_s();                       /* headless: keep to the frame rate */
    if (d->next == 0 || d->next < t - 0.5) d->next = t;
    d->next += 1.0 / HEADLESS_FPS;
    sleep_s(d->next - t);
}

/* Keys and the window manager: 1 when asked to quit. */
static int display_poll(Display *d) {
    int quit = display_quit;
    if (d->dest == DEST_WINDOW) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = 1;
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_Q || e.key.key == SDLK_ESCAPE) quit = 1;
                else if (e.key.key == SDLK_F)
                    SDL_SetWindowFullscreen(d->win, !(SDL_GetWindowFlags(d->win) & SDL_WINDOW_FULLSCREEN));
            }
        }
    } else if (d->dest == DEST_TERM) {
        char c;
        while (read(d->tty_in, &c, 1) == 1)
            if (c == 'q' || c == 27) quit = 1;
    }
    return quit;
}

static void display_close(Display *d) {
    if (d->dest == DEST_WINDOW) {
        SDL_GL_DestroyContext(d->ctx);
        SDL_DestroyWindow(d->win);
        SDL_Quit();
        return;
    }
    if (d->dest == DEST_TERM) {
        const char *off = "\x1b[0m\x1b[?25h\x1b[?1049l";
        write_all(d->tty_out, off, strlen(off));
        tcsetattr(d->tty_in, TCSANOW, &d->saved);
    } else close(d->frames);
}

/* ---------------------------------------------------------------- GL bits */

static inline int gl_has(const char *ext) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++)
        if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i), ext)) return 1;
    return 0;
}

static GLuint gl_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        exit(1);
    }
    return s;
}

static GLuint gl_program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, gl_shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, gl_shader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "program: %s\n", log);
        exit(1);
    }
    return p;
}

/* A texture to draw into, and its framebuffer. */
static inline GLuint gl_target(GLuint *tex, int w, int h, GLenum ifmt, GLenum fmt, GLenum type, GLenum filter) {
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, w, h, 0, fmt, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "a %dx%d render target is not supported here\n", w, h);
        exit(1);
    }
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    return fbo;
}

/* One triangle that covers the viewport, uv 0..1 across it. Draw with
   glDrawArrays(GL_TRIANGLES, 0, 3) and any vertex array bound. */
static const char *GL_FULLSCREEN_VS =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
    "  uv = p * 0.5 + 0.5;\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

#endif
