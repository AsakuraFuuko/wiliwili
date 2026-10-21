/*
 * App-owned audio path for the native PS5 title.
 *
 * mpv keeps the video, subtitles, the fallback decoder and the playback timeline;
 * only its audio *output* is disabled (`ao=null`). This file decodes the audio
 * stream itself with libavcodec, resamples to the port format with libswresample
 * and drives the PS5 audio port through sceAudioOut (SDL2 audio is the backup).
 *
 * The sample count fed to the port is the master clock: `wiliwili_audio_clock()`
 * reports the media position of the sample that is audible right now, and the
 * caller keeps mpv's playback rate nudged onto it. That is the same split
 * EvoPlayer uses (see references/EVO-PLAYER-PS5/projects/evoplayer/media/
 * src/evo_audio_out.c): decode thread -> block ring -> blocking output thread.
 *
 * Behaviour contract:
 *   - start() is non-blocking; the network open runs on the decode thread and a
 *     failure is latched for the caller to fall back to mpv's AO.
 *   - pause/resume, seek (any number, any order) and track switch all keep the
 *     ring generation-tagged, so audio from before a seek is never replayed.
 *   - EOF drains the ring, then closes the port: no residual sound.
 *   - stop() silences at once and joins both threads.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <SDL2/SDL.h>

#if defined(PS5_NATIVE_APP)

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

extern void wiliwili_boot_log(const char *message);

/* ---------------------------------------------------------------- device --- */
/* The payload SDK ships no audio headers; these are the verified PS5 entry
 * points (same ABI and constants the patched SDL2 ps5 driver uses). */
#define AUDIO_PORT_MAIN          0
#define AUDIO_FORMAT_S16_STEREO  1
#define AUDIO_USER_ID_SYSTEM     0xFF

extern int32_t sceAudioOutInit(void);
extern int32_t sceAudioOutOpen(int32_t userId, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                               uint32_t param);
extern int32_t sceAudioOutOutput(int32_t handle, const void *p);
extern int32_t sceAudioOutClose(int32_t handle);

#define AUDIO_OUT_RATE     48000
#define AUDIO_BLOCK_FRAMES 1024
#define AUDIO_RING_BLOCKS  32
/* Decode stays a few blocks (≈85 ms) ahead of the port. A deep ring would put
 * the audible position far behind the demuxer, which shows up as a tail of
 * sound after the last frame when the file ends. */
#define AUDIO_DECODE_AHEAD 4
#define AUDIO_CHANNELS     2

enum {
    AUDIO_BACKEND_NONE = 0,
    AUDIO_BACKEND_PS5  = 1,
    AUDIO_BACKEND_SDL  = 2,
};

typedef struct {
    double pts;   /* media seconds at the first frame */
    double span;  /* media seconds covered by `frames` (speed-scaled) */
    int frames;
    int gen;      /* seek/track generation: blocks from an older one are dropped */
    int16_t pcm[AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS];
} AudioBlock;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    pthread_t decode_thread;
    pthread_t output_thread;
    int threads_live;
    volatile int stop;

    /* control (set by the app thread) */
    volatile int paused;
    volatile int failed;
    volatile int seek_pending;
    volatile int seek_gen;
    volatile double seek_target;
    volatile int track_request;
    volatile int volume; /* 0..100 */
    volatile double speed;

    /* playlist: separate DASH audio URLs, or the muxed video URL when empty */
    char **urls;
    int url_count;
    int track;

    /* ring */
    AudioBlock ring[AUDIO_RING_BLOCKS];
    int ring_read;
    int ring_write;
    int ring_count;

    /* decode thread state */
    AVFormatContext *fmt;
    AVCodecContext *dec;
    SwrContext *swr;
    int stream_index;
    double time_base;
    double prod_pts;
    int prod_valid;
    double discard_until;
    int producer_gen;
    int16_t pending[AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS];
    int pending_frames;
    double pending_pts;
    int have_audio;
    double duration;
    volatile int eof_decoded;
    int stream_count;
    /* swr is rebuilt only when the stream layout or the speed changes */
    int swr_key_rate;
    enum AVSampleFormat swr_key_format;
    AVChannelLayout swr_key_layout;
    int swr_key_out_rate;
    int swr_key_valid;
    /* diagnostics */
    const char *codec_name;
    volatile int active;
    int sample_rate;
    int channels;
    int backend;
    int32_t device;
    void *sdl_device;
    uint64_t blocks_out;
    uint64_t decoded_frames;
    uint64_t underruns;
    uint64_t seek_count;
    uint64_t dropout_bytes;

    /* clock: media position of the sample currently being heard */
    int clock_valid;
    int clock_active; /* an output write is in flight */
    double clock_anchor_pts;
    double clock_anchor_span;
    uint64_t clock_anchor_us;
    double clock_frozen_pts;

    /* drift meter (vs. mpv playback-time), updated by the app frame hook */
    int drift_valid;
    double drift_max_ms;
    uint64_t drift_last_log_us;
    uint64_t started_us;
} AudioSession;

static AudioSession g_audio = {
    .mutex            = PTHREAD_MUTEX_INITIALIZER,
    .cond             = PTHREAD_COND_INITIALIZER,
    .track_request    = -1,
    .volume           = 100,
    .speed            = 1.0,
    .seek_target      = 0.0,
    .device           = -1,
    .clock_anchor_pts = 0.0,
};

static void audio_log(const char *format, ...);

static int g_available = -1;
/* sceAudioOutInit() is a one-shot process initialisation: a second call reports
 * an error, which must not be mistaken for "the port refused to open". */
static int g_audio_out_initialized = 0;

static int audio_ps5_init_once(void) {
    if (g_audio_out_initialized) return 0;
    const int32_t rc = sceAudioOutInit();
    if (rc != 0) {
        audio_log("audio: ps5 init rc=%d", (int)rc);
        return -1;
    }
    g_audio_out_initialized = 1;
    return 0;
}

static uint64_t audio_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void audio_log(const char *format, ...) {
    char message[320];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    wiliwili_boot_log(message);
}

