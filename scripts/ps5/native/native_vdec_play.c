/*
 * P1 M2 native video prototype: FFmpeg demux/BSF -> sceVideodec2 -> NV12 -> AGC.
 *
 * This file owns the video thread and never feeds compressed video to mpv. mpv remains
 * responsible for audio, playback-time, pause and UI state. The production path defaults
 * to AUTO capability gating; test environment variables can force it on or off.
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
#include <fcntl.h>
#include <math.h>

#if defined(PS5_NATIVE_APP) && defined(BOREALIS_USE_AGC)
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <borealis/extern/nanovg/agc/evo_agc_runtime.h>
#endif

extern void wiliwili_boot_log(const char *message);

enum {
    VDEC_PLAY_SLOTS = 3,
    VDEC_PLAY_PENDING_MIN = 4,
    VDEC_PLAY_PENDING_LIMIT = 4,
    VDEC_PLAY_PENDING_CAP = 12,
    VDEC_PLAY_CLOCK_JUMP_90K = 90000,
    VDEC_PLAY_AU_BYTES = 0x800000,
    /* Decode/Flush watchdogs are per ABI call; slot waits, demux, and diagnostic UDP logs stay outside it. */
    VDEC_PLAY_TIMEOUT_MS_DEFAULT = 250,
    VDEC_PLAY_MAX_FLUSH = 32,
    VDEC_PLAY_BACKUP_CAP         = 4,
    VDEC_PLAY_CODEC_AVC = 1,
    VDEC_PLAY_CODEC_HEVC = 974921,
    VDEC_PLAY_SYSMODULE = 207,
};
enum {
    VDEC_PLAY_MODE_AUTO = 0,
    VDEC_PLAY_MODE_ON = 1,
    VDEC_PLAY_MODE_OFF = 2,
};

static volatile int g_vdec_user_mode = VDEC_PLAY_MODE_AUTO;
enum {
    VDEC_PLAY_RESULT_OK = 0,
    VDEC_PLAY_RESULT_SEEK = -2,
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
    VdecPlayDirectMemory compute, gpu, cpu_gpu, au_pool, frame_pool, p010_pool;
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
    int width, height, pitch, pitch_bytes, p010;
    int64_t pts90k;
    uint64_t sequence;
} VdecPlaySlot;

enum {
    VDEC_PLAY_LATENCY_WINDOW = 600,
    VDEC_PLAY_CONTEXT_CAP    = 8,
    VDEC_PLAY_STAGE_NONE     = 0,
    VDEC_PLAY_STAGE_DECODE   = 1,
    VDEC_PLAY_STAGE_FLUSH    = 2,
};

typedef struct {
    uint32_t decode_us[VDEC_PLAY_LATENCY_WINDOW];
    uint32_t flush_us[VDEC_PLAY_LATENCY_WINDOW];
    uint32_t drift_us[VDEC_PLAY_LATENCY_WINDOW];
    uint32_t publish_age_us[VDEC_PLAY_LATENCY_WINDOW];
    uint32_t publish_future_us[VDEC_PLAY_LATENCY_WINDOW];
    uint32_t clock_step_us[VDEC_PLAY_LATENCY_WINDOW];
    uint32_t clock_interval_us[VDEC_PLAY_LATENCY_WINDOW];
    uint32_t audio_phase_us[VDEC_PLAY_LATENCY_WINDOW];
    int publish_age_count, publish_future_count, clock_step_count, clock_interval_count, audio_phase_count;
    int decode_count;
    int flush_count;
    int drift_count;
    uint64_t segment;
} VdecPlayLatencyWindow;

typedef struct {
    uint64_t index;
    int64_t raw_pts, raw_dts, pts90k;
    int keyframe, idr, cra;
    int pending_before, pending_after;
    int input_slot, output_slot;
    VdecPlaySlotState slot_state;
} VdecPlayAuContext;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    int thread_started;
    volatile int stop_requested;
    int timeout_ms, flush_each_decode, trace_au, trace_latency, pending_limit, pending_limit_override, adaptive_pending,
        p010_enabled;
    volatile int paused;
    volatile int seek_requested;
    int active, fallback, failure_logged, resources_live, decoder_ready, source_eof, reopen_media;
    int current_slot, ready_queue[VDEC_PLAY_SLOTS], ready_count, present_pending;
    double seek_seconds;
    int sequential_seek;
    int64_t sequential_seek_pts90k;
    uint64_t seek_generation;
    int64_t pending_pts[VDEC_PLAY_PENDING_CAP];
    int pending_count, peak_pending_count;
    int64_t last_output_pts, first_pts, last_pts, timeline_origin_pts, last_clock_pts;
    int64_t last_sample_clock_pts;
    uint64_t last_sample_wall_us;
    int64_t last_clock_input_pts;
    uint64_t last_clock_input_us;
    int clock_prediction_valid;
    int video_color_trc;
    int timeline_origin_set, awaiting_idr, sps_seen, pps_seen, vps_seen, stress_eof_count;
    double last_logged_speed;
    uint64_t input_count, accepted_count, output_count, presented_count, retired_count, sequence;
    uint64_t advanced_count, dropped_count, error_count, order_error_count;
    uint64_t direct_mem_peak;
    uint64_t last_demux_wait_us, max_demux_wait_us, demux_read_count;
    uint64_t last_slot_wait_us, max_slot_wait_us, slot_wait_count;
    uint64_t present_pending_since_us, max_present_wait_us;
    int last_call_stage, last_call_slot, last_call_flush_index, last_call_rc, last_call_valid, last_call_error,
        last_call_accepted, last_call_did_flush;
    uint64_t last_call_au_index, last_call_decode_us, last_call_flush_us, last_call_elapsed_us;
    uint64_t failover_count;
    char source_url[2048];
    char backup_urls[VDEC_PLAY_BACKUP_CAP][2048];
    int backup_count, backup_index;
    char inject[64];
    VdecPlaySlot slots[VDEC_PLAY_SLOTS];
    VdecPlayAuContext au_context[VDEC_PLAY_CONTEXT_CAP];
    int au_context_head, au_context_count;
    VdecPlayLatencyWindow latency;
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
static int play_env_flag(const char *name) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') return -1;
    if (strcmp(value, "0") == 0) return 0;
    if (strcmp(value, "1") == 0) return 1;
    return -1;
}

static int play_effective_mode(void) {
    const int test = play_env_flag("WILIWILI_TEST_VDEC");
    const int play = play_env_flag("WILIWILI_VDEC_PLAY");
    if (test == 0 || play == 0) return VDEC_PLAY_MODE_OFF;
    if (play == 1) return VDEC_PLAY_MODE_ON;
    /* Keep the historical probe-only TEST_VDEC=1 mode from starting playback. */
    if (test == 1) return VDEC_PLAY_MODE_OFF;
    return __atomic_load_n(&g_vdec_user_mode, __ATOMIC_ACQUIRE);
}

static void play_logf(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    wiliwili_boot_log(line);
}
static uint64_t play_direct_mem_bytes(const VdecPlaySession *s) {
    if (!s) return 0;
    return (uint64_t)s->decoder.compute.bytes + (uint64_t)s->decoder.gpu.bytes + (uint64_t)s->decoder.cpu_gpu.bytes +
           (uint64_t)s->decoder.au_pool.bytes + (uint64_t)s->decoder.frame_pool.bytes +
           (uint64_t)s->decoder.p010_pool.bytes;
}

static int play_percentile_index(int count, int percentage) {
    if (count <= 0) return 0;
    int index = (count - 1) * percentage / 100;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    return index;
}

