#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <string.h>
#include <stdlib.h>

/* `in` is the capture frames for this period (same frame count and
   channel count as `out`), or NULL when the device is playback-only. */
typedef void (*aither_callback_t)(float*, const float*, unsigned int, void*);

static ma_device g_device;
static aither_callback_t g_callback;
static void* g_user_data;
static int g_init;
static int g_capture;
static ma_context g_context;

static void data_callback(ma_device* dev, void* out, const void* in,
                           ma_uint32 frames) {
    if (g_callback) {
        g_callback((float*)out, g_capture ? (const float*)in : NULL,
                   frames, g_user_data);
    } else {
        memset(out, 0, frames * dev->playback.channels * sizeof(float));
    }
}

/* Returns 1 with audio input (duplex device), 0 playback-only (no
   capture device, or the backend refused duplex), -1 on failure. */
int aither_audio_init(unsigned int sr, unsigned int ch, unsigned int buf,
                       aither_callback_t cb, void* ud) {
    g_callback = cb;
    g_user_data = ud;
    /* AITHER_AUDIO=null: no sound card, the callback still runs in real
       time. For a second engine (tests, a headless box) that must not
       play through the speakers. */
    const char* mode = getenv("AITHER_AUDIO");
    if (mode && strcmp(mode, "null") == 0) {
        ma_backend nullBackend[] = { ma_backend_null };
        if (ma_context_init(nullBackend, 1, NULL, &g_context) != MA_SUCCESS) return -1;
        ma_device_config c = ma_device_config_init(ma_device_type_playback);
        c.playback.format = ma_format_f32;
        c.playback.channels = ch;
        c.sampleRate = sr;
        c.dataCallback = data_callback;
        c.periodSizeInFrames = buf;
        if (ma_device_init(&g_context, &c, &g_device) != MA_SUCCESS) return -1;
        g_capture = 0;
        g_init = 1;
        return 0;
    }
    /* AITHER_INPUT=0: playback only. A duplex device ties the output to
       a capture clock too; a streaming engine that never reads input()
       is better off without that second clock. */
    const char* in = getenv("AITHER_INPUT");
    int wantInput = !(in && strcmp(in, "0") == 0);
    ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = ch;
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = ch;
    cfg.sampleRate = sr;
    cfg.dataCallback = data_callback;
    cfg.periodSizeInFrames = buf;
    if (wantInput && ma_device_init(NULL, &cfg, &g_device) == MA_SUCCESS) {
        g_capture = 1;
        g_init = 1;
        return 1;
    }
    cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = ch;
    cfg.sampleRate = sr;
    cfg.dataCallback = data_callback;
    cfg.periodSizeInFrames = buf;
    if (ma_device_init(NULL, &cfg, &g_device) != MA_SUCCESS) return -1;
    g_capture = 0;
    g_init = 1;
    return 0;
}

int aither_audio_start(void) {
    return g_init && ma_device_start(&g_device) == MA_SUCCESS ? 0 : -1;
}

int aither_audio_stop(void) {
    return g_init && ma_device_stop(&g_device) == MA_SUCCESS ? 0 : -1;
}

void aither_audio_uninit(void) {
    if (g_init) { ma_device_uninit(&g_device); g_init = 0; }
}