static double audio_clamp(double value, double low, double high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

/* ------------------------------------------------------------- clock API --- */
/* Caller holds the mutex. */
static double audio_clock_locked(AudioSession *s) {
    if (!s->clock_valid) return -1.0;
    if (s->paused) return s->clock_frozen_pts;
    double pts = s->clock_anchor_pts;
    const uint64_t now = audio_now_us();
    if (now > s->clock_anchor_us) {
        double advance = (double)(now - s->clock_anchor_us) * 1e-6 * (s->speed > 0.0 ? s->speed : 1.0);
        if (advance > s->clock_anchor_span) advance = s->clock_anchor_span;
        pts += advance;
    }
    return pts;
}

static void audio_clock_begin_block_locked(AudioSession *s, const AudioBlock *block) {
    s->clock_anchor_pts  = block->pts;
    s->clock_anchor_span = block->span;
    s->clock_anchor_us   = audio_now_us();
    s->clock_valid       = 1;
}

/* Decode-thread helper: the clock lives behind the session mutex. */
static double audio_clock_snapshot(AudioSession *s) {
    pthread_mutex_lock(&s->mutex);
    const double value = audio_clock_locked(s);
    pthread_mutex_unlock(&s->mutex);
    return value;
}

/* --------------------------------------------------------------- device ---- */
static int audio_ps5_open(AudioSession *s) {
    if (audio_ps5_init_once() != 0) {
        audio_log("audio: output ps5 init failed");
        return -1;
    }
    int32_t handle = sceAudioOutOpen(AUDIO_USER_ID_SYSTEM, AUDIO_PORT_MAIN, 0, AUDIO_BLOCK_FRAMES, AUDIO_OUT_RATE,
                                     AUDIO_FORMAT_S16_STEREO);
    if (handle < 1) {
        audio_log("audio: output ps5 open failed rc=%d grain=%d", (int)handle, AUDIO_BLOCK_FRAMES);
        return -1;
    }
    s->device  = handle;
    s->backend = AUDIO_BACKEND_PS5;
    return 0;
}

static int audio_sdl_open(AudioSession *s) {
    SDL_AudioSpec want;
    SDL_AudioSpec have;
    memset(&want, 0, sizeof(want));
    want.freq     = AUDIO_OUT_RATE;
    want.format   = AUDIO_S16LSB;
    want.channels = AUDIO_CHANNELS;
    want.samples  = AUDIO_BLOCK_FRAMES;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return -1;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        audio_log("audio: output sdl open failed err=%s", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(dev, 0);
    audio_log("audio: output sdl driver=%s", SDL_GetCurrentAudioDriver() != NULL ? SDL_GetCurrentAudioDriver() : "?");
    s->sdl_device = (void *)(uintptr_t)dev;
    s->backend    = AUDIO_BACKEND_SDL;
    return 0;
}

static int audio_output_open(AudioSession *s) {
    if (s->backend != AUDIO_BACKEND_NONE) return 0;
    if (audio_ps5_open(s) == 0) {
        audio_log("audio: output backend=ps5 handle=%d", (int)s->device);
        return 0;
    }
    if (audio_sdl_open(s) == 0) {
        audio_log("audio: output backend=sdl");
        return 0;
    }
    audio_log("audio: output open failed (ps5+sdl)");
    return -1;
}

static void audio_output_close(AudioSession *s) {
    if (s->backend == AUDIO_BACKEND_PS5 && s->device >= 1) {
        const int32_t rc = sceAudioOutClose(s->device);
        if (rc != 0) audio_log("audio: output close rc=%d", (int)rc);
        s->device = -1;
    } else if (s->backend == AUDIO_BACKEND_SDL && s->sdl_device != NULL) {
        SDL_AudioDeviceID dev = (SDL_AudioDeviceID)(uintptr_t)s->sdl_device;
        SDL_ClearQueuedAudio(dev);
        SDL_CloseAudioDevice(dev);
        s->sdl_device = NULL;
    }
    s->backend = AUDIO_BACKEND_NONE;
}

static int audio_block_bytes(const AudioBlock *block) {
    return block->frames * AUDIO_CHANNELS * (int)sizeof(int16_t);
}

/* Blocking write: sceAudioOutOutput paces on the port, the SDL fallback paces on
 * the queued byte count. */
static void audio_output_write(AudioSession *s, AudioBlock *block) {
    const int bytes = audio_block_bytes(block);
    if (bytes <= 0) return;
    if (s->backend == AUDIO_BACKEND_PS5 && s->device >= 1) {
        sceAudioOutOutput(s->device, block->pcm);
        return;
    }
    if (s->backend == AUDIO_BACKEND_SDL && s->sdl_device != NULL) {
        SDL_AudioDeviceID dev = (SDL_AudioDeviceID)(uintptr_t)s->sdl_device;
        SDL_QueueAudio(dev, block->pcm, (uint32_t)bytes);
        /* Keep at most three blocks queued: ~64 ms of latency, and it gives the
         * queueing call the same blocking character as sceAudioOutOutput. */
        while (!s->stop && SDL_GetQueuedAudioSize(dev) > (uint32_t)(bytes * 3)) usleep(1000);
    }
}

static void audio_apply_volume(AudioBlock *block) {
    const int volume = g_audio.volume;
    if (volume == 100) return;
    const int samples = block->frames * AUDIO_CHANNELS;
    if (volume <= 0) {
        memset(block->pcm, 0, (size_t)samples * sizeof(int16_t));
        return;
    }
    const int scale = volume * 32768 / 100;
    for (int i = 0; i < samples; ++i) {
        const int value = ((int)block->pcm[i] * scale) >> 15;
        block->pcm[i]     = (int16_t)(value > 32767 ? 32767 : (value < -32768 ? -32768 : value));
    }
}

/* --------------------------------------------------------------- demuxer --- */
static int audio_bili_media_url(const char *url) {
    const char *authority = strstr(url, "://");
    if (authority == NULL) return 0;
    authority += 3;
    const char *end = strchr(authority, '/');
    if (end == NULL) end = authority + strlen(authority);
    const char *port = memchr(authority, ':', (size_t)(end - authority));
    if (port != NULL) end = port;
    const size_t host_length = (size_t)(end - authority);
    static const char *const suffixes[] = {"bilivideo.com", "bilivideo.cn", "bilibili.com", "hdslb.com"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        const size_t suffix_length = strlen(suffixes[i]);
        if (host_length == suffix_length && strncmp(authority, suffixes[i], suffix_length) == 0) return 1;
        if (host_length > suffix_length && authority[host_length - suffix_length - 1] == '.' &&
            strncmp(authority + host_length - suffix_length, suffixes[i], suffix_length) == 0)
            return 1;
    }
    return 0;
}

static int audio_json_space(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static int audio_json_string(const char *begin, const char *end, const char *key, char *value, size_t capacity) {
    char needle[64];
    const int needle_length = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needle_length <= 0 || (size_t)needle_length >= sizeof(needle)) return 0;
    const char *cursor = begin;
    while (cursor < end) {
        const char *match = strstr(cursor, needle);
        if (match == NULL || match >= end) return 0;
        cursor = match + needle_length;
        while (cursor < end && audio_json_space(*cursor)) ++cursor;
        if (cursor >= end || *cursor++ != ':') continue;
        while (cursor < end && audio_json_space(*cursor)) ++cursor;
        if (cursor >= end || *cursor++ != '"') continue;
        size_t used = 0;
        while (cursor < end && *cursor != '"') {
            unsigned char decoded;
            if (*cursor == '\\') {
                if (++cursor >= end) return 0;
                if (*cursor == '"' || *cursor == '\\' || *cursor == '/')
                    decoded = (unsigned char)*cursor;
                else
                    return 0;
            } else {
                decoded = (unsigned char)*cursor;
            }
            if (decoded == '\r' || decoded == '\n' || used + 1 >= capacity) return 0;
            value[used++] = (char)decoded;
            ++cursor;
        }
        if (cursor >= end || *cursor != '"') return 0;
        value[used] = '\0';
        return 1;
    }
    return 0;
}

static int audio_load_bili_cookie(char *cookie, size_t capacity) {
    cookie[0] = '\0';
    int fd = open("/download0/wiliwili/config/wiliwili_config.json", O_RDONLY, 0);
    if (fd < 0) return 0;
    char json[16384];
    const long size = read(fd, json, sizeof(json) - 1);
    close(fd);
    if (size <= 0 || size >= (long)sizeof(json)) return 0;
    json[size]        = '\0';
    const char *key  = strstr(json, "\"cookie\"");
    if (key == NULL) return 0;
    const char *object = strchr(key, '{');
    if (object == NULL) return 0;
    const char *object_end = strchr(object + 1, '}');
    if (object_end == NULL) return 0;
    static const char *const names[] = {"SESSDATA", "bili_jct", "DedeUserID", "DedeUserID__ckMd5",
                                        "_uuid", "buvid3", "sid"};
    size_t used  = 0;
    int fields   = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char value[512];
        if (!audio_json_string(object + 1, object_end, names[i], value, sizeof(value)) || value[0] == '\0')
            continue;
        const int written = snprintf(cookie + used, capacity - used, "%s%s=%s", fields ? "; " : "", names[i], value);
        if (written < 0 || (size_t)written >= capacity - used) {
            cookie[0] = '\0';
            return -1;
        }
        used += (size_t)written;
        ++fields;
    }
    return fields;
}

static int audio_interrupt(void *opaque) {
    AudioSession *s = (AudioSession *)opaque;
    return s->stop || s->seek_pending || s->track_request >= 0;
}

static int audio_url_is_network(const char *url) {
    return url != NULL && (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

/* Pick the audio stream for `track`: the URL index selects the file, and a
 * muxed file uses `track` as the ordinal among its audio streams. */
static int audio_pick_stream(AVFormatContext *fmt, int track) {
    int ordinal = 0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (ordinal == track) return (int)i;
        ++ordinal;
    }
    if (ordinal == 0) return -1;
    return av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
}

static int audio_stream_count(const AVFormatContext *fmt) {
    int count = 0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ++count;
    return count;
}

static void audio_close_input(AudioSession *s) {
    if (s->swr) swr_free(&s->swr);
    if (s->swr_key_valid) {
        av_channel_layout_uninit(&s->swr_key_layout);
        s->swr_key_valid = 0;
    }
    if (s->dec) avcodec_free_context(&s->dec);
    if (s->fmt) avformat_close_input(&s->fmt);
    s->stream_index = -1;
    s->have_audio   = 0;
}

static int audio_swr_configure(AudioSession *s, const AVFrame *frame) {
    const int out_rate = (int)lrint((double)AUDIO_OUT_RATE * audio_clamp(s->speed, 0.25, 4.0));
    AVChannelLayout in_layout;
    if (frame->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&in_layout, &frame->ch_layout);
    } else {
        av_channel_layout_default(&in_layout, 2);
    }
    /* Every audio frame goes through here (~43/s for AAC); rebuilding the
     * resampler each time would churn allocations for no reason. */
    if (s->swr != NULL && s->swr_key_valid && s->swr_key_rate == frame->sample_rate &&
        s->swr_key_format == frame->format && s->swr_key_out_rate == out_rate &&
        av_channel_layout_compare(&s->swr_key_layout, &in_layout) == 0) {
        av_channel_layout_uninit(&in_layout);
        return 0;
    }
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext *fresh          = NULL;
    const int rc = swr_alloc_set_opts2(&fresh, &out_layout, AV_SAMPLE_FMT_S16, out_rate, &in_layout,
                                      (enum AVSampleFormat)frame->format, frame->sample_rate, 0, NULL);
    if (rc < 0 || fresh == NULL) {
        audio_log("audio: swr config failed rc=%d", rc);
        if (fresh) swr_free(&fresh);
        av_channel_layout_uninit(&in_layout);
        return -1;
    }
    if (swr_init(fresh) < 0) {
        audio_log("audio: swr init failed");
        swr_free(&fresh);
        av_channel_layout_uninit(&in_layout);
        return -1;
    }
    if (s->swr) swr_free(&s->swr);
    if (s->swr_key_valid) av_channel_layout_uninit(&s->swr_key_layout);
    s->swr              = fresh;
    s->swr_key_rate     = frame->sample_rate;
    s->swr_key_format   = (enum AVSampleFormat)frame->format;
    s->swr_key_out_rate = out_rate;
    av_channel_layout_copy(&s->swr_key_layout, &in_layout);
    s->swr_key_valid = 1;
    av_channel_layout_uninit(&in_layout);
    if (s->sample_rate != frame->sample_rate || s->channels != frame->ch_layout.nb_channels) {
        s->sample_rate = frame->sample_rate;
        s->channels    = frame->ch_layout.nb_channels;
    }
    return 0;
}

/* Queue a finished block. Waits for ring space; the generation check lets a
 * seek discard blocks that were produced for the previous position. */
static void audio_push_block(AudioSession *s, AudioBlock *block) {
    pthread_mutex_lock(&s->mutex);
    while (!s->stop && s->ring_count >= AUDIO_DECODE_AHEAD) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 20 * 1000 * 1000;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&s->cond, &s->mutex, &deadline);
    }
    if (!s->stop && block->gen == s->seek_gen) {
        s->ring[s->ring_write] = *block;
        s->ring_write          = (s->ring_write + 1) % AUDIO_RING_BLOCKS;
        ++s->ring_count;
        pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->mutex);
}