static void play_trace_sort(uint32_t *values, int count) {
    for (int i = 1; i < count; ++i) {
        const uint32_t value = values[i];
        int j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
}

static void play_trace_report(VdecPlaySession *s, const char *label) {
    if (!s || !s->trace_latency) return;
    uint32_t age[VDEC_PLAY_LATENCY_WINDOW], future[VDEC_PLAY_LATENCY_WINDOW];
    uint32_t step[VDEC_PLAY_LATENCY_WINDOW], interval[VDEC_PLAY_LATENCY_WINDOW];
    uint32_t phase[VDEC_PLAY_LATENCY_WINDOW];
    const int age_count = s->latency.publish_age_count;
    const int future_count = s->latency.publish_future_count;
    const int step_count = s->latency.clock_step_count;
    const int interval_count = s->latency.clock_interval_count;
    const int phase_count = s->latency.audio_phase_count;
    if (age_count == 0 && future_count == 0 && step_count == 0 && interval_count == 0 && phase_count == 0) return;
    memcpy(age, s->latency.publish_age_us, (size_t)age_count * sizeof(age[0]));
    memcpy(future, s->latency.publish_future_us, (size_t)future_count * sizeof(future[0]));
    memcpy(step, s->latency.clock_step_us, (size_t)step_count * sizeof(step[0]));
    memcpy(interval, s->latency.clock_interval_us, (size_t)interval_count * sizeof(interval[0]));
    memcpy(phase, s->latency.audio_phase_us, (size_t)phase_count * sizeof(phase[0]));
    play_trace_sort(age, age_count);
    play_trace_sort(future, future_count);
    play_trace_sort(step, step_count);
    play_trace_sort(interval, interval_count);
    play_trace_sort(phase, phase_count);
    const int age_p50 = play_percentile_index(age_count, 50), age_p95 = play_percentile_index(age_count, 95);
    const int age_p99 = play_percentile_index(age_count, 99);
    const int future_p50 = play_percentile_index(future_count, 50), future_p95 = play_percentile_index(future_count, 95);
    const int future_p99 = play_percentile_index(future_count, 99);
    const int step_p50 = play_percentile_index(step_count, 50), step_p95 = play_percentile_index(step_count, 95);
    const int step_p99 = play_percentile_index(step_count, 99);
    const int interval_p50 = play_percentile_index(interval_count, 50), interval_p95 = play_percentile_index(interval_count, 95);
    const int interval_p99 = play_percentile_index(interval_count, 99);
    const int phase_p50 = play_percentile_index(phase_count, 50), phase_p95 = play_percentile_index(phase_count, 95);
    const int phase_p99 = play_percentile_index(phase_count, 99);
    play_logf(
        "vdec-clock segment=%llu label=%s publish_n=%d age_p50_us=%u age_p95_us=%u age_p99_us=%u age_max_us=%u "
        "future_n=%d future_p50_us=%u future_p95_us=%u future_p99_us=%u future_max_us=%u "
        "step_n=%d step_p50_us=%u step_p95_us=%u step_p99_us=%u step_max_us=%u "
        "sample_n=%d sample_p50_us=%u sample_p95_us=%u sample_p99_us=%u sample_max_us=%u "
        "audio_n=%d audio_p50_us=%u audio_p95_us=%u audio_p99_us=%u audio_max_us=%u",
        (unsigned long long)s->latency.segment, label ? label : "window", age_count,
        age_count ? age[age_p50] : 0, age_count ? age[age_p95] : 0, age_count ? age[age_p99] : 0,
        age_count ? age[age_count - 1] : 0, future_count, future_count ? future[future_p50] : 0,
        future_count ? future[future_p95] : 0, future_count ? future[future_p99] : 0,
        future_count ? future[future_count - 1] : 0, step_count, step_count ? step[step_p50] : 0,
        step_count ? step[step_p95] : 0, step_count ? step[step_p99] : 0, step_count ? step[step_count - 1] : 0,
        interval_count, interval_count ? interval[interval_p50] : 0, interval_count ? interval[interval_p95] : 0,
        interval_count ? interval[interval_p99] : 0, interval_count ? interval[interval_count - 1] : 0,
        phase_count, phase_count ? phase[phase_p50] : 0, phase_count ? phase[phase_p95] : 0,
        phase_count ? phase[phase_p99] : 0, phase_count ? phase[phase_count - 1] : 0);
}

static void play_latency_report(VdecPlaySession *s, const char *label) {
    if (!s || !s->trace_latency ||
        (s->latency.decode_count == 0 && s->latency.flush_count == 0 && s->latency.drift_count == 0 &&
         s->latency.publish_age_count == 0 && s->latency.publish_future_count == 0 &&
         s->latency.clock_step_count == 0 && s->latency.clock_interval_count == 0 &&
         s->latency.audio_phase_count == 0))
        return;
    uint32_t decode[VDEC_PLAY_LATENCY_WINDOW];
    uint32_t flush[VDEC_PLAY_LATENCY_WINDOW];
    uint32_t drift[VDEC_PLAY_LATENCY_WINDOW];
    const int decode_count = s->latency.decode_count;
    const int flush_count  = s->latency.flush_count;
    const int drift_count  = s->latency.drift_count;
    memcpy(decode, s->latency.decode_us, (size_t)decode_count * sizeof(decode[0]));
    memcpy(flush, s->latency.flush_us, (size_t)flush_count * sizeof(flush[0]));
    memcpy(drift, s->latency.drift_us, (size_t)drift_count * sizeof(drift[0]));
    for (int i = 1; i < decode_count; ++i) {
        const uint32_t value = decode[i];
        int j                = i;
        while (j > 0 && decode[j - 1] > value) {
            decode[j] = decode[j - 1];
            --j;
        }
        decode[j] = value;
    }
    for (int i = 1; i < flush_count; ++i) {
        const uint32_t value = flush[i];
        int j                = i;
        while (j > 0 && flush[j - 1] > value) {
            flush[j] = flush[j - 1];
            --j;
        }
        flush[j] = value;
    }
    for (int i = 1; i < drift_count; ++i) {
        const uint32_t value = drift[i];
        int j                = i;
        while (j > 0 && drift[j - 1] > value) {
            drift[j] = drift[j - 1];
            --j;
        }
        drift[j] = value;
    }
    const int decode_p50 = play_percentile_index(decode_count, 50);
    const int decode_p95 = play_percentile_index(decode_count, 95);
    const int decode_p99 = play_percentile_index(decode_count, 99);
    const int flush_p50  = play_percentile_index(flush_count, 50);
    const int flush_p95  = play_percentile_index(flush_count, 95);
    const int flush_p99  = play_percentile_index(flush_count, 99);
    const int drift_p50  = play_percentile_index(drift_count, 50);
    const int drift_p95  = play_percentile_index(drift_count, 95);
    const int drift_p99  = play_percentile_index(drift_count, 99);
    play_logf(
        "vdec-lat segment=%llu label=%s decode_n=%d p50_us=%u p95_us=%u p99_us=%u max_us=%u "
        "flush_n=%d p50_us=%u p95_us=%u p99_us=%u max_us=%u "
        "drift_n=%d p50_us=%u p95_us=%u p99_us=%u max_us=%u direct_mem=%llu "
        "slot_wait_max_us=%llu present_wait_max_us=%llu demux_max_us=%llu",
        (unsigned long long)s->latency.segment++, label ? label : "window", decode_count,
        decode_count ? decode[decode_p50] : 0, decode_count ? decode[decode_p95] : 0,
        decode_count ? decode[decode_p99] : 0, decode_count ? decode[decode_count - 1] : 0, flush_count,
        flush_count ? flush[flush_p50] : 0, flush_count ? flush[flush_p95] : 0, flush_count ? flush[flush_p99] : 0,
        flush_count ? flush[flush_count - 1] : 0, drift_count, drift_count ? drift[drift_p50] : 0,
        drift_count ? drift[drift_p95] : 0, drift_count ? drift[drift_p99] : 0,
        drift_count ? drift[drift_count - 1] : 0, (unsigned long long)play_direct_mem_bytes(s),
        (unsigned long long)s->max_slot_wait_us, (unsigned long long)s->max_present_wait_us,
        (unsigned long long)s->max_demux_wait_us);
    play_trace_report(s, label);
    s->latency.decode_count = 0;
    s->latency.flush_count  = 0;
    s->latency.drift_count  = 0;
    s->latency.publish_age_count = 0;
    s->latency.publish_future_count = 0;
    s->latency.clock_step_count = 0;
    s->latency.clock_interval_count = 0;
    s->latency.audio_phase_count = 0;
}

static void play_latency_record_drift(VdecPlaySession *s, uint64_t drift_us) {
    if (!s || !s->trace_latency || s->latency.drift_count >= VDEC_PLAY_LATENCY_WINDOW) return;
    const uint32_t value                          = drift_us > UINT32_MAX ? UINT32_MAX : (uint32_t)drift_us;
    s->latency.drift_us[s->latency.drift_count++] = value;
}
static void play_latency_record(VdecPlaySession *s, int is_flush, uint64_t elapsed_us) {
    if (!s || !s->trace_latency) return;
    if (!is_flush && s->latency.decode_count >= VDEC_PLAY_LATENCY_WINDOW) play_latency_report(s, "window");
    const uint32_t value = elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us;
    if (is_flush) {
        if (s->latency.flush_count < VDEC_PLAY_LATENCY_WINDOW) s->latency.flush_us[s->latency.flush_count++] = value;
    } else {
        s->latency.decode_us[s->latency.decode_count++] = value;
    }
}
static void play_latency_record_publish(VdecPlaySession *s, uint64_t age_us, uint64_t future_us) {
    if (!s || !s->trace_latency) return;
    if (s->latency.publish_age_count < VDEC_PLAY_LATENCY_WINDOW)
        s->latency.publish_age_us[s->latency.publish_age_count++] =
            age_us > UINT32_MAX ? UINT32_MAX : (uint32_t)age_us;
    if (future_us != 0 && s->latency.publish_future_count < VDEC_PLAY_LATENCY_WINDOW)
        s->latency.publish_future_us[s->latency.publish_future_count++] =
            future_us > UINT32_MAX ? UINT32_MAX : (uint32_t)future_us;
}

static void play_latency_record_clock(VdecPlaySession *s, uint64_t step_us, uint64_t interval_us,
                                      uint64_t audio_phase_us) {
    if (!s || !s->trace_latency) return;
    if (s->latency.clock_step_count < VDEC_PLAY_LATENCY_WINDOW)
        s->latency.clock_step_us[s->latency.clock_step_count++] =
            step_us > UINT32_MAX ? UINT32_MAX : (uint32_t)step_us;
    if (s->latency.clock_interval_count < VDEC_PLAY_LATENCY_WINDOW)
        s->latency.clock_interval_us[s->latency.clock_interval_count++] =
            interval_us > UINT32_MAX ? UINT32_MAX : (uint32_t)interval_us;
    if (audio_phase_us != UINT64_MAX && s->latency.audio_phase_count < VDEC_PLAY_LATENCY_WINDOW)
        s->latency.audio_phase_us[s->latency.audio_phase_count++] =
            audio_phase_us > UINT32_MAX ? UINT32_MAX : (uint32_t)audio_phase_us;
}

static const char *play_slot_state_name(VdecPlaySlotState state) {
    switch (state) {
        case VDEC_PLAY_SLOT_FREE:
            return "FREE";
        case VDEC_PLAY_SLOT_INFLIGHT:
            return "INFLIGHT";
        case VDEC_PLAY_SLOT_READY:
            return "READY";
        case VDEC_PLAY_SLOT_CURRENT:
            return "CURRENT";
        default:
            return "?";
    }
}

static VdecPlayAuContext *play_context_for_index_locked(VdecPlaySession *s, uint64_t index) {
    for (int i = 0; i < s->au_context_count; ++i) {
        const int position = (s->au_context_head + VDEC_PLAY_CONTEXT_CAP - 1 - i) % VDEC_PLAY_CONTEXT_CAP;
        if (s->au_context[position].index == index) return &s->au_context[position];
    }
    return NULL;
}

static void play_log_failure_context_locked(VdecPlaySession *s, const char *reason, int rc) {
    play_latency_report(s, "failure");
    play_logf(
        "vdec-play: failure-context reason=%s rc=%d stage=%d au=%llu slot=%d flush_index=%d "
        "decode_us=%llu flush_us=%llu call_us=%llu call_rc=%d valid=%d error=%d accepted=%d "
        "pending=%d seek=%d eof=%d reopen=%d direct_mem=%llu demux_last_us=%llu demux_max_us=%llu "
        "slot_last_us=%llu slot_max_us=%llu present_max_us=%llu",
        reason ? reason : "?", rc, s->last_call_stage, (unsigned long long)s->last_call_au_index, s->last_call_slot,
        s->last_call_flush_index, (unsigned long long)s->last_call_decode_us, (unsigned long long)s->last_call_flush_us,
        (unsigned long long)s->last_call_elapsed_us, s->last_call_rc, s->last_call_valid, s->last_call_error,
        s->last_call_accepted, s->pending_count, s->seek_requested, s->source_eof, s->reopen_media,
        (unsigned long long)play_direct_mem_bytes(s), (unsigned long long)s->last_demux_wait_us,
        (unsigned long long)s->max_demux_wait_us, (unsigned long long)s->last_slot_wait_us,
        (unsigned long long)s->max_slot_wait_us, (unsigned long long)s->max_present_wait_us);
    for (int i = 0; i < s->au_context_count; ++i) {
        const int position =
            (s->au_context_head + VDEC_PLAY_CONTEXT_CAP - s->au_context_count + i) % VDEC_PLAY_CONTEXT_CAP;
        const VdecPlayAuContext *context = &s->au_context[position];
        play_logf(
            "vdec-play: failure-au index=%llu pts=%lld dts=%lld pts90k=%lld key=%d idr=%d cra=%d "
            "pending=%d->%d slots=%d/%d state=%s",
            (unsigned long long)context->index, (long long)context->raw_pts, (long long)context->raw_dts,
            (long long)context->pts90k, context->keyframe, context->idr, context->cra, context->pending_before,
            context->pending_after, context->input_slot, context->output_slot,
            play_slot_state_name(context->slot_state));
    }
}
/* Keep transport backups separate from decoder recovery: a demux failure may switch URL, while a VDEC/AGC failure still
 * fails closed to A. */
static int play_add_backup_url_locked(VdecPlaySession *s, const char *url) {
    if (!s || !url || url[0] == '\0' || strlen(url) >= sizeof(s->backup_urls[0]) || strcmp(url, s->source_url) == 0 ||
        s->backup_count >= VDEC_PLAY_BACKUP_CAP)
        return 0;
    for (int i = 0; i < s->backup_count; ++i)
        if (strcmp(s->backup_urls[i], url) == 0) return 0;
    snprintf(s->backup_urls[s->backup_count], sizeof(s->backup_urls[0]), "%s", url);
    ++s->backup_count;
    return 1;
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
    play_free_direct(&d->p010_pool);
    play_free_direct(&d->frame_pool);
    play_free_direct(&d->au_pool);
    play_free_direct(&d->cpu_gpu);
    play_free_direct(&d->gpu);
    play_free_flexible(&d->cpu);
    if (d->compute_queue) sceVideodec2ReleaseComputeQueue(d->compute_queue);
    play_free_direct(&d->compute);
    memset(d, 0, sizeof(*d));
}

typedef struct {
    int idr;
    int cra;
    int has_vps;
    int has_sps;
    int has_pps;
} VdecPlayNalFlags;

static VdecPlayNalFlags play_scan_nals(const uint8_t *data, int size, uint32_t codec) {
    VdecPlayNalFlags flags;
    memset(&flags, 0, sizeof(flags));
    if (!data || size < 5) return flags;
    for (int i = 0; i + 4 < size;) {
        int prefix = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) prefix = 3;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) prefix = 4;
        if (!prefix) {
            ++i;
            continue;
        }
        const int type = codec == VDEC_PLAY_CODEC_AVC ? data[i + prefix] & 0x1f : (data[i + prefix] >> 1) & 0x3f;
        if (codec == VDEC_PLAY_CODEC_AVC) {
            if (type == 5) flags.idr = 1;
            if (type == 7) flags.has_sps = 1;
            if (type == 8) flags.has_pps = 1;
        } else {
            if (type == 19 || type == 20) flags.idr = 1;
            if (type == 21) flags.cra = 1;
            if (type == 32) flags.has_vps = 1;
            if (type == 33) flags.has_sps = 1;
            if (type == 34) flags.has_pps = 1;
        }
        i += prefix + 1;
    }
    return flags;
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
        play_log_failure_context_locked(s, reason, rc);
        play_logf(
            "vdec-play: FALLBACK_A reason=%s rc=%d pending=%d limit=%d peak=%d inputs=%llu accepted=%llu outputs=%llu "
            "errors=%llu order_errors=%llu",
            reason, rc, s->pending_count, s->pending_limit, s->peak_pending_count, (unsigned long long)s->input_count,
            (unsigned long long)s->accepted_count, (unsigned long long)s->output_count,
            (unsigned long long)s->error_count, (unsigned long long)s->order_error_count);
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
    for (int i = 0; i < s->pending_count; ++i) {
        if (s->pending_pts[i] == pts90k) {
            ++s->order_error_count;
            play_fail_locked(s, "duplicate-pending-pts", -9024);
            return -1;
        }
    }
    if (s->pending_count + 1 > s->peak_pending_count) s->peak_pending_count = s->pending_count + 1;
    if (s->pending_count >= s->pending_limit) {
        play_logf("vdec-play: watchdog pending=%d limit=%d peak=%d", s->pending_count + 1,
                  s->pending_limit, s->peak_pending_count);
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
    return play_effective_mode() != VDEC_PLAY_MODE_OFF;
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
    if (out->error) {
        ++s->error_count;
        play_release_slot_locked(s, input_slot);
        play_fail_locked(s, "output-error", out->error);
        return -1;
    }
    if (!out->valid) {
        play_release_slot_locked(s, input_slot);
        return 0;
    }
    const int p010 = out->pitch_bytes != 0 && out->pitch_bytes == out->pitch * 2u;
    const uint64_t row_bytes = out->pitch_bytes != 0 ? out->pitch_bytes : out->pitch;
    const uint64_t required = row_bytes * ((uint64_t)out->height + ((uint64_t)out->height + 1u) / 2u);
    const int slot = play_output_slot(s, out);
    if (p010 && !s->p010_enabled) {
        play_release_slot_locked(s, input_slot);
        play_fail_locked(s, "p010-disabled", -9026);
        return -1;
    }
    const int pitch_invalid = p010 ? out->pitch_bytes != out->pitch * 2u : out->pitch_bytes != out->pitch;
    const int invalid = out->error || out->picture_count != 1 || out->codec != s->decoder.codec ||
                        out->width < (uint32_t)s->decoder.visible_width ||
                        out->height < (uint32_t)s->decoder.visible_height || out->pitch < out->width || pitch_invalid ||
                        out->buffer == NULL || out->buffer_size < required || slot < 0;
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
        ++s->order_error_count;
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
    s->slots[slot].p010 = p010;
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
    const uint64_t wait_begin = play_now_us();
    int slot = play_find_free_slot_locked(s);
    while (slot < 0 && !play_stop_requested(s) && !s->fallback && !s->seek_requested) {
        pthread_cond_wait(&s->condition, &s->mutex);
        slot = play_find_free_slot_locked(s);
    }
    const uint64_t waited_us = play_now_us() - wait_begin;
    s->last_slot_wait_us     = waited_us;
    if (waited_us > s->max_slot_wait_us) s->max_slot_wait_us = waited_us;
    if (waited_us != 0) ++s->slot_wait_count;
    if (s->trace_latency && waited_us >= 50000)
        play_logf("vdec-play: slot-wait us=%llu states=%s/%s/%s pending=%d present=%d", (unsigned long long)waited_us,
                  play_slot_state_name(s->slots[0].state), play_slot_state_name(s->slots[1].state),
                  play_slot_state_name(s->slots[2].state), s->pending_count, s->present_pending);
    if (slot >= 0 && !play_stop_requested(s) && !s->fallback && !s->seek_requested)
        s->slots[slot].state = VDEC_PLAY_SLOT_INFLIGHT;
    else if (s->seek_requested && !play_stop_requested(s) && !s->fallback)
        slot = -2;
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
    if (s->p010_enabled && !play_alloc_direct(&s->decoder.p010_pool, limit,
                                               (size_t)s->decoder.frame_size * VDEC_PLAY_SLOTS, 0x32)) {
        play_logf("vdec-play: P010 conversion pool allocation failed frame_size=0x%llx",
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
    s->direct_mem_peak = play_direct_mem_bytes(s);
    play_logf("vdec-play: decoder ready visible=%dx%d frame_size=0x%llx direct_mem=%llu",
              visible_width, visible_height, (unsigned long long)s->decoder.frame_size,
              (unsigned long long)s->direct_mem_peak);
    return 0;
}

#endif

#if defined(PS5_NATIVE_APP) && defined(BOREALIS_USE_AGC)
static int play_decode_call(VdecPlaySession *s, int slot, const uint8_t *data, int size, int64_t pts90k, int is_flush,
                            int flush_index, uint64_t au_index) {
    uint8_t *au =
        (uint8_t *)s->decoder.au_pool.address + (size_t)(s->input_count % VDEC_PLAY_SLOTS) * VDEC_PLAY_AU_BYTES;
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

    const uint64_t call_begin = play_now_us();
    uint64_t decode_us        = 0;
    uint64_t flush_us         = 0;
    int32_t rc                = 0;
    if (is_flush) {
    const uint64_t begin = play_now_us();
        rc                   = sceVideodec2Flush(s->decoder.decoder, &frame, &output);
        flush_us             = play_now_us() - begin;
        play_latency_record(s, 1, flush_us);
    } else {
        const uint64_t begin = play_now_us();
        if (strcmp(s->inject, "timeout") == 0) sceKernelUsleep((unsigned int)(s->timeout_ms + 1) * 1000u);
        rc        = sceVideodec2Decode(s->decoder.decoder, &input, &frame, &output);
        decode_us = play_now_us() - begin;
        play_latency_record(s, 0, decode_us);
    }
    int did_flush = 0;
    if (!is_flush && rc == 0 && !output.valid && (s->flush_each_decode || s->pending_count >= s->pending_limit)) {
        memset(&output, 0, sizeof(output));
        output.size = sizeof(output);
        const uint64_t begin = play_now_us();
        rc = sceVideodec2Flush(s->decoder.decoder, &frame, &output);
        flush_us             = play_now_us() - begin;
        play_latency_record(s, 1, flush_us);
        did_flush = 1;
    }
    const uint64_t operation_elapsed = decode_us + flush_us;
    s->last_call_stage               = (is_flush || did_flush) ? VDEC_PLAY_STAGE_FLUSH : VDEC_PLAY_STAGE_DECODE;
    s->last_call_au_index            = au_index;
    s->last_call_slot                = slot;
    s->last_call_flush_index         = flush_index;
    s->last_call_rc                  = rc;
    s->last_call_valid               = output.valid;
    s->last_call_error               = output.error;
    s->last_call_accepted            = frame.accepted;
    s->last_call_did_flush           = did_flush;
    s->last_call_decode_us           = decode_us;
    s->last_call_flush_us            = flush_us;
    s->last_call_elapsed_us          = operation_elapsed;
    if (s->trace_au)
        play_logf(
            "vdec-play: output raw valid=%d error=%d picture_count=%d accepted=%u flush=%d decode_us=%llu "
            "flush_us=%llu operation_us=%llu",
            output.valid, output.error, output.picture_count, frame.accepted, is_flush || did_flush,
            (unsigned long long)decode_us, (unsigned long long)flush_us, (unsigned long long)operation_elapsed);
    const uint64_t call_elapsed = play_now_us() - call_begin;
    s->last_call_elapsed_us     = call_elapsed;
    if (s->trace_au && call_elapsed > (uint64_t)s->timeout_ms * 1000u && decode_us <= (uint64_t)s->timeout_ms * 1000u &&
        flush_us <= (uint64_t)s->timeout_ms * 1000u)
        play_logf("vdec-play: combined-watchdog-would-fire call_us=%llu operation_us=%llu decode_us=%llu flush_us=%llu",
                  (unsigned long long)call_elapsed, (unsigned long long)operation_elapsed,
                  (unsigned long long)decode_us, (unsigned long long)flush_us);
    if (decode_us > (uint64_t)s->timeout_ms * 1000u) {
        pthread_mutex_lock(&s->mutex);
        play_release_slot_locked(s, slot);
        play_fail_locked(s, "decode-timeout", -9008);
        pthread_mutex_unlock(&s->mutex);
        return -1;
    }
    if (flush_us > (uint64_t)s->timeout_ms * 1000u) {
        pthread_mutex_lock(&s->mutex);
        play_release_slot_locked(s, slot);
        play_fail_locked(s, "flush-timeout", -9008);
        pthread_mutex_unlock(&s->mutex);
        return -1;
    }
    if (rc != 0) {
        pthread_mutex_lock(&s->mutex);
        play_release_slot_locked(s, slot);
        play_fail_locked(s, (is_flush || did_flush) ? "flush-error" : "decode-error", rc);
        pthread_mutex_unlock(&s->mutex);
        play_logf("vdec-play: %s index=%d rc=%d accepted=%u decode_us=%llu flush_us=%llu",
                  (is_flush || did_flush) ? "flush" : "decode", is_flush ? flush_index : (int)au_index, rc,
                  frame.accepted, (unsigned long long)decode_us, (unsigned long long)flush_us);
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
    VdecPlayAuContext *context = play_context_for_index_locked(s, au_index);
    if (context) {
        context->pending_after = s->pending_count;
        context->output_slot   = output_slot;
        context->slot_state    = s->slots[slot].state;
    }
    if (s->trace_au && output.valid)
        play_logf(
            "vdec-play: output seq=%llu pts90k=%lld valid=%d error=%d picture_count=%d slot=%d flush=%d pending=%d",
                  (unsigned long long)s->sequence, (long long)s->last_output_pts, output.valid, output.error,
                  output.picture_count, output_slot, is_flush || did_flush, s->pending_count);
    pthread_mutex_unlock(&s->mutex);
    if (account < 0) return -1;
    if (output.valid && (s->sequence == 1 || (s->sequence % 120u) == 0u))
        play_logf("vdec-play: output seq=%llu pts90k=%lld slot=%d flush=%d limit=%d peak=%d error=%d picture_count=%d",
                  (unsigned long long)s->sequence, (long long)s->last_output_pts, output_slot, is_flush || did_flush,
                  s->pending_limit, s->peak_pending_count, output.error, output.picture_count);
    return output.valid ? 1 : 0;
}

static int play_flush(VdecPlaySession *s) {
    for (int i = 0; i < VDEC_PLAY_MAX_FLUSH; ++i) {
        const int slot = play_wait_for_frame_slot(s);
        if (slot == -2) return VDEC_PLAY_RESULT_SEEK;
        if (slot < 0) return -1;
        const int result = play_decode_call(s, slot, NULL, 0, 0, 1, i, UINT64_MAX);
        if (result == VDEC_PLAY_RESULT_SEEK) return VDEC_PLAY_RESULT_SEEK;
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
                          int64_t raw_dts, AVRational time_base, int keyframe) {
    if (!data || size <= 0 || raw_pts == AV_NOPTS_VALUE) {
        play_fail(s, "missing-display-pts", -9012);
        return -1;
    }
    const int64_t source_pts = av_rescale_q(raw_pts, time_base, (AVRational){1, 90000});
    const VdecPlayNalFlags nal = play_scan_nals(data, size, s->decoder.codec);
    pthread_mutex_lock(&s->mutex);
    if (s->seek_requested) {
        pthread_mutex_unlock(&s->mutex);
        return VDEC_PLAY_RESULT_SEEK;
    }
    const int new_vps = nal.has_vps && !s->vps_seen;
    const int new_sps = nal.has_sps && !s->sps_seen;
    const int new_pps = nal.has_pps && !s->pps_seen;
    if (nal.has_vps) s->vps_seen = 1;
    if (nal.has_sps) s->sps_seen = 1;
    if (nal.has_pps) s->pps_seen = 1;
    if (s->awaiting_idr) {
        const int true_idr = keyframe && nal.idr && play_true_idr(data, size, s->decoder.codec);
        if (!true_idr) {
            if (s->trace_au)
                play_logf(
                    "vdec-play: AU skip-until-IDR index=%llu pts=%lld dts=%lld key=%d idr=%d cra=%d new_vps=%d "
                    "new_sps=%d new_pps=%d size=%d",
                    (unsigned long long)s->input_count, (long long)raw_pts, (long long)raw_dts, keyframe, nal.idr,
                    nal.cra, new_vps, new_sps, new_pps, size);
            pthread_mutex_unlock(&s->mutex);
            return VDEC_PLAY_RESULT_OK;
        }
        s->awaiting_idr = 0;
    }
    if (!s->timeline_origin_set) {
        s->timeline_origin_pts = source_pts;
        s->first_pts = source_pts;
        s->timeline_origin_set = 1;
    }
    const int64_t pts90k = source_pts - s->timeline_origin_pts;
    /* Decode-order packet PTS may go backward for B frames; the watchdog checks
     * monotonicity only after min-PTS pairing on decoded display output. */
    if (pts90k < 0) {
        play_fail_locked(s, "input-pts-invalid", -9013);
        pthread_mutex_unlock(&s->mutex);
        return -1;
    }
    if (s->trace_au)
        play_logf(
            "vdec-play: AU index=%llu pts=%lld dts=%lld pts90k=%lld key=%d idr=%d cra=%d new_vps=%d new_sps=%d "
            "new_pps=%d size=%d pending=%d",
            (unsigned long long)s->input_count, (long long)raw_pts, (long long)raw_dts, (long long)pts90k, keyframe,
            nal.idr, nal.cra, new_vps, new_sps, new_pps, size, s->pending_count);
    s->last_pts = source_pts;
    const int pending_before = s->pending_count;
    if (play_pending_push_locked(s, pts90k) != 0) {
        pthread_mutex_unlock(&s->mutex);
        return -1;
    }
    const uint64_t input_index = s->input_count++;
    VdecPlayAuContext *context = &s->au_context[s->au_context_head];
    memset(context, 0, sizeof(*context));
    context->index          = input_index;
    context->raw_pts        = raw_pts;
    context->raw_dts        = raw_dts;
    context->pts90k         = pts90k;
    context->keyframe       = keyframe;
    context->idr            = nal.idr;
    context->cra            = nal.cra;
    context->pending_before = pending_before;
    context->pending_after  = s->pending_count;
    context->input_slot     = -1;
    context->output_slot    = -1;
    context->slot_state     = VDEC_PLAY_SLOT_FREE;
    s->au_context_head      = (s->au_context_head + 1) % VDEC_PLAY_CONTEXT_CAP;
    if (s->au_context_count < VDEC_PLAY_CONTEXT_CAP) ++s->au_context_count;
    pthread_mutex_unlock(&s->mutex);

    if (strcmp(s->inject, "window") == 0 && input_index == 0) {
        pthread_mutex_lock(&s->mutex);
        s->pending_count = s->pending_limit + 1;
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
    if (slot == -2) return VDEC_PLAY_RESULT_SEEK;
    if (slot < 0) return -1;
    pthread_mutex_lock(&s->mutex);
    context = play_context_for_index_locked(s, input_index);
    if (context) {
        context->input_slot = slot;
        context->slot_state = s->slots[slot].state;
    }
    pthread_mutex_unlock(&s->mutex);
    return play_decode_call(s, slot, data, size, pts90k, 0, -1, input_index) < 0 ? -1 : VDEC_PLAY_RESULT_OK;
}

enum {
    VDEC_PLAY_TRACE_PACKET_CAP = 8,
    VDEC_PLAY_TRACE_NAL_CAP = 6,
};

typedef struct {
    uint64_t index;
    int64_t pts, dts, pos;
    int size, duration, flags;
    int format, length_size, nal_count, truncated;
    uint32_t first_declared_size;
    int first_declared_offset;
    int type[VDEC_PLAY_TRACE_NAL_CAP];
    int nal_size[VDEC_PLAY_TRACE_NAL_CAP];
    int offset[VDEC_PLAY_TRACE_NAL_CAP];
} VdecPlayPacketTrace;
typedef struct {
    AVFormatContext *format;
    AVBSFContext *bsf;
    AVStream *stream;
    AVRational time_base;
    uint32_t codec;
    int has_reorder;
    int video_delay, has_b_frames, color_trc;
    int nal_length_size;
    uint64_t packet_index;
    int trace_head, trace_count;
    VdecPlayPacketTrace trace[VDEC_PLAY_TRACE_PACKET_CAP];
} VdecPlayMedia;

static int play_find_start_code(const uint8_t *data, int size, int from, int *prefix) {
    for (int i = from; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            *prefix = 3;
            return i;
        }
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            *prefix = 4;
            return i;
        }
    }
    return -1;
}

static int play_packet_trace_parse_avcc(VdecPlayPacketTrace *trace, const uint8_t *data, int size, uint32_t codec,
                                        int nal_length_size) {
    if (nal_length_size < 1 || nal_length_size > 4) nal_length_size = 4;
    trace->length_size = nal_length_size;
    int cursor = 0;
    while (cursor + nal_length_size <= size) {
        uint32_t nal_size = 0;
        for (int i = 0; i < nal_length_size; ++i) nal_size = (nal_size << 8) | data[cursor + i];
        if (trace->nal_count == 0) {
            trace->first_declared_size = nal_size;
            trace->first_declared_offset = cursor;
        }
        const int payload = cursor + nal_length_size;
        if (nal_size > (uint32_t)(size - payload)) {
            trace->truncated = 1;
            return 0;
        }
        if (nal_size > 0) {
            if (trace->nal_count < VDEC_PLAY_TRACE_NAL_CAP) {
                const int slot = trace->nal_count;
                trace->type[slot] = codec == VDEC_PLAY_CODEC_AVC ? data[payload] & 0x1f : (data[payload] >> 1) & 0x3f;
                trace->nal_size[slot] = (int)nal_size;
                trace->offset[slot] = cursor;
            } else {
                trace->truncated = 1;
            }
            ++trace->nal_count;
        }
        cursor = payload + (int)nal_size;
    }
    if (cursor != size) {
        trace->truncated = 1;
        return 0;
    }
    return 1;
}

static int play_packet_trace_parse_annexb(VdecPlayPacketTrace *trace, const uint8_t *data, int size, uint32_t codec) {
    int prefix = 0;
    const int start = play_find_start_code(data, size, 0, &prefix);
    if (start != 0) return 0;
    trace->format = 2;
    int cursor = start;
    while (cursor >= 0 && cursor < size) {
        int current_prefix = 0;
        const int nal_start = play_find_start_code(data, size, cursor, &current_prefix);
        if (nal_start < 0) break;
        const int payload = nal_start + current_prefix;
        int next_prefix = 0;
        const int next = play_find_start_code(data, size, payload, &next_prefix);
        const int end = next >= 0 ? next : size;
        if (payload >= end) return 0;
        if (trace->nal_count < VDEC_PLAY_TRACE_NAL_CAP) {
            const int slot = trace->nal_count;
            trace->type[slot] = codec == VDEC_PLAY_CODEC_AVC ? data[payload] & 0x1f : (data[payload] >> 1) & 0x3f;
            trace->nal_size[slot] = end - payload;
            trace->offset[slot] = nal_start;
        } else {
            trace->truncated = 1;
        }
        ++trace->nal_count;
        cursor = next >= 0 ? next : size;
    }
    return trace->nal_count > 0;
}

static void play_packet_trace_capture(VdecPlayPacketTrace *trace, const AVPacket *packet, uint32_t codec,
                                      int nal_length_size, uint64_t index) {
    memset(trace, 0, sizeof(*trace));
    trace->index = index;
    trace->pts = packet->pts;
    trace->dts = packet->dts;
    trace->pos = packet->pos;
    trace->size = packet->size;
    trace->duration = (int)packet->duration;
    trace->flags = packet->flags;
    const uint8_t *data = packet->data;
    const int size = packet->size;
    if (!data || size <= 0) {
        trace->format = -1;
        return;
    }
    trace->format = 1;
    if (play_packet_trace_parse_avcc(trace, data, size, codec, nal_length_size)) return;
    trace->nal_count = 0;
    trace->truncated = 0;
    if (play_packet_trace_parse_annexb(trace, data, size, codec)) return;
    trace->format = -1;
    trace->truncated = 1;
}

static void play_packet_trace_record(VdecPlayMedia *media, const AVPacket *packet) {
    VdecPlayPacketTrace *trace = &media->trace[media->trace_head];
    play_packet_trace_capture(trace, packet, media->codec, media->nal_length_size, media->packet_index++);
    media->trace_head = (media->trace_head + 1) % VDEC_PLAY_TRACE_PACKET_CAP;
    if (media->trace_count < VDEC_PLAY_TRACE_PACKET_CAP) ++media->trace_count;
}

static void play_packet_trace_log(const char *label, const VdecPlayPacketTrace *trace) {
    char nals[256];
    int used = 0;
    nals[0] = '\0';
    for (int i = 0; i < trace->nal_count && i < VDEC_PLAY_TRACE_NAL_CAP && used < (int)sizeof(nals); ++i) {
        const int written = snprintf(nals + used, sizeof(nals) - (size_t)used, "%s%d/%d/%d", i ? "," : "",
                                     trace->type[i], trace->nal_size[i], trace->offset[i]);
        if (written < 0) break;
        used += written;
    }
    play_logf("vdec-play: %s idx=%llu pts=%lld dts=%lld pos=%lld size=%d duration=%d flags=0x%x format=%s len=%d declared=%u@%d nals=%d%s [%s]",
              label, (unsigned long long)trace->index, (long long)trace->pts, (long long)trace->dts,
              (long long)trace->pos, trace->size, trace->duration, trace->flags,
              trace->format == 1 ? "avcc" : trace->format == 2 ? "annexb" : "invalid", trace->length_size,
              trace->first_declared_size, trace->first_declared_offset, trace->nal_count,
              trace->truncated ? "+" : "", nals);
}

static void play_packet_trace_dump(const VdecPlayMedia *media) {
    const int first = (media->trace_head + VDEC_PLAY_TRACE_PACKET_CAP - media->trace_count) %
                      VDEC_PLAY_TRACE_PACKET_CAP;
    for (int i = 0; i < media->trace_count; ++i)
        play_packet_trace_log("bsf-last", &media->trace[(first + i) % VDEC_PLAY_TRACE_PACKET_CAP]);
}
static int play_choose_pending_limit(const VdecPlayMedia *media, const char **reason) {
    const int delay = media->video_delay;
    const int b_frames = media->has_b_frames;
    if (delay < 0 || b_frames < 0) {
        *reason = "reorder-info-unknown";
        return VDEC_PLAY_PENDING_LIMIT;
    }
    if (delay != b_frames || delay > VDEC_PLAY_PENDING_CAP - 4) {
        *reason = delay != b_frames ? "reorder-info-mismatch" : "reorder-info-too-large";
        return VDEC_PLAY_PENDING_LIMIT;
    }
    int limit = delay + (delay > 0 ? 4 : 2);
    if (limit < VDEC_PLAY_PENDING_MIN) limit = VDEC_PLAY_PENDING_MIN;
    *reason = delay == 0 ? "video-delay-plus2-floor4" : "video-delay-plus4-reorder";
    return limit > VDEC_PLAY_PENDING_CAP ? VDEC_PLAY_PENDING_CAP : limit;
}
static int play_interrupt(void *opaque) {
    const VdecPlaySession *s = (const VdecPlaySession *)opaque;
    return play_stop_requested(s) ||
           (__atomic_load_n(&s->seek_requested, __ATOMIC_ACQUIRE) && s->decoder_ready);
}

static void play_media_close(VdecPlayMedia *media) {
    if (!media) return;
    if (media->bsf) av_bsf_free(&media->bsf);
    if (media->format) avformat_close_input(&media->format);
    memset(media, 0, sizeof(*media));
}

static int play_bili_media_url(const char *url) {
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

static int play_json_space(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static int play_json_string(const char *begin, const char *end, const char *key, char *value, size_t capacity) {
    char needle[64];
    const int needle_length = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needle_length <= 0 || (size_t)needle_length >= sizeof(needle)) return 0;
    const char *cursor = begin;
    while (cursor < end) {
        const char *match = strstr(cursor, needle);
        if (match == NULL || match >= end) return 0;
        cursor = match + needle_length;
        while (cursor < end && play_json_space(*cursor)) ++cursor;
        if (cursor >= end || *cursor++ != ':') continue;
        while (cursor < end && play_json_space(*cursor)) ++cursor;
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

static int play_load_bili_cookie(char *cookie, size_t capacity) {
    cookie[0] = '\0';
    int fd = open("/download0/wiliwili/config/wiliwili_config.json", O_RDONLY, 0);
    if (fd < 0) return 0;
    char json[16384];
    const long size = read(fd, json, sizeof(json) - 1);
    close(fd);
    if (size <= 0 || size >= (long)sizeof(json)) return 0;
    json[size] = '\0';
    const char *cookie_key = strstr(json, "\"cookie\"");
    if (cookie_key == NULL) return 0;
    const char *object = strchr(cookie_key, '{');
    if (object == NULL) return 0;
    const char *object_end = strchr(object + 1, '}');
    if (object_end == NULL) return 0;
    static const char *const names[] = {"SESSDATA", "bili_jct", "DedeUserID", "DedeUserID__ckMd5",
                                        "_uuid", "buvid3", "sid"};
    size_t used = 0;
    int fields = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char value[512];
        if (!play_json_string(object + 1, object_end, names[i], value, sizeof(value)) || value[0] == '\0') continue;
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
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_delay_max", "1", 0);
    av_dict_set(&options, "reconnect_on_network_error", "1", 0);
    av_dict_set(&options, "multiple_requests", "1", 0);
    char cookie[2048];
    const int bili_url = play_bili_media_url(s->source_url);
    const int cookie_fields = bili_url ? play_load_bili_cookie(cookie, sizeof(cookie)) : 0;
    char headers[4096];
    const int header_length = snprintf(headers, sizeof(headers),
                                       "Referer: https://www.bilibili.com\r\nUser-Agent: Mozilla/5.0\r\n%s%s\r\n",
                                       cookie_fields > 0 ? "Cookie: " : "", cookie_fields > 0 ? cookie : "");
    if (header_length > 0 && (size_t)header_length < sizeof(headers))
        av_dict_set(&options, "headers", headers, 0);
    if (bili_url && cookie_fields > 0)
        play_logf("vdec-play: bili-cookie=config fields=%d", cookie_fields);
    else if (bili_url && cookie_fields < 0)
        play_logf("vdec-play: bili-cookie=config-too-large");
    else if (bili_url)
        play_logf("vdec-play: bili-cookie=config-missing");
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
    media->video_delay = stream->codecpar->video_delay;
    media->color_trc = stream->codecpar->color_trc;
    s->video_color_trc = media->color_trc > 0 ? media->color_trc : 1;
    media->has_reorder = media->video_delay > 0;
    int probe_has_b_frames = -1;
    int probe_delay = -1;
    int probe_refs = -1;
    int probe_ticks = -1;
    AVCodecContext *probe_context = avcodec_alloc_context3(NULL);
    if (probe_context != NULL) {
        if (avcodec_parameters_to_context(probe_context, stream->codecpar) == 0) {
            probe_has_b_frames = probe_context->has_b_frames;
            probe_delay = probe_context->delay;
            probe_refs = probe_context->refs;
            probe_ticks = probe_context->ticks_per_frame;
            if (probe_context->has_b_frames > 0) media->has_reorder = 1;
        }
        avcodec_free_context(&probe_context);
    }
    media->has_b_frames = probe_has_b_frames;
    play_logf("vdec-play: codec probe video_delay=%d has_b_frames=%d delay=%d refs=%d ticks_per_frame=%d",
              media->video_delay, media->has_b_frames, probe_delay, probe_refs, probe_ticks);
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
    media->nal_length_size = 4;
    if (stream->codecpar->extradata && stream->codecpar->extradata_size >= 5) {
        const int length_offset = stream->codecpar->codec_id == AV_CODEC_ID_HEVC ? 21 : 4;
        if (stream->codecpar->extradata_size > length_offset)
            media->nal_length_size = (stream->codecpar->extradata[length_offset] & 3) + 1;
        if (media->nal_length_size < 1 || media->nal_length_size > 4) media->nal_length_size = 4;
    }
    play_logf("vdec-play: demux ready stream=%d codec=%u size=%dx%d fps=%d/%d profile=%d format=%d bits=%d trc=%d "
              "timebase=%d/%d tag=0x%x extradata=%d nal_len=%d bsf=%s reorder=%d",
              stream_index, media->codec, stream->codecpar->width, stream->codecpar->height,
              stream->avg_frame_rate.num, stream->avg_frame_rate.den, stream->codecpar->profile, stream->codecpar->format,
              stream->codecpar->bits_per_raw_sample, media->color_trc, media->time_base.num, media->time_base.den,
              stream->codecpar->codec_tag, stream->codecpar->extradata_size, media->nal_length_size, filter_name,
              media->has_reorder);
    return 0;
}
static int play_auto_capable(const VdecPlayMedia *media, char *reason, size_t capacity) {
    if (!media || !media->stream || !media->stream->codecpar || !reason || capacity == 0) return 0;
    const AVCodecParameters *par = media->stream->codecpar;
    if (par->codec_id != AV_CODEC_ID_H264 && par->codec_id != AV_CODEC_ID_HEVC) {
        snprintf(reason, capacity, "codec=%d", par->codec_id);
        return 0;
    }
    if (par->width <= 0 || par->height <= 0 || par->width > 1920 || par->height > 1080) {
        snprintf(reason, capacity, "size=%dx%d", par->width, par->height);
        return 0;
    }
    if (par->bits_per_raw_sample > 8) {
        snprintf(reason, capacity, "raw-bits=%d", par->bits_per_raw_sample);
        return 0;
    }
    const AVPixFmtDescriptor *format = par->format >= 0 ? av_pix_fmt_desc_get((enum AVPixelFormat)par->format) : NULL;
    if (format && format->comp[0].depth > 8) {
        snprintf(reason, capacity, "pixel-format=%s depth=%d", format->name, format->comp[0].depth);
        return 0;
    }
    snprintf(reason, capacity, "codec=%u size=%dx%d raw_bits=%d coded_bits=%d reorder=%d", media->codec, par->width,
             par->height, par->bits_per_raw_sample, par->bits_per_coded_sample, media->has_reorder);
    return 1;
}

static int play_switch_to_backup(VdecPlaySession *s, VdecPlayMedia *media, int failure_rc) {
    if (!s || !media) return 0;
    for (;;) {
        char backup[sizeof(s->backup_urls[0])];
        int backup_index  = -1;
        int64_t target90k = 0;
        pthread_mutex_lock(&s->mutex);
        if (s->backup_index >= s->backup_count || play_stop_requested(s) || s->fallback) {
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
        backup_index = s->backup_index++;
        snprintf(backup, sizeof(backup), "%s", s->backup_urls[backup_index]);
        if (s->last_output_pts != INT64_MIN) target90k = s->last_output_pts;
        s->decoder_ready = 0;
        while (s->present_pending && !play_stop_requested(s) && !s->fallback)
            pthread_cond_wait(&s->condition, &s->mutex);
        if (play_stop_requested(s) || s->fallback) {
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
        snprintf(s->source_url, sizeof(s->source_url), "%s", backup);
        ++s->failover_count;
        pthread_mutex_unlock(&s->mutex);
        play_logf("vdec-play: backup-switch begin index=%d rc=%d target90k=%lld failovers=%llu", backup_index,
                  failure_rc, (long long)target90k, (unsigned long long)s->failover_count);

        const int32_t reset_rc = sceVideodec2Reset(s->decoder.decoder);
        if (reset_rc != 0) {
            play_logf("vdec-play: backup-switch reset failed index=%d rc=%d", backup_index, reset_rc);
            continue;
        }
        play_media_close(media);
        if (play_media_open(s, media) != 0) {
            play_logf("vdec-play: backup-switch open failed index=%d", backup_index);
            continue;
        }
        if (media->codec != s->decoder.codec || media->stream->codecpar->width != s->decoder.visible_width ||
            media->stream->codecpar->height != s->decoder.visible_height) {
            play_logf("vdec-play: backup-switch format mismatch index=%d codec=%u size=%dx%d", backup_index,
                      media->codec, media->stream->codecpar->width, media->stream->codecpar->height);
            play_media_close(media);
            continue;
        }
        const int64_t origin90k =
            media->stream->start_time != AV_NOPTS_VALUE
                ? av_rescale_q(media->stream->start_time, media->time_base, (AVRational){1, 90000})
                : 0;
        const int64_t target = av_rescale_q(origin90k + target90k, (AVRational){1, 90000}, media->time_base);
        int seek_rc          = 0;
        int sequential_seek  = 0;
        if (target90k > 0) {
            seek_rc = avformat_seek_file(media->format, media->stream->index, INT64_MIN, target, target, 0);
            if (seek_rc < 0) {
                avformat_flush(media->format);
                sequential_seek = 1;
            }
        }
        av_bsf_flush(media->bsf);
        pthread_mutex_lock(&s->mutex);
        if (play_stop_requested(s) || s->fallback) {
            pthread_mutex_unlock(&s->mutex);
            play_media_close(media);
            return 0;
        }
        s->timeline_origin_pts      = origin90k;
        s->first_pts                = origin90k;
        s->timeline_origin_set      = 1;
        s->last_output_pts          = INT64_MIN;
        s->last_pts                 = origin90k + target90k;
        s->last_clock_pts           = INT64_MIN;
        s->pending_count            = 0;
        s->current_slot             = -1;
        s->ready_count              = 0;
        s->present_pending          = 0;
        s->present_pending_since_us = 0;
        s->awaiting_idr             = 1;
        s->sps_seen                 = 0;
        s->pps_seen                 = 0;
        s->vps_seen                 = 0;
        s->source_eof               = 0;
        s->reopen_media             = 0;
        s->sequential_seek          = sequential_seek;
        s->sequential_seek_pts90k   = target90k;
        s->seek_requested           = 0;
        memset(s->pending_pts, 0, sizeof(s->pending_pts));
        memset(s->ready_queue, 0, sizeof(s->ready_queue));
        memset(s->slots, 0, sizeof(s->slots));
        memset(s->au_context, 0, sizeof(s->au_context));
        s->au_context_head  = 0;
        s->au_context_count = 0;
        s->decoder_ready    = 1;
        pthread_cond_broadcast(&s->condition);
        pthread_mutex_unlock(&s->mutex);
        play_logf("vdec-play: backup-switch success index=%d seek_rc=%d sequential=%d target90k=%lld", backup_index,
                  seek_rc, sequential_seek, (long long)target90k);
        return 1;
    }
}
static int play_packet_is_complete(const VdecPlayMedia *media, const AVPacket *packet) {
    if (!media || !packet || (media->codec != VDEC_PLAY_CODEC_AVC && media->codec != VDEC_PLAY_CODEC_HEVC)) return 1;
    VdecPlayPacketTrace trace;
    play_packet_trace_capture(&trace, packet, media->codec, media->nal_length_size, 0);
    return trace.format != -1;
}

static int play_recover_packet(VdecPlaySession *s, VdecPlayMedia *media, AVPacket *packet) {
    if (!s || !media || !media->format || !media->stream || !packet || packet->pos < 0 || packet->pts == AV_NOPTS_VALUE)
        return -1;
    const uint32_t wanted_codec = media->codec;
    const int wanted_width = media->stream->codecpar->width;
    const int wanted_height = media->stream->codecpar->height;
    const int64_t wanted_pos = packet->pos;
    const int64_t wanted_pts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
    AVPacket *candidate = av_packet_alloc();
    if (!candidate) return -1;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (attempt > 1) {
            play_media_close(media);
            if (play_media_open(s, media) != 0) break;
            if (media->codec != wanted_codec || media->stream->codecpar->width != wanted_width ||
                media->stream->codecpar->height != wanted_height) {
                play_media_close(media);
                break;
            }
        }
        av_packet_unref(packet);
        avformat_flush(media->format);
        const int rc = avformat_seek_file(media->format, media->stream->index, INT64_MIN, wanted_pts, INT64_MAX,
                                           AVSEEK_FLAG_ANY);
        play_logf("vdec-play: short-packet retry attempt=%d mode=%s seek_rc=%d pos=%lld pts=%lld", attempt,
                  attempt == 1 ? "existing" : "reopen", rc, (long long)wanted_pos, (long long)wanted_pts);
        if (rc < 0) continue;
        for (int reads = 0; reads < 8192; ++reads) {
            av_packet_unref(candidate);
            const int read_rc = av_read_frame(media->format, candidate);
            if (read_rc < 0) break;
            if (candidate->stream_index != media->stream->index) continue;
            if (candidate->pos == wanted_pos) {
                av_packet_move_ref(packet, candidate);
                if (play_packet_is_complete(media, packet)) {
                    play_logf("vdec-play: short-packet recovered attempt=%d pos=%lld size=%d", attempt,
                              (long long)packet->pos, packet->size);
                    av_packet_free(&candidate);
                    return 0;
                }
                av_packet_unref(packet);
                break;
            }
            if (candidate->pos > wanted_pos) break;
        }
    }
    av_packet_free(&candidate);
    return -1;
}

static int play_sequential_seek_skip(VdecPlaySession *s, const AVPacket *packet, AVRational time_base) {
    if (!packet || packet->pts == AV_NOPTS_VALUE) return 1;
    pthread_mutex_lock(&s->mutex);
    const int enabled = s->sequential_seek;
    const int64_t target90k = s->sequential_seek_pts90k;
    const int64_t origin90k = s->timeline_origin_pts;
    pthread_mutex_unlock(&s->mutex);
    if (!enabled) return 0;
    const int64_t pts90k = av_rescale_q(packet->pts, time_base, (AVRational){1, 90000}) - origin90k;
    const VdecPlayNalFlags nal = play_scan_nals(packet->data, packet->size, s->decoder.codec);
    if (pts90k < target90k || !nal.idr) return 1;
    pthread_mutex_lock(&s->mutex);
    if (s->sequential_seek) s->sequential_seek = 0;
    pthread_mutex_unlock(&s->mutex);
    play_logf("vdec-play: sequential seek reached pts90k=%lld target90k=%lld idr=%d",
              (long long)pts90k, (long long)target90k, nal.idr);
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
    int result = VDEC_PLAY_RESULT_OK;
    while (!play_stop_requested(s)) {
        play_wait_if_paused(s);
        if (play_stop_requested(s)) break;
        if (__atomic_load_n(&s->seek_requested, __ATOMIC_ACQUIRE)) {
            result = VDEC_PLAY_RESULT_SEEK;
            break;
        }
        const uint64_t read_begin = play_now_us();
        const int read_rc = av_read_frame(media->format, input);
        const uint64_t read_us    = play_now_us() - read_begin;
        pthread_mutex_lock(&s->mutex);
        s->last_demux_wait_us = read_us;
        if (read_us > s->max_demux_wait_us) s->max_demux_wait_us = read_us;
        ++s->demux_read_count;
        if (s->trace_latency && read_us >= 50000)
            play_logf("vdec-play: demux-wait us=%llu rc=%d input=%llu eof=%d seek=%d", (unsigned long long)read_us,
                      read_rc, (unsigned long long)s->input_count, s->source_eof, s->seek_requested);
        pthread_mutex_unlock(&s->mutex);
        if (read_rc == AVERROR_EOF) {
            pthread_mutex_lock(&s->mutex);
            s->source_eof = 1;
            const uint64_t inputs = s->input_count;
            const uint64_t outputs = s->output_count;
            pthread_mutex_unlock(&s->mutex);
            play_logf("vdec-play: demux EOF inputs=%llu outputs=%llu", (unsigned long long)inputs,
                      (unsigned long long)outputs);
            break;
        }
        if (read_rc < 0) {
            if (__atomic_load_n(&s->seek_requested, __ATOMIC_ACQUIRE)) {
                result = VDEC_PLAY_RESULT_SEEK;
                break;
            }
            if (play_stop_requested(s)) break;
            av_packet_unref(input);
            if (play_switch_to_backup(s, media, read_rc)) continue;
            play_log_av_error("read-frame", read_rc);
            play_fail(s, "demux-error", read_rc);
            result = -1;
            break;
        }
        if (input->stream_index != media->stream->index) {
            av_packet_unref(input);
            continue;
        }
        play_packet_trace_record(media, input);
        if (!play_packet_is_complete(media, input)) {
            play_packet_trace_log(
                "packet-short",
                &media->trace[(media->trace_head + VDEC_PLAY_TRACE_PACKET_CAP - 1) % VDEC_PLAY_TRACE_PACKET_CAP]);
            if (play_recover_packet(s, media, input) != 0) {
                play_packet_trace_dump(media);
                av_packet_unref(input);
                if (play_switch_to_backup(s, media, -9025)) continue;
                play_fail(s, "demux-short-packet", -9025);
                result = -1;
                break;
            }
            play_packet_trace_record(media, input);
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
                play_packet_trace_dump(media);
                play_fail(s, "bsf-error", rc);
                result = -1;
                goto done;
            }
            if (play_sequential_seek_skip(s, filtered, media->time_base)) {
                av_packet_unref(filtered);
                continue;
            }
            const int submit = play_submit_au(s, filtered->data, filtered->size, filtered->pts, filtered->dts,
                                              media->time_base, (filtered->flags & AV_PKT_FLAG_KEY) != 0);
            av_packet_unref(filtered);
            if (submit == VDEC_PLAY_RESULT_SEEK) {
                result = VDEC_PLAY_RESULT_SEEK;
                goto done;
            }
            if (submit < 0) {
                result = -1;
                goto done;
            }
        }
    }
flush_bsf:
    if (!play_stop_requested(s) && !s->fallback && !__atomic_load_n(&s->seek_requested, __ATOMIC_ACQUIRE)) {
        int rc = av_bsf_send_packet(media->bsf, NULL);
        if (rc < 0 && rc != AVERROR_EOF) {
            play_log_av_error("BSF flush", rc);
            play_fail(s, "bsf-flush-error", rc);
            result = -1;
        }
        while (result == VDEC_PLAY_RESULT_OK && !play_stop_requested(s) && !s->fallback &&
               !__atomic_load_n(&s->seek_requested, __ATOMIC_ACQUIRE)) {
            rc = av_bsf_receive_packet(media->bsf, filtered);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) {
                play_log_av_error("BSF flush receive", rc);
                play_fail(s, "bsf-flush-error", rc);
                result = -1;
                break;
            }
            if (play_sequential_seek_skip(s, filtered, media->time_base)) {
                av_packet_unref(filtered);
                continue;
            }
            const int submit = play_submit_au(s, filtered->data, filtered->size, filtered->pts, filtered->dts,
                                              media->time_base, (filtered->flags & AV_PKT_FLAG_KEY) != 0);
            av_packet_unref(filtered);
            if (submit == VDEC_PLAY_RESULT_SEEK) {
                result = VDEC_PLAY_RESULT_SEEK;
                break;
            }
            if (submit < 0) result = -1;
        }
        if (result == VDEC_PLAY_RESULT_OK && !s->fallback && !play_stop_requested(s) && play_flush(s) != 0)
            result = -1;
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
    while (__atomic_load_n(&s->paused, __ATOMIC_ACQUIRE) && !play_stop_requested(s) && !s->fallback &&
           !s->seek_requested)
        pthread_cond_wait(&s->condition, &s->mutex);
    pthread_mutex_unlock(&s->mutex);
}

static int play_restart_after_seek(VdecPlaySession *s, VdecPlayMedia *media) {
    double seconds = 0.0;
    int reopen_media = 0;
    int64_t current_pts90k = INT64_MIN;
    pthread_mutex_lock(&s->mutex);
    seconds = s->seek_seconds;
    reopen_media = s->source_eof || s->reopen_media;
    if (s->output_count > 0 || s->input_count > 0) current_pts90k = s->last_pts - s->timeline_origin_pts;
    const int64_t requested_target90k = (int64_t)llround(seconds * 90000.0);
    if (current_pts90k != INT64_MIN && requested_target90k < current_pts90k) reopen_media = 1;
    s->reopen_media = 0;
    s->seek_requested = 0;
    while (s->present_pending && !play_stop_requested(s) && !s->fallback)
        pthread_cond_wait(&s->condition, &s->mutex);
    const int cancelled = play_stop_requested(s) || s->fallback;
    pthread_mutex_unlock(&s->mutex);
    if (cancelled) return -1;

    int32_t rc = sceVideodec2Reset(s->decoder.decoder);
    play_logf("vdec-play: seek reset target=%.3f rc=%d reopen=%d", seconds, rc, reopen_media);
    if (rc != 0) {
        play_fail(s, "seek-reset", rc);
        return -1;
    }
    if (reopen_media) {
        /* EOF or a backward seek can leave the HTTP/MP4 AVIO demuxer in a stale
         * range state; reopen before seeking, while ordinary forward seeks reuse it. */
        play_media_close(media);
        if (play_media_open(s, media) != 0) {
            play_fail(s, "replay-open", -9022);
            return -1;
        }
        if (media->codec != s->decoder.codec || media->stream->codecpar->width != s->decoder.visible_width ||
            media->stream->codecpar->height != s->decoder.visible_height) {
            play_media_close(media);
            play_fail(s, "replay-format", -9023);
            return -1;
        }
        s->flush_each_decode = !media->has_reorder;
        play_logf("vdec-play: replay media reopened codec=%u size=%dx%d flush_each_decode=%d", media->codec,
                  media->stream->codecpar->width, media->stream->codecpar->height, s->flush_each_decode);
    }
    const int64_t target90k = (int64_t)llround(seconds * 90000.0);
    const int64_t target = av_rescale_q(target90k, (AVRational){1, 90000}, media->time_base);
    int sequential_seek = 0;
    if (!reopen_media || target90k != 0) {
        rc = avformat_seek_file(media->format, media->stream->index, INT64_MIN, target, target, 0);
        if (rc < 0) {
            play_log_av_error("seek", rc);
            if (!reopen_media && current_pts90k != INT64_MIN && target90k >= current_pts90k) {
                avformat_flush(media->format);
                sequential_seek = 1;
                play_logf("vdec-play: seek demux unavailable rc=%d; sequential-forward current90k=%lld target90k=%lld",
                          rc, (long long)current_pts90k, (long long)target90k);
            } else {
                play_fail(s, "seek-demux", rc);
                return -1;
            }
        }
    }
    av_bsf_flush(media->bsf);
    pthread_mutex_lock(&s->mutex);
    if (!s->timeline_origin_set) {
        s->timeline_origin_pts = media->stream->start_time != AV_NOPTS_VALUE
                                     ? av_rescale_q(media->stream->start_time, media->time_base, (AVRational){1, 90000})
                                     : 0;
        s->first_pts = s->timeline_origin_pts;
        s->timeline_origin_set = 1;
    }
    ++s->seek_generation;
    s->source_eof = 0;
    s->sequential_seek = sequential_seek;
    s->sequential_seek_pts90k = sequential_seek ? target90k : 0;
    s->current_slot = -1;
    s->ready_count = 0;
    s->present_pending = 0;
    s->pending_count = 0;
    s->peak_pending_count = 0;
    s->last_output_pts = INT64_MIN;
    s->last_pts = 0;
    s->last_clock_pts = INT64_MIN;
    s->awaiting_idr = 1;
    s->sps_seen = 0;
    s->pps_seen = 0;
    s->vps_seen = 0;
    s->input_count = 0;
    s->accepted_count = 0;
    s->output_count = 0;
    s->presented_count = 0;
    s->retired_count = 0;
    s->sequence = 0;
    s->advanced_count = 0;
    s->dropped_count = 0;
    memset(s->ready_queue, 0, sizeof(s->ready_queue));
    memset(s->slots, 0, sizeof(s->slots));
    pthread_cond_broadcast(&s->condition);
    const uint64_t generation = s->seek_generation;
    pthread_mutex_unlock(&s->mutex);
    play_logf("vdec-play: seek ready generation=%llu target=%.3f target90k=%lld; awaiting true IDR",
              (unsigned long long)generation, seconds, (long long)target90k);
    return 0;
}
static void *play_thread_main(void *opaque) {
    VdecPlaySession *s = (VdecPlaySession *)opaque;
    VdecPlayMedia media;
    memset(&media, 0, sizeof(media));
    int result = -1;
    const int media_ok = play_media_open(s, &media) == 0;
    int auto_gate_ok = 1;
    if (media_ok) {
        const int mode = play_effective_mode();
        if (mode == VDEC_PLAY_MODE_AUTO) {
            char reason[128];
            auto_gate_ok = play_auto_capable(&media, reason, sizeof(reason));
            if (auto_gate_ok) {
                play_logf("vdec-play: auto-gate=pass %s", reason);
            } else {
                play_logf("vdec-play: auto-gate=A reason=%s", reason);
                pthread_mutex_lock(&s->mutex);
                s->active = 0;
                s->stop_requested = 1;
                pthread_cond_broadcast(&s->condition);
                pthread_mutex_unlock(&s->mutex);
            }
        } else {
            play_logf("vdec-play: capability gate bypassed mode=on");
        }
        if (auto_gate_ok) {
            const char *policy_reason = "fixed-safe-default";
            pthread_mutex_lock(&s->mutex);
            if (!s->pending_limit_override && s->adaptive_pending)
                s->pending_limit = play_choose_pending_limit(&media, &policy_reason);
            const int policy_limit = s->pending_limit;
            const int policy_override = s->pending_limit_override;
            const int policy_adaptive = s->adaptive_pending;
            pthread_mutex_unlock(&s->mutex);
            play_logf("vdec-play: pending policy=%s limit=%d video_delay=%d has_b_frames=%d peak=%d reason=%s",
                      policy_override ? "override" : policy_adaptive ? "adaptive" : "fixed", policy_limit,
                      media.video_delay, media.has_b_frames, 0, policy_reason);
            s->flush_each_decode = !media.has_reorder;
            play_logf("vdec-play: flush_each_decode=%d", s->flush_each_decode);
        }
    }
    const int decoder_ok = media_ok && auto_gate_ok && !play_stop_requested(s) && !s->fallback &&
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
        while (!play_stop_requested(s) && !s->fallback) {
            if (__atomic_load_n(&s->seek_requested, __ATOMIC_ACQUIRE) && play_restart_after_seek(s, &media) != 0) {
                result = -1;
                break;
            }
            result = play_media_loop(s, &media);
            if (result == VDEC_PLAY_RESULT_SEEK) {
                if (play_restart_after_seek(s, &media) != 0) {
                    result = -1;
                    break;
                }
                continue;
            }
            if (result != VDEC_PLAY_RESULT_OK || play_stop_requested(s) || s->fallback) break;

            pthread_mutex_lock(&s->mutex);
            if (s->input_count == 0 || s->output_count != s->accepted_count || s->pending_count != 0) {
                play_fail_locked(s, "stream-count-mismatch", -9019);
                pthread_mutex_unlock(&s->mutex);
                result = -1;
                break;
            }
            s->source_eof = 1;
            const uint64_t inputs = s->input_count;
            const uint64_t accepted = s->accepted_count;
            const uint64_t outputs = s->output_count;
            play_logf("vdec-play: EOF inputs=%llu accepted=%llu outputs=%llu limit=%d peak=%d errors=%llu order_errors=%llu",
                      (unsigned long long)inputs, (unsigned long long)accepted, (unsigned long long)outputs,
                      s->pending_limit, s->peak_pending_count, (unsigned long long)s->error_count,
                      (unsigned long long)s->order_error_count);
            if (play_gate_value("WILIWILI_VDEC_STRESS") && s->stress_eof_count < 5) {
                ++s->stress_eof_count;
                s->seek_seconds = 0.0;
                s->reopen_media = 1;
                s->seek_requested = 1;
                play_logf("m5-stress: eof=%d/5", s->stress_eof_count);
            }
            while (!play_stop_requested(s) && !s->fallback && !s->seek_requested)
                pthread_cond_wait(&s->condition, &s->mutex);
            const int restart = !play_stop_requested(s) && !s->fallback && s->seek_requested;
            pthread_mutex_unlock(&s->mutex);
            if (!restart) break;
            if (play_restart_after_seek(s, &media) != 0) {
                result = -1;
                break;
            }
        }
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
    s->seek_requested = 0;
    s->active = 0;
    s->fallback = 0;
    s->failure_logged = 0;
    s->resources_live = 0;
    s->decoder_ready = 0;
    s->source_eof = 0;
    s->reopen_media = 0;
    s->sequential_seek = 0;
    s->sequential_seek_pts90k = 0;
    s->ready_count = 0;
    s->present_pending = 0;
    s->present_pending_since_us = 0;
    s->timeout_ms = VDEC_PLAY_TIMEOUT_MS_DEFAULT;
    s->flush_each_decode = 1;
    s->pending_limit = VDEC_PLAY_PENDING_LIMIT;
    s->pending_limit_override = 0;
    s->adaptive_pending = 1;
    s->p010_enabled = 0;
    s->video_color_trc = 1;
    s->trace_au = 0;
    s->trace_latency            = 0;
    s->seek_seconds = 0.0;
    s->seek_generation = 0;
    s->stress_eof_count = 0;
    s->pending_count = 0;
    s->peak_pending_count = 0;
    s->last_output_pts = INT64_MIN;
    s->first_pts = 0;
    s->last_pts = 0;
    s->timeline_origin_pts = 0;
    s->last_clock_pts = INT64_MIN;
    s->timeline_origin_set = 0;
    s->awaiting_idr = 1;
    s->sps_seen = 0;
    s->pps_seen = 0;
    s->vps_seen = 0;
    s->last_logged_speed = -1.0;
    s->direct_mem_peak          = 0;
    s->last_demux_wait_us       = 0;
    s->max_demux_wait_us        = 0;
    s->demux_read_count         = 0;
    s->last_slot_wait_us        = 0;
    s->max_slot_wait_us         = 0;
    s->slot_wait_count          = 0;
    s->max_present_wait_us      = 0;
    s->last_call_stage          = VDEC_PLAY_STAGE_NONE;
    s->last_call_slot           = -1;
    s->last_call_flush_index    = -1;
    s->last_call_rc             = 0;
    s->last_call_valid          = 0;
    s->last_call_error          = 0;
    s->last_call_accepted       = 0;
    s->last_call_did_flush      = 0;
    s->last_call_au_index       = UINT64_MAX;
    s->last_call_decode_us      = 0;
    s->last_call_flush_us       = 0;
    s->last_call_elapsed_us     = 0;
    s->failover_count           = 0;
    s->input_count = 0;
    s->accepted_count = 0;
    s->output_count = 0;
    s->presented_count = 0;
    s->retired_count = 0;
    s->sequence = 0;
    s->advanced_count = 0;
    s->dropped_count = 0;
    memset(s->ready_queue, 0, sizeof(s->ready_queue));
    memset(s->pending_pts, 0, sizeof(s->pending_pts));
    memset(s->slots, 0, sizeof(s->slots));
    memset(s->au_context, 0, sizeof(s->au_context));
    s->au_context_head  = 0;
    s->au_context_count = 0;
    memset(&s->latency, 0, sizeof(s->latency));
    s->last_sample_clock_pts = INT64_MIN;
    s->last_sample_wall_us = 0;
    s->last_clock_input_pts = INT64_MIN;
    s->last_clock_input_us = 0;
    s->clock_prediction_valid = 0;
    memset(s->source_url, 0, sizeof(s->source_url));
    memset(s->backup_urls, 0, sizeof(s->backup_urls));
    s->backup_count = 0;
    s->backup_index = 0;
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
    play_logf(
        "vdec-play: stop summary inputs=%llu accepted=%llu outputs=%llu presented=%llu retired=%llu dropped=%llu "
        "errors=%llu order_errors=%llu failovers=%llu direct_mem=%llu",
        (unsigned long long)s->input_count, (unsigned long long)s->accepted_count, (unsigned long long)s->output_count,
        (unsigned long long)s->presented_count, (unsigned long long)s->retired_count,
        (unsigned long long)s->dropped_count, (unsigned long long)s->error_count,
        (unsigned long long)s->order_error_count, (unsigned long long)s->failover_count,
        (unsigned long long)play_direct_mem_bytes(s));
    if (s->trace_latency) play_latency_report(s, "stop");
    if (s->resources_live) play_decoder_close(&s->decoder);
    s->thread_started = 0;
    play_reset_state_locked(s);
    pthread_mutex_unlock(&s->mutex);
}
void wiliwili_vdec_play_set_mode(int mode) {
    if (mode < VDEC_PLAY_MODE_AUTO || mode > VDEC_PLAY_MODE_OFF) mode = VDEC_PLAY_MODE_AUTO;
    __atomic_store_n(&g_vdec_user_mode, mode, __ATOMIC_RELEASE);
#if defined(PS5_NATIVE_APP) && defined(BOREALIS_USE_AGC)
    if (mode == VDEC_PLAY_MODE_OFF) {
        VdecPlaySession *s = &g_vdec_play;
        pthread_mutex_lock(&s->mutex);
        const int active = s->active;
        pthread_mutex_unlock(&s->mutex);
        if (active) wiliwili_vdec_play_stop();
    }
    play_logf("vdec-play: user mode=%d", mode);
#endif
}


int wiliwili_vdec_play_start(const char *requested_url, int start_seconds) {
    if (!wiliwili_vdec_play_enabled() || requested_url == NULL || requested_url[0] == '\0') return 0;
    wiliwili_vdec_play_stop();
    const char *override_url = getenv("WILIWILI_VDEC_URL");
    const char *switch_url = getenv("WILIWILI_VDEC_SWITCH_URL");
    const int is_switch_request = switch_url != NULL && strcmp(requested_url, switch_url) == 0;
    const char *source = !is_switch_request && override_url != NULL && override_url[0] != '\0' ? override_url : requested_url;
    VdecPlaySession *s = &g_vdec_play;
    pthread_mutex_lock(&s->mutex);
    if (strlen(source) >= sizeof(s->source_url)) {
        pthread_mutex_unlock(&s->mutex);
        play_logf("vdec-play: URL too long; keep A");
        return 0;
    }
    play_reset_state_locked(s);
    snprintf(s->source_url, sizeof(s->source_url), "%s", source);
    const char *backup_url = getenv("WILIWILI_VDEC_BACKUP_URL");
    if (backup_url != NULL && backup_url[0] != '\0' && !play_add_backup_url_locked(s, backup_url))
        play_logf("vdec-play: backup URL rejected");
    const char *inject = getenv("WILIWILI_VDEC_INJECT");
    if (inject) snprintf(s->inject, sizeof(s->inject), "%s", inject);
    const char *timeout = getenv("WILIWILI_VDEC_TIMEOUT_MS");
    if (timeout && atoi(timeout) > 0) s->timeout_ms = atoi(timeout);
    s->trace_au = play_gate_value("WILIWILI_VDEC_TRACE_AU");
    s->trace_latency    = play_gate_value("WILIWILI_VDEC_TRACE_LATENCY");
    s->adaptive_pending = 1;
    if (play_gate_value("WILIWILI_VDEC_FIXED_PENDING")) s->adaptive_pending = 0;
    s->p010_enabled = play_gate_value("WILIWILI_VDEC_P010");
    play_logf("vdec-play: p010-present=%d", s->p010_enabled);
    const char *pending_limit = getenv("WILIWILI_VDEC_PENDING_LIMIT");
    if (pending_limit != NULL) {
        const int requested_limit = atoi(pending_limit);
        if (requested_limit >= VDEC_PLAY_PENDING_MIN && requested_limit <= VDEC_PLAY_PENDING_CAP) {
            s->pending_limit = requested_limit;
            s->pending_limit_override = 1;
            play_logf("vdec-play: pending_limit override=%d", s->pending_limit);
        } else {
            play_logf("vdec-play: pending_limit request=%d rejected; safe default remains=%d", requested_limit,
                      s->pending_limit);
        }
    }
    if (start_seconds > 0) {
        s->seek_requested = 1;
        s->seek_seconds = start_seconds;
    }
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
    play_logf(
        "vdec-play: start source=%s inject=%s timeout_ms=%d pending_limit=%d adaptive_pending=%d p010=%d start=%d "
        "trace_au=%d trace_latency=%d backups=%d dts=UINT64_MAX",
        s->source_url, s->inject[0] ? s->inject : "none", s->timeout_ms, s->pending_limit, s->adaptive_pending,
        s->p010_enabled, start_seconds, s->trace_au, s->trace_latency, s->backup_count);
    return 1;
}

void wiliwili_vdec_play_add_backup_url(const char *url) {
    if (!wiliwili_vdec_play_enabled() || url == NULL || url[0] == '\0') return;
    VdecPlaySession *s = &g_vdec_play;
    pthread_mutex_lock(&s->mutex);
    if (s->active && play_add_backup_url_locked(s, url))
        play_logf("vdec-play: backup URL registered index=%d", s->backup_count - 1);
    pthread_mutex_unlock(&s->mutex);
}

int wiliwili_vdec_play_seek(double seconds) {
    if (!wiliwili_vdec_play_enabled() || !isfinite(seconds)) return 0;
    if (seconds < 0.0) seconds = 0.0;
    VdecPlaySession *s = &g_vdec_play;
    pthread_mutex_lock(&s->mutex);
    if (!s->active || s->fallback) {
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    s->seek_seconds = seconds;
    s->last_clock_pts = INT64_MIN;
    s->last_clock_input_pts = INT64_MIN;
    s->last_clock_input_us = 0;
    s->clock_prediction_valid = 0;
    s->seek_requested = 1;
    pthread_cond_broadcast(&s->condition);
    pthread_mutex_unlock(&s->mutex);
    play_logf("vdec-play: seek requested target=%.3f", seconds);
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

static uint8_t play_p010_to_u8(uint16_t sample) {
    const unsigned value = (unsigned)(sample >> 6);
    const unsigned rounded = (value + 2u) >> 2;
    return (uint8_t)(rounded > 255u ? 255u : rounded);
}

static void play_p010_to_nv12(const uint8_t *src, int src_pitch_bytes, uint8_t *dst, int dst_pitch_bytes,
                              int width, int height) {
    const uint8_t *src_uv = src + (size_t)src_pitch_bytes * height;
    uint8_t *dst_uv = dst + (size_t)dst_pitch_bytes * height;
    for (int row = 0; row < height; ++row) {
        const uint16_t *src_row = (const uint16_t *)(src + (size_t)row * src_pitch_bytes);
        uint8_t *dst_row = dst + (size_t)row * dst_pitch_bytes;
        for (int x = 0; x < width; ++x) dst_row[x] = play_p010_to_u8(src_row[x]);
    }
    for (int row = 0; row < (height + 1) / 2; ++row) {
        const uint16_t *src_row = (const uint16_t *)(src_uv + (size_t)row * src_pitch_bytes);
        uint8_t *dst_row = dst_uv + (size_t)row * dst_pitch_bytes;
        for (int x = 0; x < width; ++x) dst_row[x] = play_p010_to_u8(src_row[x]);
    }
}
void wiliwili_vdec_p010_to_nv12(const uint8_t *src, int src_pitch_bytes, uint8_t *dst, int dst_pitch_bytes,
                                int width, int height) {
    play_p010_to_nv12(src, src_pitch_bytes, dst, dst_pitch_bytes, width, height);
}

int wiliwili_vdec_play_draw(double playback_time, double speed, int paused, int view_x, int view_y, int view_w,
                            int view_h, int view_mode, double audio_pts) {
    VdecPlaySession *s = &g_vdec_play;
    if (!isfinite(playback_time)) {
        pthread_mutex_lock(&s->mutex);
        play_fail_locked(s, "clock-invalid", -9021);
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    const int64_t raw_clock_pts90k = (int64_t)llround(playback_time * 90000.0);
    int64_t clock_pts90k = raw_clock_pts90k;
    const uint64_t clock_now_us = play_now_us();
    pthread_mutex_lock(&s->mutex);
    if (!s->active || s->fallback) {
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    if (!s->decoder_ready || !s->resources_live) {
        pthread_mutex_unlock(&s->mutex);
        return 1;
    }
    if (s->last_clock_input_pts == INT64_MIN) {
        s->last_clock_input_pts = raw_clock_pts90k;
        s->last_clock_input_us = clock_now_us;
        s->clock_prediction_valid = 0;
    } else if (raw_clock_pts90k != s->last_clock_input_pts) {
        s->clock_prediction_valid = llabs(raw_clock_pts90k - s->last_clock_input_pts) <= VDEC_PLAY_CLOCK_JUMP_90K;
        s->last_clock_input_pts = raw_clock_pts90k;
        s->last_clock_input_us = clock_now_us;
    }
    if (!paused && speed > 0.0 && s->clock_prediction_valid && clock_now_us >= s->last_clock_input_us) {
        uint64_t elapsed_us = clock_now_us - s->last_clock_input_us;
        if (elapsed_us > 250000) elapsed_us = 250000;
        clock_pts90k = raw_clock_pts90k + (int64_t)llround((double)elapsed_us * speed * 90000.0 / 1000000.0);
    }
    if (s->trace_au && (s->last_logged_speed < 0.0 || fabs(s->last_logged_speed - speed) > 0.0001)) {
        play_logf("vdec-play: clock playback=%.3f speed=%.3f paused=%d", playback_time, speed, paused != 0);
        s->last_logged_speed = speed;
    }
    if (s->trace_latency) {
        const uint64_t sample_wall_us = clock_now_us;
        const uint64_t interval_us = s->last_sample_wall_us != 0 && sample_wall_us >= s->last_sample_wall_us
                                         ? sample_wall_us - s->last_sample_wall_us
                                         : 0;
        const uint64_t step_us = s->last_sample_clock_pts != INT64_MIN
                                     ? (uint64_t)(llabs(raw_clock_pts90k - s->last_sample_clock_pts) * 1000000LL / 90000LL)
                                     : 0;
        uint64_t audio_phase_us = UINT64_MAX;
        if (isfinite(audio_pts) && audio_pts >= 0.0) {
            const int64_t audio_pts90k = (int64_t)llround(audio_pts * 90000.0);
            audio_phase_us = (uint64_t)(llabs(raw_clock_pts90k - audio_pts90k) * 1000000LL / 90000LL);
        }
        if (s->last_sample_wall_us != 0) play_latency_record_clock(s, step_us, interval_us, audio_phase_us);
        s->last_sample_clock_pts = raw_clock_pts90k;
        s->last_sample_wall_us = sample_wall_us;
    }
    /* mpv may reload its separate audio file at EOF and reset playback-time; restart the
     * native timeline instead of leaving the decoder blocked behind stale READY frames. */
    if (s->last_clock_pts != INT64_MIN && llabs(clock_pts90k - s->last_clock_pts) > VDEC_PLAY_CLOCK_JUMP_90K &&
        !s->seek_requested) {
        const int64_t previous_clock = s->last_clock_pts;
        s->seek_seconds = playback_time;
        if (clock_pts90k < previous_clock && clock_pts90k <= VDEC_PLAY_CLOCK_JUMP_90K)
            s->reopen_media = 1;
        s->seek_requested = 1;
        s->last_clock_pts = INT64_MIN;
        pthread_cond_broadcast(&s->condition);
        s->last_clock_input_pts = INT64_MIN;
        s->last_clock_input_us = 0;
        s->clock_prediction_valid = 0;
        play_logf("vdec-play: clock discontinuity old90k=%lld new90k=%lld; seek requested target=%.3f reopen=%d",
                  (long long)previous_clock, (long long)clock_pts90k, playback_time, s->reopen_media);
        pthread_mutex_unlock(&s->mutex);
        return 1;
    }
    s->last_clock_pts = clock_pts90k;
    int candidate = -1;
    for (int i = 0; i < s->ready_count; ++i) {
        const int ready_slot = s->ready_queue[i];
        if ((!paused && s->slots[ready_slot].pts90k <= clock_pts90k) || (s->current_slot < 0 && i == 0)) {
            candidate = i;
            break;
        }
    }
    int64_t selected_pts = INT64_MIN;
    int64_t future_pts = INT64_MIN;
    if (candidate >= 0) {
        selected_pts = s->slots[s->ready_queue[candidate]].pts90k;
        if (candidate + 1 < s->ready_count) future_pts = s->slots[s->ready_queue[candidate + 1]].pts90k;
    }
    if (candidate >= 0 && s->trace_latency) {
            const uint64_t age_us = selected_pts != INT64_MIN && selected_pts <= clock_pts90k
                                        ? (uint64_t)((clock_pts90k - selected_pts) * 1000000LL / 90000LL)
                                        : 0;
            const uint64_t future_us = future_pts != INT64_MIN && future_pts > clock_pts90k
                                           ? (uint64_t)((future_pts - clock_pts90k) * 1000000LL / 90000LL)
                                           : 0;
            play_latency_record_publish(s, age_us, future_us);
        }
    if (candidate >= 0) {
        for (int i = 0; i < candidate; ++i) {
            play_release_slot_locked(s, s->ready_queue[i]);
            ++s->dropped_count;
        }
        const int next = s->ready_queue[candidate];
        int remaining = 0;
        for (int i = candidate + 1; i < s->ready_count; ++i)
            s->ready_queue[remaining++] = s->ready_queue[i];
        s->ready_count = remaining;
        if (s->current_slot >= 0) play_release_slot_locked(s, s->current_slot);
        s->current_slot = next;
        s->slots[next].state = VDEC_PLAY_SLOT_CURRENT;
        ++s->advanced_count;
    }
    const int slot = s->current_slot;
    if (slot < 0) {
        pthread_mutex_unlock(&s->mutex);
        return 1;
    }
    VdecPlaySlot *frame = &s->slots[slot];
    const uint8_t *source = (const uint8_t *)s->decoder.frame_pool.address +
                             (size_t)slot * s->decoder.frame_size;
    const uint8_t *y = source;
    const uint8_t *uv = source + (size_t)frame->pitch_bytes * frame->height;
    int y_pitch = frame->pitch_bytes;
    int uv_pitch = frame->pitch_bytes;
    int ten_bit = 0;
    int color_trc = 1;
    const int native_p010_hdr = frame->p010 && (s->video_color_trc == 16 || s->video_color_trc == 18);
    if (frame->p010 && !native_p010_hdr) {
        if (!s->decoder.p010_pool.address) {
            play_fail_locked(s, "p010-conversion-pool", -9027);
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
        uint8_t *converted = (uint8_t *)s->decoder.p010_pool.address + (size_t)slot * s->decoder.frame_size;
        play_p010_to_nv12(source, frame->pitch_bytes, converted, frame->pitch, frame->width, frame->height);
        y = converted;
        uv = converted + (size_t)frame->pitch * frame->height;
        y_pitch = frame->pitch;
        uv_pitch = frame->pitch;
    } else if (native_p010_hdr) {
        ten_bit = 1;
        color_trc = s->video_color_trc;
    }
    const int rc = evo_agc_blit_yuv_rect(y, y_pitch, uv, uv_pitch, NULL, 0, NULL, 0, frame->width, frame->height,
                                         s->decoder.visible_width, s->decoder.visible_height, view_x, view_y, view_w,
                                         view_h, view_mode, ten_bit, color_trc, 0, frame->pts90k * 1000000 / 90000);
    if (rc != 0) {
        play_fail_locked(s, "agc-blit", rc);
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    if (s->presented_count >= 5) {
        const uint64_t drift90k = frame->pts90k >= clock_pts90k ? (uint64_t)(frame->pts90k - clock_pts90k)
                                                                : (uint64_t)(clock_pts90k - frame->pts90k);
        play_latency_record_drift(s, drift90k * 1000000u / 90000u);
    }
    if (!s->present_pending) s->present_pending_since_us = play_now_us();
    s->present_pending = 1;
    const uint64_t direct_mem = play_direct_mem_bytes(s);
    if (direct_mem > s->direct_mem_peak) s->direct_mem_peak = direct_mem;
    ++s->presented_count;
    if (s->presented_count == 1 || s->presented_count % 120 == 0)
        play_logf(
            "vdec-play: presented=%llu pts90k=%lld clock90k=%lld slot=%d retired=%llu advanced=%llu dropped=%llu "
            "eof=%d limit=%d peak=%d pending=%d inputs=%llu accepted=%llu outputs=%llu errors=%llu order_errors=%llu",
                  (unsigned long long)s->presented_count, (long long)frame->pts90k, (long long)clock_pts90k, slot,
                  (unsigned long long)s->retired_count, (unsigned long long)s->advanced_count,
                  (unsigned long long)s->dropped_count, s->source_eof, s->pending_limit, s->peak_pending_count,
                  s->pending_count, (unsigned long long)s->input_count, (unsigned long long)s->accepted_count,
                  (unsigned long long)s->output_count, (unsigned long long)s->error_count,
                  (unsigned long long)s->order_error_count);
    pthread_mutex_unlock(&s->mutex);
    return 1;
}

void wiliwili_vdec_play_restore_ui_state(void) {
    evo_agc_runtime_restore_ui_state();
}

void wiliwili_vdec_play_frame_retire(void) {
    VdecPlaySession *s = &g_vdec_play;
    pthread_mutex_lock(&s->mutex);
    if (s->present_pending) {
        const uint64_t wait_us = s->present_pending_since_us != 0 ? play_now_us() - s->present_pending_since_us : 0;
        if (wait_us > s->max_present_wait_us) s->max_present_wait_us = wait_us;
        if (s->trace_latency && wait_us >= 50000)
            play_logf("vdec-play: present-wait us=%llu retired=%llu", (unsigned long long)wait_us,
                      (unsigned long long)s->retired_count);
        s->present_pending_since_us = 0;
        s->present_pending = 0;
        ++s->retired_count;
        pthread_cond_broadcast(&s->condition);
    }
    pthread_mutex_unlock(&s->mutex);
}

#else

int wiliwili_vdec_play_start(const char *url, int start_seconds) {
    (void)url;
    (void)start_seconds;
    return 0;
}
void wiliwili_vdec_play_add_backup_url(const char *url) { (void)url; }
int wiliwili_vdec_play_seek(double seconds) {
    (void)seconds;
    return 0;
}
void wiliwili_vdec_play_stop(void) {}
void wiliwili_vdec_play_pause(int paused) { (void)paused; }
int wiliwili_vdec_play_draw(double playback_time, double speed, int paused, int view_x, int view_y, int view_w,
                            int view_h, int view_mode, double audio_pts) {
    (void)playback_time;
    (void)speed;
    (void)paused;
    (void)view_x;
    (void)view_y;
    (void)view_w;
    (void)view_h;
    (void)view_mode;
    (void)audio_pts;
    return 0;
}
void wiliwili_vdec_play_frame_retire(void) {}

#endif
