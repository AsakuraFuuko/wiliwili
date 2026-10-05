/*
 * P1 M2 native video prototype: FFmpeg demux/BSF -> sceVideodec2 -> NV12 -> AGC.
 *
 * This file owns the video thread and never feeds compressed video to mpv. mpv remains
 * responsible for audio, playback-time, pause and UI state. The path is gated by both
 * WILIWILI_TEST_VDEC=1 and WILIWILI_VDEC_PLAY=1.
 *
 * The Videodec2 ABI structs below match the already verified local probe. OutputInfo
 * has no PTS/frame id; output slots are identified only by frame-pool address and
 * display PTS is paired with the minimum pending PTS.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#if defined(PS5_NATIVE_APP) && defined(BOREALIS_USE_AGC)
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
#include <borealis/extern/nanovg/agc/evo_agc_runtime.h>
#endif

extern void wiliwili_boot_log(const char *message);

enum {
    VDEC_PLAY_SLOTS = 3,
    VDEC_PLAY_PENDING_LIMIT = 4,
    VDEC_PLAY_PENDING_CAP = 8,
    VDEC_PLAY_AU_BYTES = 0x800000,
    VDEC_PLAY_TIMEOUT_MS_DEFAULT = 250,
    VDEC_PLAY_MAX_FLUSH = 32,
    VDEC_PLAY_CODEC_AVC = 1,
    VDEC_PLAY_CODEC_HEVC = 974921,
    VDEC_PLAY_SYSMODULE = 207,
};

typedef struct {
    uint64_t size;
    uint32_t resource_type, codec_type, profile, max_level;
    int32_t max_width, max_height, max_dpb_frames;
    uint32_t pipeline_depth;
    uint64_t compute_queue, cpu_affinity;
    int32_t cpu_priority;
    uint32_t optimize_progressive, check_memory_type, reserved;
} VdecPlayConfig;

typedef struct {
    uint64_t size;
    uint64_t cpu_size;
    void *cpu;
    uint64_t gpu_size;
    void *gpu;
    uint64_t cpu_gpu_size;
    void *cpu_gpu;
    uint64_t max_frame_size;
    uint32_t frame_alignment, reserved;
} VdecPlayMemory;

typedef struct {
    uint64_t size;
    uint16_t pipe_id, queue_id;
    uint8_t check_memory_type, reserved0;
    uint16_t reserved1;
} VdecPlayComputeConfig;

typedef struct {
    uint64_t size, cpu_gpu_size;
    void *cpu_gpu;
} VdecPlayComputeMemory;

typedef struct {
    uint64_t size;
    void *au;
    uint64_t au_size, pts, dts, attached;
} VdecPlayInput;

typedef struct {
    uint64_t size;
    void *buffer;
    uint64_t buffer_size;
    uint32_t accepted, reserved;
} VdecPlayFrameBuffer;

typedef struct {
    uint64_t size;
    uint8_t valid, error, picture_count, padding;
    uint32_t codec, width, pitch, height, reserved;
    void *buffer;
    uint64_t buffer_size;
    uint32_t frame_format, pitch_bytes;
} VdecPlayOutput;

typedef struct {
    void *address;
    int64_t physical;
    size_t bytes;
} VdecPlayDirectMemory;

typedef struct {
    void *address;
    size_t bytes;
} VdecPlayFlexibleMemory;

typedef struct {
    VdecPlayDirectMemory compute, gpu, cpu_gpu, au_pool, frame_pool;
    VdecPlayFlexibleMemory cpu;
    uint64_t frame_size;
    void *compute_queue, *decoder;
    uint32_t codec;
    int visible_width, visible_height;
} VdecPlayDecoder;

typedef enum {
    VDEC_PLAY_SLOT_FREE = 0,
    VDEC_PLAY_SLOT_INFLIGHT = 1,
    VDEC_PLAY_SLOT_READY = 2,
    VDEC_PLAY_SLOT_CURRENT = 3,
} VdecPlaySlotState;

typedef struct {
    VdecPlaySlotState state;
    int width, height, pitch, pitch_bytes;
    int64_t pts90k;
    uint64_t sequence;
} VdecPlaySlot;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    int thread_started;
    volatile int stop_requested;
    volatile int paused;
    int active, fallback, failure_logged, resources_live, decoder_ready, source_eof;
    int current_slot, ready_queue[VDEC_PLAY_SLOTS], ready_count, present_pending;
    int timeout_ms, flush_each_decode;
    int64_t pending_pts[VDEC_PLAY_PENDING_CAP];
    int pending_count;
    int64_t last_output_pts, first_pts, last_pts;
    uint64_t input_count, accepted_count, output_count, presented_count, retired_count, sequence;
    char source_url[2048];
    char inject[64];
    VdecPlaySlot slots[VDEC_PLAY_SLOTS];
    VdecPlayDecoder decoder;
} VdecPlaySession;

static VdecPlaySession g_vdec_play = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
    .current_slot = -1,
    .last_output_pts = INT64_MIN,
};

#if defined(PS5_NATIVE_APP) && defined(BOREALIS_USE_AGC)
extern int32_t sceSysmoduleLoadModule(uint16_t id);
extern int64_t sceKernelGetDirectMemorySize(void);
extern int32_t sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int32_t, int64_t *);
extern int32_t sceKernelMapDirectMemory(void **, size_t, int32_t, int32_t, int64_t, size_t);
extern int32_t sceKernelReleaseDirectMemory(int64_t, size_t);
extern int32_t sceKernelMunmap(void *, size_t);
extern int32_t sceKernelMapNamedFlexibleMemory(void **, size_t, int32_t, int32_t, const char *);
extern int32_t sceKernelReleaseFlexibleMemory(void *, size_t);
extern int32_t sceKernelUsleep(unsigned int);
extern int32_t sceVideodec2QueryComputeMemoryInfo(void *);
extern int32_t sceVideodec2AllocateComputeQueue(const void *, const void *, void **);
extern int32_t sceVideodec2ReleaseComputeQueue(void *);
extern int32_t sceVideodec2QueryDecoderMemoryInfo(const void *, void *);
extern int32_t sceVideodec2CreateDecoder(const void *, const void *, void **);
extern int32_t sceVideodec2DeleteDecoder(void *);
extern int32_t sceVideodec2Reset(void *);
extern int32_t sceVideodec2Decode(void *, void *, void *, void *);
extern int32_t sceVideodec2Flush(void *, void *, void *);

static uint64_t play_now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

static int play_stop_requested(const VdecPlaySession *s) {
    return __atomic_load_n(&s->stop_requested, __ATOMIC_ACQUIRE) != 0;
}

static int play_gate_value(const char *name) {
    const char *value = getenv(name);
    return value != NULL && strcmp(value, "1") == 0;
}

static void play_logf(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    wiliwili_boot_log(line);
}

static void play_log_av_error(const char *stage, int rc) {
    char error[AV_ERROR_MAX_STRING_SIZE];
    if (av_strerror(rc, error, sizeof(error)) != 0) snprintf(error, sizeof(error), "rc=%d", rc);
    play_logf("vdec-play: %s failed rc=%d (%s)", stage, rc, error);
}

static uint64_t play_align16k(uint64_t value) { return (value + 0x3fffu) & ~UINT64_C(0x3fff); }

static void *play_alloc_direct(VdecPlayDirectMemory *m, uint64_t limit, size_t bytes, int32_t prot) {
    if (!m || !bytes) return NULL;
    memset(m, 0, sizeof(*m));
    m->physical = -1;
    if (sceKernelAllocateDirectMemory(0, (int64_t)limit, bytes, 0x4000, 12, &m->physical) != 0) {
        m->physical = -1;
        return NULL;
    }
    if (sceKernelMapDirectMemory(&m->address, bytes, prot, 0, m->physical, 0x4000) != 0 || !m->address) {
        sceKernelReleaseDirectMemory(m->physical, bytes);
        m->physical = -1;
        return NULL;
    }
    m->bytes = bytes;
    return m->address;
}

static void play_free_direct(VdecPlayDirectMemory *m) {
    if (!m) return;
    if (m->address) sceKernelMunmap(m->address, m->bytes);
    if (m->physical >= 0) sceKernelReleaseDirectMemory(m->physical, m->bytes);
    memset(m, 0, sizeof(*m));
    m->physical = -1;
}

static int play_alloc_flexible(VdecPlayFlexibleMemory *m, size_t bytes, const char *name) {
    memset(m, 0, sizeof(*m));
    if (!bytes) return 0;
    if (sceKernelMapNamedFlexibleMemory(&m->address, bytes, 0x03, 0, name) != 0 || !m->address) return -1;
    m->bytes = bytes;
    return 0;
}

static void play_free_flexible(VdecPlayFlexibleMemory *m) {
    if (!m || !m->address) return;
    sceKernelReleaseFlexibleMemory(m->address, m->bytes);
    sceKernelMunmap(m->address, m->bytes);
    memset(m, 0, sizeof(*m));
}

static void play_decoder_close(VdecPlayDecoder *d) {
    if (!d) return;
    if (d->decoder) sceVideodec2DeleteDecoder(d->decoder);
    play_free_direct(&d->frame_pool);
    play_free_direct(&d->au_pool);
    play_free_direct(&d->cpu_gpu);
    play_free_direct(&d->gpu);
    play_free_flexible(&d->cpu);
    if (d->compute_queue) sceVideodec2ReleaseComputeQueue(d->compute_queue);
    play_free_direct(&d->compute);
    memset(d, 0, sizeof(*d));
}

static int play_true_idr(const uint8_t *data, int size, uint32_t codec) {
    if (!data || size < 5) return 0;
    for (int i = 0; i + 4 < size;) {
        int prefix = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) prefix = 3;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) prefix = 4;
        if (!prefix) { ++i; continue; }
        const int type = codec == VDEC_PLAY_CODEC_AVC ? data[i + prefix] & 0x1f : (data[i + prefix] >> 1) & 0x3f;
        if ((codec == VDEC_PLAY_CODEC_AVC && type == 5) ||
            (codec == VDEC_PLAY_CODEC_HEVC && (type == 19 || type == 20))) return 1;
        i += prefix + 1;
    }
    return 0;
}

static int play_find_free_slot_locked(const VdecPlaySession *s) {
    for (int i = 0; i < VDEC_PLAY_SLOTS; ++i)
        if (s->slots[i].state == VDEC_PLAY_SLOT_FREE) return i;
    return -1;
}

static void play_release_slot_locked(VdecPlaySession *s, int slot) {
    if (slot < 0 || slot >= VDEC_PLAY_SLOTS) return;
    s->slots[slot].state = VDEC_PLAY_SLOT_FREE;
    pthread_cond_broadcast(&s->condition);
}

static void play_fail_locked(VdecPlaySession *s, const char *reason, int rc) {
    if (s->fallback) return;
    s->fallback = 1;
    s->active = 0;
    s->stop_requested = 1;
    if (!s->failure_logged) {
        play_logf("vdec-play: FALLBACK_A reason=%s rc=%d pending=%d inputs=%llu accepted=%llu outputs=%llu",
                  reason, rc, s->pending_count, (unsigned long long)s->input_count,
                  (unsigned long long)s->accepted_count, (unsigned long long)s->output_count);
        s->failure_logged = 1;
    }
    pthread_cond_broadcast(&s->condition);
}

static void play_fail(VdecPlaySession *s, const char *reason, int rc) {
    pthread_mutex_lock(&s->mutex);
    play_fail_locked(s, reason, rc);
    pthread_mutex_unlock(&s->mutex);
}

static int play_pending_push_locked(VdecPlaySession *s, int64_t pts90k) {
    if (s->pending_count >= VDEC_PLAY_PENDING_LIMIT) {
        play_logf("vdec-play: watchdog pending=%d limit=%d", s->pending_count + 1, VDEC_PLAY_PENDING_LIMIT);
        play_fail_locked(s, "pending-window", -9001);
        return -1;
    }
    s->pending_pts[s->pending_count++] = pts90k;
    return 0;
}

static int play_pending_pop_min_locked(VdecPlaySession *s, int64_t *pts90k) {
    if (s->pending_count <= 0 || !pts90k) return -1;
    int min = 0;
    for (int i = 1; i < s->pending_count; ++i)
        if (s->pending_pts[i] < s->pending_pts[min]) min = i;
    *pts90k = s->pending_pts[min];
    for (int i = min + 1; i < s->pending_count; ++i) s->pending_pts[i - 1] = s->pending_pts[i];
    --s->pending_count;
    return 0;
}

#endif

int wiliwili_vdec_play_enabled(void) {
#if defined(PS5_NATIVE_APP) && defined(BOREALIS_USE_AGC)
    return play_gate_value("WILIWILI_TEST_VDEC") && play_gate_value("WILIWILI_VDEC_PLAY");
#else
    return 0;
#endif
}

#if defined(PS5_NATIVE_APP) && defined(BOREALIS_USE_AGC)
static int play_output_slot(const VdecPlaySession *s, const VdecPlayOutput *out) {
    const uintptr_t base = (uintptr_t)s->decoder.frame_pool.address;
    const uintptr_t end = base + s->decoder.frame_pool.bytes;
    const uintptr_t address = (uintptr_t)out->buffer;
    if (address < base || address >= end || s->decoder.frame_size == 0) return -1;
    const uintptr_t offset = address - base;
    if (offset % s->decoder.frame_size != 0) return -1;
    const int slot = (int)(offset / s->decoder.frame_size);
    return slot >= 0 && slot < VDEC_PLAY_SLOTS ? slot : -1;
}

static int play_account_output_locked(VdecPlaySession *s, int input_slot, const VdecPlayOutput *out,
                                      int *output_slot) {
    if (!out->valid) {
        play_release_slot_locked(s, input_slot);
        return 0;
    }
    const int p010 = out->pitch_bytes != 0 && out->pitch_bytes == out->pitch * 2u;
    const uint64_t row_bytes = out->pitch_bytes != 0 ? out->pitch_bytes : out->pitch;
    const uint64_t required = row_bytes * ((uint64_t)out->height + ((uint64_t)out->height + 1u) / 2u);
    const int slot = play_output_slot(s, out);
    const int invalid = out->error || out->picture_count != 1 || out->codec != s->decoder.codec || p010 ||
                        out->width < (uint32_t)s->decoder.visible_width ||
                        out->height < (uint32_t)s->decoder.visible_height || out->pitch < out->width ||
                        out->pitch_bytes != out->pitch || out->buffer == NULL || out->buffer_size < required ||
                        slot < 0;
    if (invalid) {
        play_release_slot_locked(s, input_slot);
        play_fail_locked(s, "output-contract", -9002);
        return -1;
    }
    if (slot != input_slot && s->slots[slot].state != VDEC_PLAY_SLOT_INFLIGHT &&
        s->slots[slot].state != VDEC_PLAY_SLOT_FREE) {
        play_release_slot_locked(s, input_slot);
        play_fail_locked(s, "slot-reuse-before-retire", -9003);
        return -1;
    }
    int64_t pts90k = 0;
    if (play_pending_pop_min_locked(s, &pts90k) != 0) {
        play_release_slot_locked(s, input_slot);
        play_fail_locked(s, "output-without-pending-pts", -9004);
        return -1;
    }
    if (s->last_output_pts != INT64_MIN && pts90k <= s->last_output_pts) {
        play_release_slot_locked(s, input_slot);
        play_fail_locked(s, "pts-not-monotonic", -9005);
        return -1;
    }
    s->last_output_pts = pts90k;
    if (slot != input_slot) play_release_slot_locked(s, input_slot);
    s->slots[slot].state = VDEC_PLAY_SLOT_READY;
    s->slots[slot].width = (int)out->width;
    s->slots[slot].height = (int)out->height;
    s->slots[slot].pitch = (int)out->pitch;
    s->slots[slot].pitch_bytes = (int)out->pitch_bytes;
    s->slots[slot].pts90k = pts90k;
    s->slots[slot].sequence = ++s->sequence;
    if (s->ready_count >= VDEC_PLAY_SLOTS) {
        play_fail_locked(s, "ready-queue-overflow", -9006);
        return -1;
    }
    s->ready_queue[s->ready_count++] = slot;
    ++s->output_count;
    *output_slot = slot;
    pthread_cond_broadcast(&s->condition);
    return 1;
}

static int play_wait_for_frame_slot(VdecPlaySession *s) {
    pthread_mutex_lock(&s->mutex);
    int slot = play_find_free_slot_locked(s);
    while (slot < 0 && !play_stop_requested(s) && !s->fallback) {
        pthread_cond_wait(&s->condition, &s->mutex);
        slot = play_find_free_slot_locked(s);
    }
    if (slot >= 0 && !play_stop_requested(s) && !s->fallback)
        s->slots[slot].state = VDEC_PLAY_SLOT_INFLIGHT;
    else
        slot = -1;
    pthread_mutex_unlock(&s->mutex);
    return slot;
}

static int play_setup_decoder(VdecPlaySession *s, AVCodecParameters *par) {
    s->resources_live = 1;
    const uint32_t codec = par->codec_id == AV_CODEC_ID_HEVC ? VDEC_PLAY_CODEC_HEVC : VDEC_PLAY_CODEC_AVC;
    const int visible_width = par->width;
    const int visible_height = par->height;
    const int max_height = visible_height >= 2160 ? 2176 : (visible_height + 15) & ~15;
    const int profile = par->profile > 0 ? par->profile : (codec == VDEC_PLAY_CODEC_HEVC ? 1 : 100);
    const int level = par->level > 0 ? par->level :
                      (codec == VDEC_PLAY_CODEC_HEVC ? (visible_width >= 3840 ? 153 : 123)
                                                     : (visible_width >= 3840 ? 52 : 51));
    const uint64_t limit = (uint64_t)sceKernelGetDirectMemorySize();
    int32_t rc = sceSysmoduleLoadModule(VDEC_PLAY_SYSMODULE);
    if (rc != 0) {
        play_logf("vdec-play: sysmodule207 rc=%d", rc);
        return -1;
    }
    VdecPlayComputeMemory compute;
    memset(&compute, 0, sizeof(compute));
    compute.size = sizeof(compute);
    rc = sceVideodec2QueryComputeMemoryInfo(&compute);
    if (rc != 0) { play_logf("vdec-play: query_compute rc=%d", rc); return -1; }
    compute.cpu_gpu_size = play_align16k(compute.cpu_gpu_size);
    if (!play_alloc_direct(&s->decoder.compute, limit, (size_t)compute.cpu_gpu_size, 0x33)) {
        play_logf("vdec-play: compute allocation failed bytes=%llu", (unsigned long long)compute.cpu_gpu_size);
        return -1;
    }
    compute.cpu_gpu = s->decoder.compute.address;
    VdecPlayComputeConfig compute_config;
    memset(&compute_config, 0, sizeof(compute_config));
    compute_config.size = sizeof(compute_config);
    rc = sceVideodec2AllocateComputeQueue(&compute_config, &compute, &s->decoder.compute_queue);
    if (rc != 0) { play_logf("vdec-play: compute_queue rc=%d", rc); return -1; }

    VdecPlayConfig config;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.resource_type = 1;
    config.codec_type = codec;
    config.profile = (uint32_t)profile;
    config.max_level = (uint32_t)level;
    config.max_width = visible_width;
    config.max_height = max_height;
    config.max_dpb_frames = -1;
    config.pipeline_depth = 1;
    config.compute_queue = (uint64_t)s->decoder.compute_queue;
    config.cpu_affinity = 0x3f;
    config.cpu_priority = 700;
    config.optimize_progressive = 1;
    play_logf("vdec-play: config codec=%u profile=%u level=%u max=%dx%d dpb=-1 depth=1",
              codec, config.profile, config.max_level, config.max_width, config.max_height);

    VdecPlayMemory memory;
    memset(&memory, 0, sizeof(memory));
    memory.size = sizeof(memory);
    rc = sceVideodec2QueryDecoderMemoryInfo(&config, &memory);
    if (rc != 0) { play_logf("vdec-play: query_decoder rc=%d", rc); return -1; }
    const uint64_t cpu_bytes = play_align16k(memory.cpu_size);
    const uint64_t gpu_bytes = play_align16k(memory.gpu_size);
    const uint64_t cpu_gpu_bytes = play_align16k(memory.cpu_gpu_size);
    if (play_alloc_flexible(&s->decoder.cpu, (size_t)cpu_bytes, "VdecPlayCpu") != 0 ||
        (gpu_bytes && !play_alloc_direct(&s->decoder.gpu, limit, (size_t)gpu_bytes, 0x32)) ||
        (cpu_gpu_bytes && !play_alloc_direct(&s->decoder.cpu_gpu, limit, (size_t)cpu_gpu_bytes, 0x33))) {
        play_logf("vdec-play: decoder memory allocation failed cpu=0x%llx gpu=0x%llx cpu_gpu=0x%llx",
                  (unsigned long long)cpu_bytes, (unsigned long long)gpu_bytes,
                  (unsigned long long)cpu_gpu_bytes);
        return -1;
    }
    memory.cpu = s->decoder.cpu.address;
    memory.cpu_size = cpu_bytes;
    memory.gpu = s->decoder.gpu.address;
    memory.gpu_size = gpu_bytes;
    memory.cpu_gpu = s->decoder.cpu_gpu.address;
    memory.cpu_gpu_size = cpu_gpu_bytes;
    s->decoder.frame_size = play_align16k(memory.max_frame_size);
    if (!s->decoder.frame_size ||
        !play_alloc_direct(&s->decoder.au_pool, limit, VDEC_PLAY_AU_BYTES * VDEC_PLAY_SLOTS, 0x32) ||
        !play_alloc_direct(&s->decoder.frame_pool, limit, (size_t)s->decoder.frame_size * VDEC_PLAY_SLOTS, 0x32)) {
        play_logf("vdec-play: frame pool allocation failed frame_size=0x%llx",
                  (unsigned long long)s->decoder.frame_size);
        return -1;
    }
    rc = sceVideodec2CreateDecoder(&config, &memory, &s->decoder.decoder);
    if (rc != 0) { play_logf("vdec-play: create_decoder rc=%d", rc); return -1; }
    rc = sceVideodec2Reset(s->decoder.decoder);
    if (rc != 0) { play_logf("vdec-play: reset rc=%d", rc); return -1; }
    s->decoder.codec = codec;
    s->decoder.visible_width = visible_width;
    s->decoder.visible_height = visible_height;
    s->resources_live = 1;
    play_logf("vdec-play: decoder ready visible=%dx%d frame_size=0x%llx",
              visible_width, visible_height, (unsigned long long)s->decoder.frame_size);
    return 0;
}

#endif

#if defined(PS5_NATIVE_APP) && defined(BOREALIS_USE_AGC)
static int play_decode_call(VdecPlaySession *s, int slot, const uint8_t *data, int size,
                            int64_t pts90k, int is_flush, int flush_index) {
    uint8_t *au = (uint8_t *)s->decoder.au_pool.address +
                  (size_t)(s->input_count % VDEC_PLAY_SLOTS) * VDEC_PLAY_AU_BYTES;
    if (!is_flush) {
        if (!data || size <= 0 || (uint64_t)size > VDEC_PLAY_AU_BYTES) {
            pthread_mutex_lock(&s->mutex);
            play_release_slot_locked(s, slot);
            play_fail_locked(s, "AU-too-large", -9007);
            pthread_mutex_unlock(&s->mutex);
            return -1;
        }
        memcpy(au, data, (size_t)size);
        if (strcmp(s->inject, "bad-stream") == 0 && s->input_count == 1) au[0] ^= 0xff;
    }
    VdecPlayInput input;
    memset(&input, 0, sizeof(input));
    input.size = sizeof(input);
    input.au = au;
    input.au_size = is_flush ? 0 : (uint64_t)size;
    input.pts = is_flush ? 0 : (uint64_t)pts90k;
    /* The PTS gate proved that decode DTS must never reach this ABI field. */
    input.dts = UINT64_MAX;
    VdecPlayFrameBuffer frame;
    memset(&frame, 0, sizeof(frame));
    frame.size = sizeof(frame);
    frame.buffer = (uint8_t *)s->decoder.frame_pool.address + (size_t)slot * s->decoder.frame_size;
    frame.buffer_size = s->decoder.frame_size;
    VdecPlayOutput output;
    memset(&output, 0, sizeof(output));
    output.size = sizeof(output);

    const uint64_t begin = play_now_us();
    if (!is_flush && strcmp(s->inject, "timeout") == 0)
        sceKernelUsleep((unsigned int)(s->timeout_ms + 1) * 1000u);
    int32_t rc = is_flush ? sceVideodec2Flush(s->decoder.decoder, &frame, &output)
                          : sceVideodec2Decode(s->decoder.decoder, &input, &frame, &output);
    int did_flush = 0;
    if (!is_flush && rc == 0 && !output.valid &&
        (s->flush_each_decode || s->pending_count >= VDEC_PLAY_PENDING_LIMIT)) {
        memset(&output, 0, sizeof(output));
        output.size = sizeof(output);
        rc = sceVideodec2Flush(s->decoder.decoder, &frame, &output);
        did_flush = 1;
    }
    const uint64_t elapsed = play_now_us() - begin;
    if (elapsed > (uint64_t)s->timeout_ms * 1000u) {
        pthread_mutex_lock(&s->mutex);
        play_release_slot_locked(s, slot);
        play_fail_locked(s, (is_flush || did_flush) ? "flush-timeout" : "decode-timeout", -9008);
        pthread_mutex_unlock(&s->mutex);
        return -1;
    }
    if (rc != 0) {
        pthread_mutex_lock(&s->mutex);
        play_release_slot_locked(s, slot);
        play_fail_locked(s, (is_flush || did_flush) ? "flush-error" : "decode-error", rc);
        pthread_mutex_unlock(&s->mutex);
        play_logf("vdec-play: %s index=%d rc=%d accepted=%u", (is_flush || did_flush) ? "flush" : "decode",
                  is_flush ? flush_index : (int)s->input_count, rc, frame.accepted);
        return -1;
    }
    if (!is_flush && strcmp(s->inject, "bad-stream") == 0 && s->input_count == 1) {
        pthread_mutex_lock(&s->mutex);
        play_release_slot_locked(s, slot);
        play_fail_locked(s, "injected-bad-stream", -9015);
        pthread_mutex_unlock(&s->mutex);
        return -1;
    }
    pthread_mutex_lock(&s->mutex);
    /* Some firmware revisions leave FrameBuffer.accepted=0 while the AU is
     * buffered; rc==0 is the stable accepted-input signal. */
    if (!is_flush) ++s->accepted_count;
    int output_slot = -1;
    const int account = play_account_output_locked(s, slot, &output, &output_slot);
    pthread_mutex_unlock(&s->mutex);
    if (account < 0) return -1;
    if (output.valid && (s->sequence == 1 || (s->sequence % 120u) == 0u))
        play_logf("vdec-play: output seq=%llu pts90k=%lld slot=%d flush=%d",
                  (unsigned long long)s->sequence, (long long)s->last_output_pts, output_slot,
                  is_flush || did_flush);
    return output.valid ? 1 : 0;
}