/* Append converted S16 stereo frames to the pending block, splitting on block
 * boundaries. Media starts at *pts* and advances by 1/(48000*speed) per frame. */
static void audio_append_samples(AudioSession *s, const int16_t *samples, int frames, double *pts) {
    const double speed         = s->speed > 0.0 ? s->speed : 1.0;
    const double media_per_frame = 1.0 / ((double)AUDIO_OUT_RATE * speed);
    int index                    = 0;
    while (index < frames) {
        const int space = AUDIO_BLOCK_FRAMES - s->pending_frames;
        int run         = frames - index;
        if (run > space) run = space;
        if (s->pending_frames == 0) s->pending_pts = *pts;
        memcpy(&s->pending[(size_t)s->pending_frames * AUDIO_CHANNELS],
               &samples[(size_t)index * AUDIO_CHANNELS], (size_t)run * AUDIO_CHANNELS * sizeof(int16_t));
        s->pending_frames += run;
        index += run;
        *pts += (double)run * media_per_frame;
        s->decoded_frames += (uint64_t)run;
        if (s->pending_frames >= AUDIO_BLOCK_FRAMES) {
            AudioBlock block;
            block.pts    = s->pending_pts;
            block.frames = AUDIO_BLOCK_FRAMES;
            block.span   = (double)AUDIO_BLOCK_FRAMES * media_per_frame;
            block.gen    = s->producer_gen;
            memcpy(block.pcm, s->pending, sizeof(block.pcm));
            s->pending_frames = 0;
            audio_push_block(s, &block);
        }
    }
}

