#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <string.h>

typedef void (*aither_callback_t)(float*, unsigned int, void*);

static ma_device g_device;
static aither_callback_t g_callback;
static void* g_user_data;
static int g_init;

static void data_callback(ma_device* dev, void* out, const void* in,
                           ma_uint32 frames) {
    (void)in;
    if (g_callback) {
        g_callback((float*)out, frames, g_user_data);
    } else {
        memset(out, 0, frames * dev->playback.channels * sizeof(float));
    }
}

int aither_audio_init(unsigned int sr, unsigned int ch, unsigned int buf,
                       aither_callback_t cb, void* ud) {
    g_callback = cb;
    g_user_data = ud;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = ch;
    cfg.sampleRate = sr;
    cfg.dataCallback = data_callback;
    cfg.periodSizeInFrames = buf;
    if (ma_device_init(NULL, &cfg, &g_device) != MA_SUCCESS) return -1;
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