static int play_flush(VdecPlaySession *s) {
    for (int i = 0; i < VDEC_PLAY_MAX_FLUSH; ++i) {
        const int slot = play_wait_for_frame_slot(s);
        if (slot < 0) return -1;
        const int result = play_decode_call(s, slot, NULL, 0, 0, 1, i);
        if (result < 0) return -1;
        if (result == 0) {
            pthread_mutex_lock(&s->mutex);
            const int pending = s->pending_count;
            const uint64_t outputs = s->output_count;
            const uint64_t accepted = s->accepted_count;
            if (pending != 0 || outputs != accepted) {
                play_fail_locked(s, "flush-tail-mismatch", -9010);
                pthread_mutex_unlock(&s->mutex);
                return -1;
            }
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
    }
    play_fail(s, "flush-tail-window", -9011);
    return -1;
}

static int play_submit_au(VdecPlaySession *s, const uint8_t *data, int size, int64_t raw_pts,
                          AVRational time_base, int keyframe) {
    if (!data || size <= 0 || raw_pts == AV_NOPTS_VALUE) {
        play_fail(s, "missing-display-pts", -9012);
        return -1;
    }
    const int64_t source_pts = av_rescale_q(raw_pts, time_base, (AVRational){1, 90000});
    pthread_mutex_lock(&s->mutex);
    if (s->input_count == 0) {
        s->first_pts = source_pts;
        s->last_pts = source_pts;
    }
    const int64_t pts90k = source_pts - s->first_pts;
    /* Decode-order packet PTS may go backward for B frames; the watchdog checks
     * monotonicity only after min-PTS pairing on decoded display output. */
    if (pts90k < 0) {
        play_fail_locked(s, "input-pts-invalid", -9013);
        pthread_mutex_unlock(&s->mutex);
        return -1;
    }
    s->last_pts = source_pts;
    if (play_pending_push_locked(s, pts90k) != 0) {
        pthread_mutex_unlock(&s->mutex);
        return -1;
    }
    const uint64_t input_index = s->input_count++;
    pthread_mutex_unlock(&s->mutex);

    if (input_index == 0 && (!keyframe || !play_true_idr(data, size, s->decoder.codec))) {
        play_fail(s, "startup-not-true-IDR", -9014);
        return -1;
    }
    if (strcmp(s->inject, "window") == 0 && input_index == 0) {
        pthread_mutex_lock(&s->mutex);
        s->pending_count = VDEC_PLAY_PENDING_LIMIT + 1;
        play_fail_locked(s, "pending-window-injected", -9016);
        pthread_mutex_unlock(&s->mutex);
        return -1;
    }
    if (strcmp(s->inject, "reset") == 0 && input_index == 30) {
        const int32_t reset_rc = sceVideodec2Reset(s->decoder.decoder);
        play_logf("vdec-play: injected midstream reset rc=%d", reset_rc);
        play_fail(s, "injected-reset-failure", reset_rc == 0 ? -9017 : reset_rc);
        return -1;
    }
    const int slot = play_wait_for_frame_slot(s);
    if (slot < 0) return -1;
    return play_decode_call(s, slot, data, size, pts90k, 0, -1) < 0 ? -1 : 0;
}

typedef struct {
    AVFormatContext *format;
    AVBSFContext *bsf;
    AVStream *stream;
    AVRational time_base;
    uint32_t codec;
    int has_reorder;
} VdecPlayMedia;

static int play_interrupt(void *opaque) {
    return play_stop_requested((const VdecPlaySession *)opaque);
}

static void play_media_close(VdecPlayMedia *media) {
    if (!media) return;
    if (media->bsf) av_bsf_free(&media->bsf);
    if (media->format) avformat_close_input(&media->format);
    memset(media, 0, sizeof(*media));
}

static int play_media_open(VdecPlaySession *s, VdecPlayMedia *media) {
    AVFormatContext *format = avformat_alloc_context();
    if (!format) {
        play_logf("vdec-play: avformat_alloc_context failed");
        return -1;
    }
    format->interrupt_callback.callback = play_interrupt;
    format->interrupt_callback.opaque = s;
    AVDictionary *options = NULL;
    av_dict_set(&options, "rw_timeout", "5000000", 0);
    av_dict_set(&options, "timeout", "5000000", 0);
    av_dict_set(&options, "headers", "Referer: https://www.bilibili.com\r\nUser-Agent: Mozilla/5.0\r\n", 0);
    int rc = avformat_open_input(&format, s->source_url, NULL, &options);
    av_dict_free(&options);
    if (rc < 0) {
        play_log_av_error("open-input", rc);
        avformat_free_context(format);
        return -1;
    }
    rc = avformat_find_stream_info(format, NULL);
    if (rc < 0) {
        play_log_av_error("find-stream-info", rc);
        avformat_close_input(&format);
        return -1;
    }
    const int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (stream_index < 0) {
        play_logf("vdec-play: no video stream");
        avformat_close_input(&format);
        return -1;
    }
    AVStream *stream = format->streams[stream_index];
    media->has_reorder = stream->codecpar->video_delay > 0;
    AVCodecContext *probe_context = avcodec_alloc_context3(NULL);
    if (probe_context != NULL) {
        if (avcodec_parameters_to_context(probe_context, stream->codecpar) == 0 && probe_context->has_b_frames > 0)
            media->has_reorder = 1;
        avcodec_free_context(&probe_context);
    }
    const char *filter_name = stream->codecpar->codec_id == AV_CODEC_ID_HEVC
                                  ? "hevc_mp4toannexb"
                                  : stream->codecpar->codec_id == AV_CODEC_ID_H264 ? "h264_mp4toannexb" : NULL;
    if (!filter_name) {
        play_logf("vdec-play: unsupported codec id=%d", stream->codecpar->codec_id);
        avformat_close_input(&format);
        return -1;
    }
    const AVBitStreamFilter *filter = av_bsf_get_by_name(filter_name);
    if (!filter || av_bsf_alloc(filter, &media->bsf) < 0 || !media->bsf) {
        play_logf("vdec-play: BSF unavailable name=%s", filter_name);
        avformat_close_input(&format);
        return -1;
    }
    rc = avcodec_parameters_copy(media->bsf->par_in, stream->codecpar);
    if (rc < 0) {
        play_log_av_error("BSF parameters", rc);
        av_bsf_free(&media->bsf);
        avformat_close_input(&format);
        return -1;
    }
    media->bsf->time_base_in = stream->time_base;
    rc = av_bsf_init(media->bsf);
    if (rc < 0) {
        play_log_av_error("BSF init", rc);
        av_bsf_free(&media->bsf);
        avformat_close_input(&format);
        return -1;
    }
    media->format = format;
    media->stream = stream;
    media->time_base = stream->time_base;
    media->codec = stream->codecpar->codec_id == AV_CODEC_ID_HEVC ? VDEC_PLAY_CODEC_HEVC : VDEC_PLAY_CODEC_AVC;
    play_logf("vdec-play: demux ready stream=%d codec=%u size=%dx%d timebase=%d/%d bsf=%s reorder=%d",
              stream_index, media->codec, stream->codecpar->width, stream->codecpar->height,
              media->time_base.num, media->time_base.den, filter_name, media->has_reorder);
    return 0;
}

static void play_wait_if_paused(VdecPlaySession *s);
static int play_media_loop(VdecPlaySession *s, VdecPlayMedia *media) {
    AVPacket *input = av_packet_alloc();
    AVPacket *filtered = av_packet_alloc();
    if (!input || !filtered) {
        av_packet_free(&input);
        av_packet_free(&filtered);
        play_fail(s, "packet-allocation", -9018);
        return -1;
    }
    int result = 0;
    while (!play_stop_requested(s)) {
        play_wait_if_paused(s);
        if (play_stop_requested(s)) break;
        const int read_rc = av_read_frame(media->format, input);
        if (read_rc == AVERROR_EOF) break;
        if (read_rc < 0) {
            play_log_av_error("read-frame", read_rc);
            play_fail(s, "demux-error", read_rc);
            result = -1;
            break;
        }
        if (input->stream_index != media->stream->index) {
            av_packet_unref(input);
            continue;
        }
        int rc = av_bsf_send_packet(media->bsf, input);
        av_packet_unref(input);
        if (rc < 0) {
            play_log_av_error("BSF send", rc);
            play_fail(s, "bsf-error", rc);
            result = -1;
            break;
        }
        for (;;) {
            rc = av_bsf_receive_packet(media->bsf, filtered);
            if (rc == AVERROR(EAGAIN)) break;
            if (rc == AVERROR_EOF) goto flush_bsf;
            if (rc < 0) {
                play_log_av_error("BSF receive", rc);
                play_fail(s, "bsf-error", rc);
                result = -1;
                goto done;
            }
            if (play_submit_au(s, filtered->data, filtered->size, filtered->pts, media->time_base,
                               (filtered->flags & AV_PKT_FLAG_KEY) != 0) != 0) {
                av_packet_unref(filtered);
                result = -1;
                goto done;
            }
            av_packet_unref(filtered);
        }
    }
flush_bsf:
    if (!play_stop_requested(s) && !s->fallback) {
        int rc = av_bsf_send_packet(media->bsf, NULL);
        if (rc < 0 && rc != AVERROR_EOF) {
            play_log_av_error("BSF flush", rc);
            play_fail(s, "bsf-flush-error", rc);
            result = -1;
        }
        while (result == 0 && !play_stop_requested(s) && !s->fallback) {
            rc = av_bsf_receive_packet(media->bsf, filtered);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) {
                play_log_av_error("BSF flush receive", rc);
                play_fail(s, "bsf-flush-error", rc);
                result = -1;
                break;
            }
            if (play_submit_au(s, filtered->data, filtered->size, filtered->pts, media->time_base,
                               (filtered->flags & AV_PKT_FLAG_KEY) != 0) != 0) result = -1;
            av_packet_unref(filtered);
        }
        if (result == 0 && !s->fallback && !play_stop_requested(s) && play_flush(s) != 0) result = -1;
    }
done:
    av_packet_free(&input);
    av_packet_free(&filtered);
    return result;
}