static void audio_decode_frame_to_ring(AudioSession *s, AVFrame *frame) {
    if (frame->nb_samples <= 0) return;
    double frame_pts;
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        frame_pts = (double)frame->best_effort_timestamp * s->time_base;
    } else if (s->prod_valid) {
        frame_pts = s->prod_pts;
    } else {
        frame_pts = 0.0;
    }
    /* The sample counter is the clock; decoded timestamps only re-anchor it. */
    if (!s->prod_valid) {
        s->prod_pts   = frame_pts;
        s->prod_valid = 1;
    } else if (fabs(frame_pts - s->prod_pts) > 0.25) {
        s->prod_pts = frame_pts;
    }
    if (audio_swr_configure(s, frame) != 0) {
        s->failed = 1;
        return;
    }
    const double speed           = s->speed > 0.0 ? s->speed : 1.0;
    const double media_per_frame = 1.0 / ((double)AUDIO_OUT_RATE * speed);
    const int in_rate            = frame->sample_rate > 0 ? frame->sample_rate : AUDIO_OUT_RATE;
    const int out_rate           = (int)lrint((double)AUDIO_OUT_RATE * audio_clamp(speed, 0.25, 4.0));
    const int delay              = (int)av_rescale_rnd(swr_get_delay(s->swr, in_rate), out_rate, in_rate, AV_ROUND_UP);
    const int capacity           = (int)av_rescale_rnd(frame->nb_samples + delay, out_rate, in_rate, AV_ROUND_UP) + 64;
    if (capacity <= 0) return;
    static int16_t *scratch;
    static int scratch_frames;
    if (capacity > scratch_frames) {
        int16_t *grown = (int16_t *)realloc(scratch, (size_t)capacity * AUDIO_CHANNELS * sizeof(int16_t));
        if (grown == NULL) {
            s->failed = 1;
            return;
        }
        scratch        = grown;
        scratch_frames = capacity;
    }
    uint8_t *planes[1] = {(uint8_t *)scratch};
    const int converted =
        swr_convert(s->swr, planes, capacity, (const uint8_t *const *)frame->extended_data, frame->nb_samples);
    if (converted <= 0) return;
    double pts = s->prod_pts;
    int skip   = 0;
    if (s->discard_until >= 0.0) {
        const double end = pts + (double)converted * media_per_frame;
        if (end <= s->discard_until) {
            s->prod_pts = end;
            return;
        }
        if (pts < s->discard_until)
            skip = (int)ceil((s->discard_until - pts) * (double)AUDIO_OUT_RATE * speed) - 1;
        if (skip < 0) skip = 0;
        if (skip > converted) skip = converted;
        s->discard_until = -1.0;
    }
    audio_append_samples(s, scratch + (size_t)skip * AUDIO_CHANNELS, converted - skip, &pts);
    s->prod_pts = pts;
}

static void audio_drain_decoder(AudioSession *s) {
    AVFrame *frame = av_frame_alloc();
    if (frame == NULL) return;
    while (!s->stop) {
        const int rc = avcodec_receive_frame(s->dec, frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) break;
        audio_decode_frame_to_ring(s, frame);
        av_frame_unref(frame);
    }
    av_frame_free(&frame);
}

static int audio_open_input(AudioSession *s, double start_seconds, int seek_only) {
    const char *url = NULL;
    if (s->url_count > 0 && s->track >= 0 && s->track < s->url_count)
        url = s->urls[s->track];
    else if (s->url_count > 0)
        url = s->urls[0];
    else
        url = s->urls != NULL ? s->urls[0] : NULL;
    if (url == NULL || url[0] == '\0') return -1;

    if (!seek_only || s->fmt == NULL) {
        audio_close_input(s);
        AVFormatContext *fmt = avformat_alloc_context();
        if (fmt == NULL) return -1;
        fmt->interrupt_callback.callback = audio_interrupt;
        fmt->interrupt_callback.opaque   = s;
        AVDictionary *options = NULL;
        av_dict_set(&options, "rw_timeout", "5000000", 0);
        av_dict_set(&options, "timeout", "5000000", 0);
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", "1", 0);
        av_dict_set(&options, "reconnect_on_network_error", "1", 0);
        av_dict_set(&options, "multiple_requests", "1", 0);
        char cookie[2048];
        const int bili_url     = audio_bili_media_url(url);
        const int cookie_fields = bili_url ? audio_load_bili_cookie(cookie, sizeof(cookie)) : 0;
        char headers[4096];
        const int header_length = snprintf(headers, sizeof(headers),
                                           "Referer: https://www.bilibili.com\r\nUser-Agent: Mozilla/5.0\r\n%s%s\r\n",
                                           cookie_fields > 0 ? "Cookie: " : "", cookie_fields > 0 ? cookie : "");
        if (header_length > 0 && (size_t)header_length < sizeof(headers))
            av_dict_set(&options, "headers", headers, 0);
        int rc = avformat_open_input(&fmt, url, NULL, &options);
        av_dict_free(&options);
        if (rc < 0) {
            audio_log("audio: open-input failed rc=%d url=%.64s", rc, url);
            avformat_free_context(fmt);
            return -1;
        }
        rc = avformat_find_stream_info(fmt, NULL);
        if (rc < 0) {
            audio_log("audio: find-stream-info failed rc=%d", rc);
            avformat_close_input(&fmt);
            return -1;
        }
        const int stream_index = audio_pick_stream(fmt, s->url_count > 0 ? 0 : s->track);
        if (stream_index < 0) {
            audio_log("audio: no audio stream");
            avformat_close_input(&fmt);
            return -1;
        }
        AVStream *stream = fmt->streams[stream_index];
        const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == NULL) {
            audio_log("audio: no decoder id=%d", stream->codecpar->codec_id);
            avformat_close_input(&fmt);
            return -1;
        }
        AVCodecContext *dec = avcodec_alloc_context3(codec);
        if (dec != NULL) {
            /* Opus pre-skip is dropped by libavcodec only when it can rebase
             * timestamps, which needs the packet timebase (EVO hit the same). */
            dec->pkt_timebase = stream->time_base;
        }
        if (dec == NULL || avcodec_parameters_to_context(dec, stream->codecpar) < 0 ||
            avcodec_open2(dec, codec, NULL) < 0) {
            audio_log("audio: decoder open failed name=%s", codec->name);
            if (dec) avcodec_free_context(&dec);
            avformat_close_input(&fmt);
            return -1;
        }
        s->fmt          = fmt;
        s->dec          = dec;
        s->stream_index = stream_index;
        s->time_base    = av_q2d(stream->time_base);
        s->duration     = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0.0;
        s->codec_name   = codec->name;
        s->sample_rate  = stream->codecpar->sample_rate;
        s->channels     = stream->codecpar->ch_layout.nb_channels;
        s->stream_count = audio_stream_count(fmt);
        audio_log("audio: demux ready codec=%s rate=%d ch=%d tb=%.6f duration=%.3f streams=%d",
                  codec->name, s->sample_rate, s->channels, s->time_base, s->duration, s->stream_count);
    }

    if (start_seconds > 0.0 && s->fmt != NULL && s->stream_index >= 0) {
        const int64_t ts = (int64_t)llround(start_seconds / s->time_base);
        const int rc     = avformat_seek_file(s->fmt, s->stream_index, INT64_MIN, ts, INT64_MAX, 0);
        if (rc < 0) {
            const int rc2 = av_seek_frame(s->fmt, s->stream_index, ts, AVSEEK_FLAG_BACKWARD);
            if (rc2 < 0) {
                audio_log("audio: seek failed rc=%d/%d target=%.3f", rc, rc2, start_seconds);
                return -1;
            }
        }
        avcodec_flush_buffers(s->dec);
        s->swr_key_valid = 0;
    }
    s->prod_pts      = start_seconds > 0.0 ? start_seconds : 0.0;
    s->prod_valid    = 1;
    s->discard_until = start_seconds > 0.0 ? start_seconds : -1.0;
    s->eof_decoded   = 0;
    s->have_audio    = 1;
    return 0;
}

/* ---------------------------------------------------------- decode thread -- */
static void audio_mark_failed(AudioSession *s, const char *reason, int rc) {
    if (!s->failed) audio_log("audio: failed reason=%s rc=%d", reason, rc);
    s->failed = 1;
    pthread_cond_broadcast(&s->cond);
}