#endif

#if defined(PS5_NATIVE_APP) && defined(BOREALIS_USE_AGC)
static void play_wait_if_paused(VdecPlaySession *s) {
    pthread_mutex_lock(&s->mutex);
    while (__atomic_load_n(&s->paused, __ATOMIC_ACQUIRE) && !play_stop_requested(s) && !s->fallback)
        pthread_cond_wait(&s->condition, &s->mutex);
    pthread_mutex_unlock(&s->mutex);
}

static void *play_thread_main(void *opaque) {
    VdecPlaySession *s = (VdecPlaySession *)opaque;
    VdecPlayMedia media;
    memset(&media, 0, sizeof(media));
    int result = -1;
    const int media_ok = play_media_open(s, &media) == 0;
    if (media_ok) {
        s->flush_each_decode = !media.has_reorder;
        play_logf("vdec-play: flush_each_decode=%d", s->flush_each_decode);
    }
    const int decoder_ok = media_ok && !play_stop_requested(s) && !s->fallback &&
                           play_setup_decoder(s, media.stream->codecpar) == 0;
    if (!decoder_ok) {
        if (!play_stop_requested(s) && !s->fallback) play_fail(s, "startup-failure", -9020);
        if (s->resources_live) {
            play_decoder_close(&s->decoder);
            s->resources_live = 0;
        }
    } else {
        pthread_mutex_lock(&s->mutex);
        s->decoder_ready = 1;
        pthread_cond_broadcast(&s->condition);
        pthread_mutex_unlock(&s->mutex);
        result = play_media_loop(s, &media);
    }
    if (result == 0 && !play_stop_requested(s) && !s->fallback) {
        pthread_mutex_lock(&s->mutex);
        if (s->input_count == 0 || s->output_count != s->accepted_count || s->pending_count != 0)
            play_fail_locked(s, "stream-count-mismatch", -9019);
        else {
            s->source_eof = 1;
            play_logf("vdec-play: EOF inputs=%llu accepted=%llu outputs=%llu",
                      (unsigned long long)s->input_count, (unsigned long long)s->accepted_count,
                      (unsigned long long)s->output_count);
        }
        pthread_mutex_unlock(&s->mutex);
    }
    play_media_close(&media);
    pthread_mutex_lock(&s->mutex);
    pthread_cond_broadcast(&s->condition);
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}