static void *audio_decode_main(void *arg) {
    AudioSession *s = (AudioSession *)arg;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame   = av_frame_alloc();
    if (packet == NULL || frame == NULL) {
        audio_mark_failed(s, "alloc", -1);
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        return NULL;
    }

    double target = s->seek_target;
    if (audio_open_input(s, target, 0) != 0) {
        audio_mark_failed(s, "open", -1);
        av_packet_free(&packet);
        av_frame_free(&frame);
        return NULL;
    }
    audio_log("audio: start target=%.3f url=%.64s", target,
              s->url_count > 0 ? (s->urls[s->track] ? s->urls[s->track] : "?") : (s->urls && s->urls[0] ? s->urls[0] : "?"));

    int read_errors = 0;
    while (!s->stop) {
        if (s->track_request >= 0) {
            const int request = s->track_request;
            s->track_request  = -1;
            const double now  = audio_now_us() > s->started_us ? audio_clock_snapshot(s) : s->seek_target;
            const double at   = now >= 0.0 ? now : 0.0;
            audio_log("audio: track switch %d -> %d at %.3f", s->track, request, at);
            const int previous = s->track;
            s->track           = request;
            if (audio_open_input(s, at, 0) != 0) {
                s->track = previous;
                if (audio_open_input(s, at, 0) != 0) {
                    audio_mark_failed(s, "track-switch", -1);
                    break;
                }
            }
            pthread_mutex_lock(&s->mutex);
            s->producer_gen = s->seek_gen;
            s->ring_count   = 0;
            s->ring_read    = 0;
            s->ring_write   = 0;
            s->clock_valid  = 0;
            pthread_cond_broadcast(&s->cond);
            pthread_mutex_unlock(&s->mutex);
            s->pending_frames = 0;
            continue;
        }
        if (s->seek_pending) {
            const double seek_to = s->seek_target;
            s->seek_pending      = 0;
            ++s->seek_count;
            pthread_mutex_lock(&s->mutex);
            s->producer_gen = s->seek_gen;
            s->ring_count   = 0;
            s->ring_read    = 0;
            s->ring_write   = 0;
            s->clock_valid  = 0;
            pthread_cond_broadcast(&s->cond);
            pthread_mutex_unlock(&s->mutex);
            s->pending_frames = 0;
            s->prod_pts       = seek_to;
            s->prod_valid     = 1;
            s->discard_until  = seek_to;
            s->eof_decoded    = 0;
            if (s->fmt != NULL) avformat_flush(s->fmt);
            const int64_t ts = s->fmt != NULL && s->stream_index >= 0 ? (int64_t)llround(seek_to / s->time_base) : 0;
            int rc = s->fmt != NULL ? avformat_seek_file(s->fmt, s->stream_index, INT64_MIN, ts, INT64_MAX, 0) : -1;
            if (rc < 0 && s->fmt != NULL)
                rc = av_seek_frame(s->fmt, s->stream_index, ts, AVSEEK_FLAG_BACKWARD);
            if (s->dec) avcodec_flush_buffers(s->dec);
            s->swr_key_valid = 0;
            if (rc >= 0) {
                audio_log("audio: seek target=%.3f gen=%d", seek_to, s->seek_gen);
            } else {
                audio_log("audio: seek fallback reopen target=%.3f rc=%d", seek_to, rc);
                if (audio_open_input(s, seek_to, 0) != 0) {
                    audio_mark_failed(s, "seek", rc);
                    break;
                }
            }
            continue;
        }
        if (s->paused) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 10 * 1000 * 1000;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }
            pthread_mutex_lock(&s->mutex);
            pthread_cond_timedwait(&s->cond, &s->mutex, &deadline);
            pthread_mutex_unlock(&s->mutex);
            continue;
        }
        if (s->eof_decoded) {
            usleep(10000);
            continue;
        }
        av_packet_unref(packet);
        const int rc = av_read_frame(s->fmt, packet);
        if (rc < 0) {
            if (s->stop || s->seek_pending || s->track_request >= 0) continue;
            if (rc == AVERROR_EOF) {
                audio_drain_decoder(s);
                pthread_mutex_lock(&s->mutex);
                /* Flush the partial block so the tail is not lost. */
                if (s->pending_frames > 0) {
                    AudioBlock block;
                    const double speed = s->speed > 0.0 ? s->speed : 1.0;
                    block.pts    = s->pending_pts;
                    block.frames = s->pending_frames;
                    block.span   = (double)s->pending_frames / ((double)AUDIO_OUT_RATE * speed);
                    block.gen    = s->producer_gen;
                    memcpy(block.pcm, s->pending, (size_t)block.frames * AUDIO_CHANNELS * sizeof(int16_t));
                    if (s->ring_count < AUDIO_RING_BLOCKS && block.gen == s->seek_gen) {
                        s->ring[s->ring_write] = block;
                        s->ring_write          = (s->ring_write + 1) % AUDIO_RING_BLOCKS;
                        ++s->ring_count;
                    }
                    s->pending_frames = 0;
                }
                s->eof_decoded = 1;
                pthread_cond_broadcast(&s->cond);
                pthread_mutex_unlock(&s->mutex);
                audio_log("audio: eof decoded_frames=%llu", (unsigned long long)s->decoded_frames);
                continue;
            }
            ++read_errors;
            audio_log("audio: read error rc=%d count=%d", rc, read_errors);
            if (read_errors >= 3) {
                audio_mark_failed(s, "read", rc);
                break;
            }
            usleep(200000);
            const double reopen_at = audio_clock_snapshot(s);
            if (audio_open_input(s, reopen_at > 0.0 ? reopen_at : 0.0, 1) != 0) {
                audio_mark_failed(s, "read-reopen", rc);
                break;
            }
            continue;
        }
        read_errors = 0;
        if (packet->stream_index != s->stream_index) {
            av_packet_unref(packet);
            continue;
        }
        int send_rc = avcodec_send_packet(s->dec, packet);
        if (send_rc == AVERROR(EAGAIN)) {
            /* The decoder still holds a frame: drain one, then hand the packet
             * over instead of dropping it. */
            while (!s->stop) {
                const int recv_rc = avcodec_receive_frame(s->dec, frame);
                if (recv_rc == AVERROR(EAGAIN) || recv_rc == AVERROR_EOF) break;
                if (recv_rc < 0) break;
                audio_decode_frame_to_ring(s, frame);
                av_frame_unref(frame);
                break;
            }
            send_rc = avcodec_send_packet(s->dec, packet);
        }
        av_packet_unref(packet);
        if (send_rc < 0 && send_rc != AVERROR(EAGAIN)) {
            audio_log("audio: send-packet rc=%d", send_rc);
            continue;
        }
        while (!s->stop) {
            const int recv_rc = avcodec_receive_frame(s->dec, frame);
            if (recv_rc == AVERROR(EAGAIN) || recv_rc == AVERROR_EOF) break;
            if (recv_rc < 0) break;
            audio_decode_frame_to_ring(s, frame);
            av_frame_unref(frame);
        }
        if (s->failed) break;
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    return NULL;
}

/* ---------------------------------------------------------- output thread -- */
static void *audio_output_main(void *arg) {
    AudioSession *s = (AudioSession *)arg;
    AudioBlock block;
    while (!s->stop) {
        int have_block = 0;
        pthread_mutex_lock(&s->mutex);
        for (;;) {
            if (s->stop) break;
            if (s->paused || s->seek_pending) {
                struct timespec deadline;
                clock_gettime(CLOCK_REALTIME, &deadline);
                deadline.tv_nsec += 10 * 1000 * 1000;
                if (deadline.tv_nsec >= 1000000000L) {
                    deadline.tv_sec += 1;
                    deadline.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&s->cond, &s->mutex, &deadline);
                continue;
            }
            if (s->ring_count > 0) {
                /* Wait for a small cushion before the first write; after that a
                 * single block is enough to stay ahead of the port. */
                if (s->blocks_out == 0 && s->ring_count < 3 && !s->eof_decoded) {
                    struct timespec deadline;
                    clock_gettime(CLOCK_REALTIME, &deadline);
                    deadline.tv_nsec += 10 * 1000 * 1000;
                    if (deadline.tv_nsec >= 1000000000L) {
                        deadline.tv_sec += 1;
                        deadline.tv_nsec -= 1000000000L;
                    }
                    pthread_cond_timedwait(&s->cond, &s->mutex, &deadline);
                    continue;
                }
                block      = s->ring[s->ring_read];
                s->ring_read = (s->ring_read + 1) % AUDIO_RING_BLOCKS;
                --s->ring_count;
                pthread_cond_broadcast(&s->cond);
                if (block.gen != s->seek_gen) continue;
                audio_clock_begin_block_locked(s, &block);
                have_block = 1;
                break;
            }
            if (s->eof_decoded) {
                /* Last sample played: silence the port and leave it closed so
                 * nothing can ring out after the picture parks. */
                pthread_mutex_unlock(&s->mutex);
                audio_output_close(s);
                audio_log("audio: output drained blocks=%llu underruns=%llu",
                          (unsigned long long)s->blocks_out, (unsigned long long)s->underruns);
                pthread_mutex_lock(&s->mutex);
                while (!s->stop && (s->eof_decoded || s->paused) && s->ring_count == 0) {
                    struct timespec deadline;
                    clock_gettime(CLOCK_REALTIME, &deadline);
                    deadline.tv_nsec += 20 * 1000 * 1000;
                    if (deadline.tv_nsec >= 1000000000L) {
                        deadline.tv_sec += 1;
                        deadline.tv_nsec -= 1000000000L;
                    }
                    pthread_cond_timedwait(&s->cond, &s->mutex, &deadline);
                }
                pthread_mutex_unlock(&s->mutex);
                continue;
            }
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 10 * 1000 * 1000;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&s->cond, &s->mutex, &deadline);
        }
        pthread_mutex_unlock(&s->mutex);
        if (!have_block || s->stop) continue;
        if (audio_output_open(s) != 0) {
            audio_mark_failed(s, "output-open", -1);
            break;
        }
        audio_apply_volume(&block);
        audio_output_write(s, &block);
        ++s->blocks_out;
    }
    return NULL;
}

/* ------------------------------------------------------------- public API -- */
int wiliwili_audio_available(void) {
    if (g_available >= 0) return g_available;
    if (audio_ps5_init_once() != 0) {
        audio_log("audio: probe sceAudioOutInit failed");
        g_available = 0;
        return 0;
    }
    const int32_t handle = sceAudioOutOpen(AUDIO_USER_ID_SYSTEM, AUDIO_PORT_MAIN, 0, 512, AUDIO_OUT_RATE,
                                           AUDIO_FORMAT_S16_STEREO);
    if (handle < 1) {
        audio_log("audio: probe port open failed rc=%d", (int)handle);
        g_available = 0;
        return 0;
    }
    sceAudioOutClose(handle);
    g_available = 1;
    audio_log("audio: probe ps5 audio available");
    return 1;
}

static void audio_free_urls(char **urls, int count) {
    if (urls == NULL) return;
    for (int i = 0; i < count; ++i) free(urls[i]);
    free(urls);
}

void wiliwili_audio_stop(void) {
    AudioSession *s = &g_audio;
    pthread_mutex_lock(&s->mutex);
    const int live = s->threads_live;
    s->stop         = 1;
    s->paused       = 1;
    s->failed       = 0;
    s->seek_pending = 0;
    s->track_request = -1;
    s->ring_count   = 0;
    s->clock_valid  = 0;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    audio_output_close(s);
    if (live) {
        if (s->decode_thread != 0) pthread_join(s->decode_thread, NULL);
        if (s->output_thread != 0) pthread_join(s->output_thread, NULL);
    }
    /* A port opened by the output thread while teardown began is closed here. */
    audio_output_close(s);
    s->decode_thread = 0;
    s->output_thread = 0;
    s->threads_live  = 0;
    s->stop          = 0;
    audio_free_urls(s->urls, s->url_count);
    s->urls      = NULL;
    s->url_count = 0;
    s->track     = 0;
    s->eof_decoded = 0;
    s->pending_frames = 0;
    s->have_audio = 0;
    s->codec_name = NULL;
    s->blocks_out = 0;
    s->underruns  = 0;
    s->seek_count = 0;
    s->drift_valid = 0;
    s->drift_max_ms = 0.0;
    s->drift_last_log_us = 0;
    audio_close_input(s);
    s->active = 0;
}

int wiliwili_audio_start(const char *video_url, const char *const *audio_urls, int audio_count,
                         double start_seconds) {
    if (!wiliwili_audio_available()) return 0;
    if (!isfinite(start_seconds) || start_seconds < 0.0) start_seconds = 0.0;
    wiliwili_audio_stop();
    AudioSession *s = &g_audio;
    s->urls    = (char **)calloc((size_t)(audio_count > 0 ? audio_count : 1), sizeof(char *));
    if (s->urls == NULL) return 0;
    if (audio_count > 0 && audio_urls != NULL) {
        s->url_count = audio_count;
        for (int i = 0; i < audio_count; ++i) s->urls[i] = strdup(audio_urls[i] != NULL ? audio_urls[i] : "");
    } else {
        s->url_count = 1;
        s->urls[0]   = strdup(video_url != NULL ? video_url : "");
    }
    if (s->urls[0] == NULL || s->urls[0][0] == '\0') {
        audio_log("audio: start with empty url");
        audio_free_urls(s->urls, s->url_count);
        s->urls      = NULL;
        s->url_count = 0;
        return 0;
    }
    s->track         = 0;
    s->paused        = 1;
    s->failed        = 0;
    s->stop          = 0;
    s->seek_pending  = 0;
    s->seek_gen      = 0;
    s->seek_target   = start_seconds;
    s->track_request = -1;
    s->speed         = 1.0;
    s->volume        = 100;
    s->device        = -1;
    s->sdl_device    = NULL;
    s->backend       = AUDIO_BACKEND_NONE;
    s->blocks_out    = 0;
    s->decoded_frames = 0;
    s->underruns     = 0;
    s->eof_decoded   = 0;
    s->pending_frames = 0;
    s->clock_valid   = 0;
    s->drift_valid   = 0;
    s->drift_max_ms  = 0.0;
    s->drift_last_log_us = 0;
    s->started_us    = audio_now_us();
    s->active        = 1;
    s->threads_live  = 1;
    if (pthread_create(&s->decode_thread, NULL, audio_decode_main, s) != 0) {
        s->threads_live = 0;
        s->active       = 0;
        s->failed       = 1;
        audio_log("audio: decode thread create failed");
        return 0;
    }
    if (pthread_create(&s->output_thread, NULL, audio_output_main, s) != 0) {
        s->stop = 1;
        pthread_cond_broadcast(&s->cond);
        pthread_join(s->decode_thread, NULL);
        s->decode_thread = 0;
        s->threads_live  = 0;
        s->active        = 0;
        s->failed        = 1;
        audio_log("audio: output thread create failed");
        return 0;
    }
    if (audio_output_open(s) != 0) {
        s->stop = 1;
        pthread_cond_broadcast(&s->cond);
        pthread_join(s->decode_thread, NULL);
        pthread_join(s->output_thread, NULL);
        s->decode_thread = 0;
        s->output_thread = 0;
        s->threads_live  = 0;
        s->active        = 0;
        audio_output_close(s);
        audio_log("audio: start aborted (output port refused)");
        return 0;
    }
    audio_log("audio: start tracks=%d start=%.3f", s->url_count, start_seconds);
    return 1;
}