static void play_reset_state_locked(VdecPlaySession *s) {
    s->stop_requested = 0;
    s->paused = 0;
    s->active = 0;
    s->fallback = 0;
    s->failure_logged = 0;
    s->resources_live = 0;
    s->decoder_ready = 0;
    s->source_eof = 0;
    s->current_slot = -1;
    s->ready_count = 0;
    s->present_pending = 0;
    s->timeout_ms = VDEC_PLAY_TIMEOUT_MS_DEFAULT;
    s->flush_each_decode = 1;
    s->pending_count = 0;
    s->last_output_pts = INT64_MIN;
    s->first_pts = 0;
    s->last_pts = 0;
    s->input_count = 0;
    s->accepted_count = 0;
    s->output_count = 0;
    s->presented_count = 0;
    s->retired_count = 0;
    s->sequence = 0;
    memset(s->ready_queue, 0, sizeof(s->ready_queue));
    memset(s->pending_pts, 0, sizeof(s->pending_pts));
    memset(s->slots, 0, sizeof(s->slots));
    memset(s->source_url, 0, sizeof(s->source_url));
    memset(s->inject, 0, sizeof(s->inject));
}

void wiliwili_vdec_play_stop(void) {
    VdecPlaySession *s = &g_vdec_play;
    pthread_mutex_lock(&s->mutex);
    const int join_needed = s->thread_started;
    const pthread_t thread = s->thread;
    s->stop_requested = 1;
    s->active = 0;
    pthread_cond_broadcast(&s->condition);
    pthread_mutex_unlock(&s->mutex);
    if (join_needed && !pthread_equal(pthread_self(), thread)) pthread_join(thread, NULL);
#if defined(PS5_NATIVE_APP) && defined(BOREALIS_USE_AGC)
    if (s->resources_live) evo_agc_runtime_wait_idle(500);
#endif
    pthread_mutex_lock(&s->mutex);
    if (s->resources_live) play_decoder_close(&s->decoder);
    s->thread_started = 0;
    play_reset_state_locked(s);
    pthread_mutex_unlock(&s->mutex);
}