void wiliwili_audio_pause(int paused) {
    AudioSession *s = &g_audio;
    if (!s->active) return;
    pthread_mutex_lock(&s->mutex);
    if (paused && !s->paused) s->clock_frozen_pts = audio_clock_locked(s);
    if (!paused && s->paused) {
        s->clock_anchor_pts = s->clock_frozen_pts;
        s->clock_anchor_us  = audio_now_us();
    }
    s->paused = paused != 0;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mutex);
}

int wiliwili_audio_seek(double seconds) {
    AudioSession *s = &g_audio;
    if (!s->active || !isfinite(seconds)) return 0;
    if (seconds < 0.0) seconds = 0.0;
    pthread_mutex_lock(&s->mutex);
    s->seek_target  = seconds;
    s->seek_pending = 1;
    ++s->seek_gen;
    s->ring_count = 0;
    s->ring_read  = 0;
    s->ring_write = 0;
    s->clock_valid = 0;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    return 1;
}

void wiliwili_audio_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    g_audio.volume = volume;
}

void wiliwili_audio_set_speed(double speed) {
    if (!isfinite(speed) || speed < 0.25 || speed > 4.0) return;
    g_audio.speed = speed;
}

int wiliwili_audio_is_active(void) { return g_audio.active; }

int wiliwili_audio_take_failure(void) {
    if (!g_audio.active) return 0;
    if (!g_audio.failed) return 0;
    g_audio.failed = 0;
    return 1;
}

double wiliwili_audio_clock(void) {
    AudioSession *s = &g_audio;
    if (!s->active) return -1.0;
    pthread_mutex_lock(&s->mutex);
    const double clock = audio_clock_locked(s);
    pthread_mutex_unlock(&s->mutex);
    return clock;
}

double wiliwili_audio_duration(void) { return g_audio.duration; }

int wiliwili_audio_is_eof(void) { return g_audio.eof_decoded; }

int wiliwili_audio_track_count(void) {
    if (!g_audio.active) return 0;
    /* DASH: one URL per mirror is one selectable track. Muxed file: the audio
     * streams are the tracks. */
    if (g_audio.url_count > 1) return g_audio.url_count;
    return g_audio.stream_count > 0 ? g_audio.stream_count : 1;
}

int wiliwili_audio_track(void) { return g_audio.track; }

int wiliwili_audio_switch_track(int index) {
    AudioSession *s = &g_audio;
    if (!s->active) return 0;
    if (index < 0 || index >= wiliwili_audio_track_count() || index == s->track) return 0;
    s->track_request = index;
    pthread_cond_broadcast(&s->cond);
    return 1;
}

/* Called once per rendered frame by the player. Keeps the drift receipt and the
 * port clock honest; the sync *correction* lives in MPVCore because only it can
 * nudge mpv's playback rate. */
void wiliwili_audio_observe(double mpv_time, double speed, int paused, int seeking) {
    AudioSession *s = &g_audio;
    if (!s->active) return;
    if (speed > 0.0) s->speed = speed;
    const double clock = wiliwili_audio_clock();
    if (clock < 0.0 || !isfinite(mpv_time) || mpv_time <= 0.0 || paused || seeking) return;
    const double drift_ms = (clock - mpv_time) * 1000.0;
    if (fabs(drift_ms) > s->drift_max_ms) s->drift_max_ms = fabs(drift_ms);
    const uint64_t now = audio_now_us();
    if (s->drift_last_log_us == 0 || now - s->drift_last_log_us >= 2000000ull) {
        s->drift_last_log_us = now;
        audio_log("audio: clock=%.3f mpv=%.3f drift_ms=%.1f max_ms=%.1f blocks=%llu underrun=%llu track=%d speed=%.2f",
                  clock, mpv_time, drift_ms, s->drift_max_ms, (unsigned long long)s->blocks_out,
                  (unsigned long long)s->underruns, s->track, s->speed);
    }
}

void wiliwili_audio_summary(void) {
    AudioSession *s = &g_audio;
    if (!s->active) return;
    const double elapsed = s->started_us != 0 ? (double)(audio_now_us() - s->started_us) / 1e6 : 0.0;
    audio_log("audio: summary elapsed_s=%.1f blocks=%llu decoded=%llu underrun=%llu seeks=%llu drift_max_ms=%.1f",
              elapsed, (unsigned long long)s->blocks_out, (unsigned long long)s->decoded_frames,
              (unsigned long long)s->underruns, (unsigned long long)s->seek_count, s->drift_max_ms);
}

#else /* !PS5_NATIVE_APP */

int wiliwili_audio_available(void) { return 0; }
void wiliwili_audio_stop(void) {}
int wiliwili_audio_start(const char *video_url, const char *const *audio_urls, int audio_count,
                         double start_seconds) {
    (void)video_url;
    (void)audio_urls;
    (void)audio_count;
    (void)start_seconds;
    return 0;
}
void wiliwili_audio_pause(int paused) { (void)paused; }
int wiliwili_audio_seek(double seconds) {
    (void)seconds;
    return 0;
}
void wiliwili_audio_set_volume(int volume) { (void)volume; }
void wiliwili_audio_set_speed(double speed) { (void)speed; }
int wiliwili_audio_is_active(void) { return 0; }
int wiliwili_audio_take_failure(void) { return 0; }
double wiliwili_audio_clock(void) { return -1.0; }
double wiliwili_audio_duration(void) { return 0.0; }
int wiliwili_audio_is_eof(void) { return 0; }
int wiliwili_audio_track_count(void) { return 0; }
int wiliwili_audio_track(void) { return 0; }
int wiliwili_audio_switch_track(int index) {
    (void)index;
    return 0;
}
void wiliwili_audio_observe(double mpv_time, double speed, int paused, int seeking) {
    (void)mpv_time;
    (void)speed;
    (void)paused;
    (void)seeking;
}
void wiliwili_audio_summary(void) {}

#endif /* PS5_NATIVE_APP */