int wiliwili_vdec_play_start(const char *requested_url) {
    if (!wiliwili_vdec_play_enabled() || requested_url == NULL || requested_url[0] == '\0') return 0;
    wiliwili_vdec_play_stop();
    const char *override_url = getenv("WILIWILI_VDEC_URL");
    const char *source = override_url != NULL && override_url[0] != '\0' ? override_url : requested_url;
    VdecPlaySession *s = &g_vdec_play;
    pthread_mutex_lock(&s->mutex);
    if (strlen(source) >= sizeof(s->source_url)) {
        pthread_mutex_unlock(&s->mutex);
        play_logf("vdec-play: URL too long; keep A");
        return 0;
    }
    play_reset_state_locked(s);
    snprintf(s->source_url, sizeof(s->source_url), "%s", source);
    const char *inject = getenv("WILIWILI_VDEC_INJECT");
    if (inject) snprintf(s->inject, sizeof(s->inject), "%s", inject);
    const char *timeout = getenv("WILIWILI_VDEC_TIMEOUT_MS");
    if (timeout && atoi(timeout) > 0) s->timeout_ms = atoi(timeout);
    s->active = 1;
    if (pthread_create(&s->thread, NULL, play_thread_main, s) != 0) {
        s->active = 0;
        s->fallback = 1;
        pthread_mutex_unlock(&s->mutex);
        play_logf("vdec-play: thread create failed; keep A");
        return 0;
    }
    s->thread_started = 1;
    pthread_mutex_unlock(&s->mutex);
    play_logf("vdec-play: start source=%s inject=%s timeout_ms=%d dts=UINT64_MAX",
              s->source_url, s->inject[0] ? s->inject : "none", s->timeout_ms);
    return 1;
}

int wiliwili_vdec_play_is_active(void) {
    VdecPlaySession *s = &g_vdec_play;
    pthread_mutex_lock(&s->mutex);
    const int active = s->active && !s->fallback;
    pthread_mutex_unlock(&s->mutex);
    return active;
}

void wiliwili_vdec_play_pause(int paused) {
    VdecPlaySession *s = &g_vdec_play;
    pthread_mutex_lock(&s->mutex);
    s->paused = paused != 0;
    pthread_cond_broadcast(&s->condition);
    pthread_mutex_unlock(&s->mutex);
}

int wiliwili_vdec_play_draw(void) {
    VdecPlaySession *s = &g_vdec_play;
    pthread_mutex_lock(&s->mutex);
    if (!s->active || s->fallback) {
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    /* Before the first decoded frame, native video owns the rectangle and keeps
     * mpv's video track disabled; the clear color is preferable to double decode. */
    if (!s->decoder_ready || !s->resources_live) {
        pthread_mutex_unlock(&s->mutex);
        return 1;
    }
    if (s->ready_count > 0) {
        const int next = s->ready_queue[0];
        for (int i = 1; i < s->ready_count; ++i) s->ready_queue[i - 1] = s->ready_queue[i];
        --s->ready_count;
        if (s->current_slot >= 0) play_release_slot_locked(s, s->current_slot);
        s->current_slot = next;
        s->slots[next].state = VDEC_PLAY_SLOT_CURRENT;
    }
    const int slot = s->current_slot;
    if (slot < 0) {
        pthread_mutex_unlock(&s->mutex);
        return 1;
    }
    VdecPlaySlot *frame = &s->slots[slot];
    const uint8_t *y = (const uint8_t *)s->decoder.frame_pool.address +
                       (size_t)slot * s->decoder.frame_size;
    const uint8_t *uv = y + (size_t)frame->pitch_bytes * frame->height;
    const int rc = evo_agc_blit_yuv(y, frame->pitch_bytes, uv, frame->pitch_bytes,
                                    NULL, 0, NULL, 0, frame->width, frame->height,
                                    s->decoder.visible_width, s->decoder.visible_height,
                                    2, 0, 1, 0, frame->pts90k * 1000000 / 90000);
    if (rc != 0) {
        play_fail_locked(s, "agc-blit", rc);
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    s->present_pending = 1;
    ++s->presented_count;
    if (s->presented_count == 1 || s->presented_count % 120 == 0)
        play_logf("vdec-play: presented=%llu pts90k=%lld slot=%d retired=%llu",
                  (unsigned long long)s->presented_count, (long long)frame->pts90k, slot,
                  (unsigned long long)s->retired_count);
    pthread_mutex_unlock(&s->mutex);
    return 1;
}

void wiliwili_vdec_play_frame_retire(void) {
    VdecPlaySession *s = &g_vdec_play;
    pthread_mutex_lock(&s->mutex);
    if (s->present_pending) {
        s->present_pending = 0;
        ++s->retired_count;
        pthread_cond_broadcast(&s->condition);
    }
    pthread_mutex_unlock(&s->mutex);
}

#else

int wiliwili_vdec_play_start(const char *url) { (void)url; return 0; }
int wiliwili_vdec_play_is_active(void) { return 0; }
void wiliwili_vdec_play_stop(void) {}
void wiliwili_vdec_play_pause(int paused) { (void)paused; }
int wiliwili_vdec_play_draw(void) { return 0; }
void wiliwili_vdec_play_frame_retire(void) {}

#endif
