/*
 * sceVideodec2 硬解探针（原生标题 app slot 专用）。
 *
 * P2a（已通过，2026-09-26 真机）：完整 bring-up + 喂一帧 IDR ⇒ 全部 rc=0、出 NV12 帧。
 * P2b（历史）：连续解 Annex-B 流并把 NV12 Y/UV 当纹理绘制，验证呈现代价。
 * P0（2026-10-05）：`WILIWILI_TEST_VDEC=1` + `WILIWILI_VDEC_P0=1` 启用多格式
 *     能力/内存/Decode P95 统计；`WILIWILI_VDEC_CODEC=avc|hevc`、
 *     `WILIWILI_VDEC_MAIN10=1`、`WILIWILI_VDEC_PATH`、`WILIWILI_VDEC_WIDTH/HEIGHT`
 *     选择样本。P0 不复制/绘制 4K/P010 帧，默认正式标题路径不触发探针。
 * PTS 闸门（2026-10-06）：再加 `WILIWILI_VDEC_ORDER=1` 与
 *     `WILIWILI_VDEC_META=/app0/assets/vdec-reorder.txt`，读取真实 FFmpeg
 *     packet PTS/DTS，按输出 Y 平面识别带 B 帧的显示帧，并在 segment 变化时
 *     执行 Flush→Reset。`WILIWILI_VDEC_DTS_UNKNOWN=1` 按 ABI 传 `UINT64_MAX`；不设时
 *     传 sidecar 的真实 DTS（仅作反例对照）。它仍必须同时受 TEST_VDEC/P0 门控。
 *
 * 序列、结构体与常量取自 EVO-PLAYER-PS5 的 sce_videodec2.h 与 videodec2-abi.md（GPL-3.0）。
 * 默认触发：assets/wiliwili-options.txt 的 WILIWILI_TEST_VDEC=1；片源为 H.264 诊断流。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

struct NVGcontext;

extern void wiliwili_boot_log(const char *message);
extern int wiliwili_trace_enabled(void);
extern void *SDL_GL_GetProcAddress(const char *proc);
extern void wiliwili_draw_gl_texture(struct NVGcontext *vg, unsigned int texture, int width, int height);

/* ---- 系统导入 ---- */
int32_t sceSysmoduleLoadModule(uint16_t id);
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd, size_t length, size_t alignment,
                                      int32_t memoryType, int64_t *physicalAddr);
int32_t sceKernelMapDirectMemory(void **addr, size_t length, int32_t prot, int32_t flags, int64_t physicalAddr,
                                 size_t alignment);
int32_t sceKernelMapNamedFlexibleMemory(void **addr, size_t length, int32_t prot, int32_t flags, const char *name);

/* ---- libSceVideodec2 ---- */
int32_t sceVideodec2QueryComputeMemoryInfo(void *memory);
int32_t sceVideodec2AllocateComputeQueue(const void *config, const void *memory, void **queue);
int32_t sceVideodec2ReleaseComputeQueue(void *queue);
int32_t sceVideodec2QueryDecoderMemoryInfo(const void *config, void *memory);
int32_t sceVideodec2CreateDecoder(const void *config, const void *memory, void **decoder);
int32_t sceVideodec2DeleteDecoder(void *decoder);
int32_t sceVideodec2Reset(void *decoder);
int32_t sceVideodec2Decode(void *decoder, void *input, void *frame, void *output);
int32_t sceVideodec2Flush(void *decoder, void *frame, void *output);

#define SCE_SYSMODULE_VIDEODEC2 207
#define CODEC_AVC 1u
#define CODEC_HEVC 974921u
#define RESOURCE_COMPUTE 1u
#define PIPELINE_SLOTS 3
#define MAX_AU 4096
#define AU_SLOT_SIZE 0x800000u
#define STREAM_CAP (8u * 1024u * 1024u)

typedef struct {
    uint64_t size;
    uint32_t resource_type;
    uint32_t codec_type;
    uint32_t profile;
    uint32_t max_level;
    int32_t max_width;
    int32_t max_height;
    int32_t max_dpb_frames;
    uint32_t pipeline_depth;
    uint64_t compute_queue;
    uint64_t cpu_affinity;
    int32_t cpu_priority;
    uint32_t optimize_progressive;
    uint32_t check_memory_type;
    uint32_t reserved;
} DecoderConfigInfo;

typedef struct {
    uint64_t size;
    uint64_t cpu_size;
    void *cpu;
    uint64_t gpu_size;
    void *gpu;
    uint64_t cpu_gpu_size;
    void *cpu_gpu;
    uint64_t max_frame_size;
    uint32_t frame_alignment;
    uint32_t reserved;
} DecoderMemoryInfo;

typedef struct {
    uint64_t size;
    uint16_t pipe_id;
    uint16_t queue_id;
    uint8_t check_memory_type;
    uint8_t reserved0;
    uint16_t reserved1;
} ComputeConfigInfo;

typedef struct {
    uint64_t size;
    uint64_t cpu_gpu_size;
    void *cpu_gpu;
} ComputeMemoryInfo;

typedef struct {
    uint64_t size;
    void *au;
    uint64_t au_size;
    uint64_t pts;
    uint64_t dts;
    uint64_t attached;
} InputData;

typedef struct {
    uint64_t size;
    void *buffer;
    uint64_t buffer_size;
    uint32_t accepted;
    uint32_t reserved;
} FrameBuffer;

typedef struct {
    uint64_t size;
    uint8_t valid;
    uint8_t error;
    uint8_t picture_count;
    uint8_t padding;
    uint32_t codec;
    uint32_t width;
    uint32_t pitch;
    uint32_t height;
    uint32_t reserved;
    void *buffer;
    uint64_t buffer_size;
    uint32_t frame_format;
    uint32_t pitch_bytes;
} OutputInfo;
typedef struct {
    int64_t raw_pts;
    int64_t raw_dts;
    uint64_t fed_pts;
    uint64_t fed_dts;
    int display_index;
    int expected_luma;
    int segment;
} OrderMeta;

#define MAX_ORDER_OUTPUT (MAX_AU + 16)

/* ---- 状态 ---- */
static void *g_au_pool;
static void *g_frame_pool;
static uint64_t g_frame_size;
static void *g_decoder;

static uint8_t g_stream[STREAM_CAP];
static int g_stream_size;
static int g_au_offset[MAX_AU];
static int g_au_size[MAX_AU];
static int g_au_count;
static int g_au_index;

static int g_vdec_codec = CODEC_AVC;
static int g_vdec_main10;
static int g_vdec_width = 640;
static int g_vdec_height = 368;
static int g_vdec_p0;
static int g_vdec_draw;
static uint32_t g_decode_us[MAX_AU];
static int g_decode_count;
static OrderMeta g_order_meta[MAX_AU];
static int g_order_meta_count;
static int g_order_mode;
static int g_order_current_segment;
static int g_order_segment_outputs;
static int g_order_total_outputs;
static int g_order_fifo_failures;
static int g_order_unknown_outputs;
static int g_order_duplicate_outputs;
static int g_order_output_display[MAX_ORDER_OUTPUT];
static int g_order_output_segment[MAX_ORDER_OUTPUT];
static int g_order_unknown_dts;

static uint8_t g_y_storage[1920 * 1088]; /* 稳定副本（帧池槽会被复用） */
static uint8_t g_uv_storage[1920 * 544]; /* NV12 的 UV 平面：半高度、交织 CbCr */
static const uint8_t *g_y_plane_ptr  = g_y_storage;
static const uint8_t *g_uv_plane_ptr = g_uv_storage;
static int g_y_width, g_y_height, g_y_pitch;
static unsigned long long g_y_uploads;
/* 每帧是否真的来了新数据（由播放器在取到新帧时置位）。为 0 时跳过纹理上传，
 * 只重画上一次的纹理——重传同样的数据在 AGC 上要 4+ ms/帧，纯属浪费。 */
static int g_nv12_fresh = 1;
void wiliwili_nv12_mark_fresh(int fresh) { g_nv12_fresh = fresh; }

/* ── 帧耗时分段统计（原生线调优） ─────────────────────────────────────────
 * 由 borealis 帧循环在四个位置调用，分成四段（每段都有明确的绘制内容）：
 *   ui     = beginFrame/clear + 整棵视图树的 nanovg 绘制与 GL 提交
 *   submit = nvgEndFrame（UI 收尾提交）
 *   video  = 视频上屏（硬解探针叠加 + 自管播放器的 NV12 纹理上传与绘制）
 *   swap   = endFrame（eglSwapBuffers：AGC 批次提交 + flip 与等待）
 * 每 30 帧汇总一行，用来判断"卡"到底卡在哪一段。
 * ──────────────────────────────────────────────────────────────────────── */
static long long g_t0, g_t1, g_t2, g_t3, g_t_clear, g_t_video_end;
static long long g_ui_us, g_submit_us, g_video_us, g_swap_us, g_clear_us;
static int g_phase_frames;
static long long phase_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000LL;
}

/* ── 每帧 draw 调用计数 ───────────────────────────────────────────────────
 * nanovg 的 GL 后端对每个记录调用发一次 draw（nanovg_gl.h:1124 等，绘制
 * 只在 nvgEndFrame 里真正提交），而 ps5-opengl 的每次 draw 都有一份固定的
 * 提交开销：真机实测主页 nvgEndFrame ≈ 0 ms、播放页 290 ms 且随时间增长，
 * 差别就在调用数/单次成本上。borealis 通过 glad 用函数指针调 GL
 * （`glad_glDrawArrays` 是变量，链接期 --wrap 拦不到），所以在运行时把指针
 * 换成计数版：第一次帧钩子时安装，之后每个 draw +1。 */
static void (*g_real_draw_arrays)(unsigned int, int, int);
static unsigned g_draw_calls_frame, g_draw_calls_report;
extern void (*glad_glDrawArrays)(unsigned int mode, int first, int count);
static void wiliwili_counting_draw_arrays(unsigned int mode, int first, int count) {
    ++g_draw_calls_frame;
    g_real_draw_arrays(mode, first, count);
}
static void wiliwili_draw_counter_install(void) {
    if (g_real_draw_arrays == 0 && glad_glDrawArrays != 0) {
        g_real_draw_arrays = glad_glDrawArrays;
        glad_glDrawArrays  = wiliwili_counting_draw_arrays;
    }
}

void wiliwili_frame_phase_begin(void) {
    if (g_real_draw_arrays == 0) wiliwili_draw_counter_install();
    g_t0 = phase_now_us();
}
/* beginFrame+clear 的结束点：由 SDLVideoContext::clear() 调用（见 sdl_video.cpp）。 */
void wiliwili_frame_phase_clear(void) { g_t_clear = phase_now_us(); }
void wiliwili_frame_phase_ui(void) { g_t1 = phase_now_us(); }
void wiliwili_frame_phase_video_begin(void) { g_t2 = phase_now_us(); }
/* 视频画完的时刻：视频已挪到 UI 之前，submit 段现在只含 nvgEndFrame。 */
void wiliwili_frame_phase_video_end(void) { g_t_video_end = phase_now_us(); }
void wiliwili_frame_phase_submit(void) {
    g_t3        = phase_now_us();
    g_clear_us  = g_t_clear - g_t0;
    g_ui_us     = g_t1 - g_t_clear;
    g_video_us  = g_t_video_end - g_t2;
    g_submit_us = g_t3 - g_t_video_end;
}
void wiliwili_frame_phase_swap(void) {
    long long now = phase_now_us();
    g_swap_us     = now - g_t3;
    /* 每 30 帧计数一次，但每行都要 open/write/fsync/close；AGC 已有 agc health，只有诊断时
     * 用 WILIWILI_TRACE 打开此逐帧段统计，避免正常运行被文件日志拖慢。 */
    if (++g_phase_frames >= 30) {
        if (wiliwili_trace_enabled()) {
            char lb[256];
            snprintf(lb, sizeof(lb), "frame: clear=%lldms ui=%lldms submit=%lldms video=%lldms swap=%lldms calls=%u/30",
                     g_clear_us / 1000, g_ui_us / 1000, g_submit_us / 1000, g_video_us / 1000, g_swap_us / 1000,
                     g_draw_calls_frame - g_draw_calls_report);
            wiliwili_boot_log(lb);
        }
        g_phase_frames      = 0;
        g_draw_calls_report = g_draw_calls_frame;
    }
}
static double g_upload_ms_total;
static unsigned long long g_draws;
static int g_ready;
/* 上屏修正开关：真机实测 PS5 硬解帧出来后方向与颜色对不上。默认 180 度旋转、HD 走 BT.709。 */
int wiliwili_video_flip = 2; /* 真机实测：底子是垂直翻转（180 度会再引入水平错误） */
int wiliwili_video_swap = 1; /* 真机颜色不对：先试 U/V 交换（可用 WILIWILI_VIDEO_SWAP=0 关掉比对） */
int wiliwili_video_709  = 1;

static void log2(const char *fmt, long a, long b) {
    char line[192];
    snprintf(line, sizeof(line), fmt, a, b);
    wiliwili_boot_log(line);
}

static void log3(const char *fmt, long a, long b, long c) {
    char line[192];
    snprintf(line, sizeof(line), fmt, a, b, c);
    wiliwili_boot_log(line);
}

static uint64_t align16k(uint64_t value) { return (value + 0x3FFF) & ~0x3FFFuLL; }

static void *alloc_direct(uint64_t limit, uint64_t size, int32_t prot) {
    int64_t start = 0;
    if (sceKernelAllocateDirectMemory(0, (int64_t)limit, (size_t)size, 0x4000, 12, &start) != 0) return NULL;
    void *address = NULL;
    if (sceKernelMapDirectMemory(&address, (size_t)size, prot, 0, start, 0x4000) != 0) return NULL;
    return address;
}

static int find_start_code(int from, int *prefix_len) {
    for (int i = from; i + 3 < g_stream_size; ++i) {
        if (g_stream[i] != 0 || g_stream[i + 1] != 0) continue;
        if (g_stream[i + 2] == 1) {
            *prefix_len = 3;
            return i;
        }
        if (g_stream[i + 2] == 0 && g_stream[i + 3] == 1) {
            *prefix_len = 4;
            return i;
        }
    }
    return -1;
}

static int is_picture_start(int nal_start, int nal_size) {
    if (g_vdec_codec == CODEC_AVC) {
        if (nal_size < 2) return 0;
        int type = g_stream[nal_start] & 0x1F;
        /* P0 samples use one slice per picture. */
        return type == 1 || type == 5;
    }
    if (nal_size < 3) return 0;
    int type = (g_stream[nal_start] >> 1) & 0x3F;
    /* HEVC first_slice_segment_in_pic_flag is bit 7 of the first payload byte. */
    return type <= 31 && (g_stream[nal_start + 2] & 0x80) != 0;
}

/* Split one-picture Annex-B access units. The test encoder emits one slice per
 * picture; the HEVC path additionally checks first_slice_segment_in_pic_flag. */
static void split_stream(void) {
    int au_start = 0;
    int seen_picture = 0;
    int first_prefix_len = 0;
    int pos = find_start_code(0, &first_prefix_len);
    while (pos >= 0) {
        int prefix_len = 0;
        (void)find_start_code(pos, &prefix_len);
        int next_prefix_len = 0;
        int next = find_start_code(pos + prefix_len, &next_prefix_len);
        int nal_start = pos + prefix_len;
        int nal_end = next >= 0 ? next : g_stream_size;
        if (is_picture_start(nal_start, nal_end - nal_start)) {
            if (seen_picture && g_au_count < MAX_AU) {
                g_au_offset[g_au_count] = au_start;
                g_au_size[g_au_count] = pos - au_start;
                ++g_au_count;
                au_start = pos;
            }
            seen_picture = 1;
        }
        if (next < 0) break;
        pos = next;
    }
    if (seen_picture && au_start < g_stream_size && g_au_count < MAX_AU) {
        g_au_offset[g_au_count] = au_start;
        g_au_size[g_au_count] = g_stream_size - au_start;
        ++g_au_count;
    }
}
static int is_au_prefix(int nal_start, int nal_size) {
    if (nal_size <= 0) return 0;
    if (g_vdec_codec == CODEC_AVC) {
        int type = g_stream[nal_start] & 0x1F;
        return type == 7 || type == 8 || type == 9;
    }
    int type = (g_stream[nal_start] >> 1) & 0x3F;
    return type >= 32 && type <= 35;
}

/* Order-mode splitter keeps SPS/PPS/VPS/AUD with the following picture. This
 * matters for a reset or a resolution switch: those headers are the first AU
 * after the seek, not trailing bytes of the previous picture. */
static void split_stream_order(void) {
    int au_start = 0;
    int pending_prefix = -1;
    int seen_picture = 0;
    int pos = find_start_code(0, &(int){0});
    while (pos >= 0) {
        int prefix_len = 0;
        (void)find_start_code(pos, &prefix_len);
        int next_prefix_len = 0;
        int next = find_start_code(pos + prefix_len, &next_prefix_len);
        int nal_start = pos + prefix_len;
        int nal_end = next >= 0 ? next : g_stream_size;
        int nal_size = nal_end - nal_start;
        if (is_picture_start(nal_start, nal_size)) {
            if (seen_picture && g_au_count < MAX_AU) {
                int boundary = pending_prefix >= 0 ? pending_prefix : pos;
                if (boundary > au_start) {
                    g_au_offset[g_au_count] = au_start;
                    g_au_size[g_au_count] = boundary - au_start;
                    ++g_au_count;
                }
                au_start = boundary;
            }
            pending_prefix = -1;
            seen_picture = 1;
        } else if (seen_picture && is_au_prefix(nal_start, nal_size) && pending_prefix < 0) {
            pending_prefix = pos;
        }
        if (next < 0) break;
        pos = next;
    }
    if (seen_picture && au_start < g_stream_size && g_au_count < MAX_AU) {
        g_au_offset[g_au_count] = au_start;
        g_au_size[g_au_count] = g_stream_size - au_start;
        ++g_au_count;
    }
}

static int au_is_idr(int index) {
    int start = g_au_offset[index];
    int end = start + g_au_size[index];
    int pos = find_start_code(start, &(int){0});
    while (pos >= 0 && pos < end) {
        int prefix_len = 0;
        (void)find_start_code(pos, &prefix_len);
        int next_prefix_len = 0;
        int next = find_start_code(pos + prefix_len, &next_prefix_len);
        int nal_start = pos + prefix_len;
        if (nal_start >= end) break;
        int type = g_vdec_codec == CODEC_AVC ? (g_stream[nal_start] & 0x1F) : ((g_stream[nal_start] >> 1) & 0x3F);
        if ((g_vdec_codec == CODEC_AVC && type == 5) ||
            (g_vdec_codec == CODEC_HEVC && (type == 19 || type == 20))) {
            return 1;
        }
        if (next < 0 || next >= end) break;
        pos = next;
    }
    return 0;
}

static uint32_t vdec_elapsed_us(const struct timespec *a, const struct timespec *b) {
    int64_t sec = (int64_t)b->tv_sec - (int64_t)a->tv_sec;
    int64_t nsec = (int64_t)b->tv_nsec - (int64_t)a->tv_nsec;
    return (uint32_t)(sec * 1000000LL + nsec / 1000LL);
}

static int vdec_compare_us(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static uint32_t vdec_p95_us(void) {
    if (g_decode_count <= 0) return 0;
    qsort(g_decode_us, (size_t)g_decode_count, sizeof(g_decode_us[0]), vdec_compare_us);
    int index = (g_decode_count * 95 + 99) / 100 - 1;
    if (index < 0) index = 0;
    return g_decode_us[index];
}
static int load_order_meta(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char line[256];
    int length = 0;
    char ch;
    for (;;) {
        ssize_t n = read(fd, &ch, 1);
        if (n <= 0) break;
        if (ch != '\n' && length < (int)sizeof(line) - 1) {
            line[length++] = ch;
            continue;
        }
        line[length] = '\0';
        length = 0;
        if (line[0] == '\0' || line[0] == '#') continue;
        int seq, display, luma, segment;
        long long raw_pts, raw_dts;
        unsigned long long fed_pts, fed_dts;
        if (sscanf(line, "%d %lld %lld %llu %llu %d %d %d", &seq, &raw_pts, &raw_dts, &fed_pts,
                   &fed_dts, &display, &luma, &segment) != 8) {
            continue;
        }
        if (g_order_meta_count >= MAX_AU) break;
        OrderMeta *meta = &g_order_meta[g_order_meta_count++];
        meta->raw_pts = (int64_t)raw_pts;
        meta->raw_dts = (int64_t)raw_dts;
        meta->fed_pts = (uint64_t)fed_pts;
        meta->fed_dts = (uint64_t)fed_dts;
        meta->display_index = display;
        meta->expected_luma = luma;
        meta->segment = segment;
    }
    if (length > 0 && g_order_meta_count < MAX_AU) {
        line[length] = '\0';
        if (line[0] != '\0' && line[0] != '#') {
            int seq, display, luma, segment;
            long long raw_pts, raw_dts;
            unsigned long long fed_pts, fed_dts;
            if (sscanf(line, "%d %lld %lld %llu %llu %d %d %d", &seq, &raw_pts, &raw_dts, &fed_pts,
                       &fed_dts, &display, &luma, &segment) == 8) {
                OrderMeta *meta = &g_order_meta[g_order_meta_count++];
                meta->raw_pts = (int64_t)raw_pts;
                meta->raw_dts = (int64_t)raw_dts;
                meta->fed_pts = (uint64_t)fed_pts;
                meta->fed_dts = (uint64_t)fed_dts;
                meta->display_index = display;
                meta->expected_luma = luma;
                meta->segment = segment;
            }
        }
    }
    close(fd);
    return g_order_meta_count;
}

static int order_expected_display(int segment, int ordinal) {
    int previous = -1;
    for (int n = 0; n <= ordinal; ++n) {
        int best = 0x7fffffff;
        for (int i = 0; i < g_order_meta_count; ++i) {
            const OrderMeta *meta = &g_order_meta[i];
            if (meta->segment == segment && meta->display_index > previous && meta->display_index < best) {
                best = meta->display_index;
            }
        }
        if (best == 0x7fffffff) return -1;
        previous = best;
    }
    return previous;
}

static int order_sample_luma(const OutputInfo *out) {
    if (out->buffer == NULL || out->width == 0 || out->height == 0 || out->pitch_bytes != out->pitch ||
        out->pitch < out->width || out->buffer_size < (uint64_t)out->pitch_bytes * out->height) {
        return -1;
    }
    const uint8_t *y = (const uint8_t *)out->buffer;
    int cx = (int)out->width / 2;
    int cy = (int)out->height / 2;
    int sum = 0;
    int count = 0;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            int x = cx + dx;
            int row = cy + dy;
            if (x >= 0 && x < (int)out->width && row >= 0 && row < (int)out->height) {
                sum += y[(size_t)row * out->pitch_bytes + (size_t)x];
                ++count;
            }
        }
    }
    return count > 0 ? (sum + count / 2) / count : -1;
}

static int order_find_output(const OutputInfo *out, int segment, int *delta) {
    int luma = order_sample_luma(out);
    int best = -1;
    int best_delta = 0x7fffffff;
    for (int i = 0; i < g_order_meta_count; ++i) {
        const OrderMeta *meta = &g_order_meta[i];
        if (meta->segment != segment) continue;
        int difference = luma >= 0 ? abs(luma - meta->expected_luma) : 0x7fffffff;
        if (difference < best_delta) {
            best = i;
            best_delta = difference;
        }
    }
    *delta = best_delta == 0x7fffffff ? -1 : best_delta;
    return best;
}

static void order_begin_segment(int segment) {
    g_order_current_segment = segment;
    g_order_segment_outputs = 0;
    char line[128];
    snprintf(line, sizeof(line), "vdec: order segment=%d begin expected=%d", segment,
             order_expected_display(segment, 0));
    wiliwili_boot_log(line);
}

static void order_report_output(const char *phase, int input_index, int flush_index, const OutputInfo *out) {
    if (!g_order_mode) return;
    int delta = -1;
    int matched_meta = order_find_output(out, g_order_current_segment, &delta);
    int matched_display = matched_meta >= 0 ? g_order_meta[matched_meta].display_index : -1;
    int expected_display = order_expected_display(g_order_current_segment, g_order_segment_outputs);
    int fifo = matched_display >= 0 && matched_display == expected_display && delta <= 2;
    if (!fifo) ++g_order_fifo_failures;
    if (matched_meta < 0 || delta > 2) ++g_order_unknown_outputs;
    for (int i = 0; i < g_order_total_outputs; ++i) {
        if (g_order_output_segment[i] == g_order_current_segment &&
            g_order_output_display[i] == matched_display && matched_display >= 0) {
            ++g_order_duplicate_outputs;
            break;
        }
    }
    if (g_order_total_outputs < MAX_ORDER_OUTPUT) {
        g_order_output_display[g_order_total_outputs] = matched_display;
        g_order_output_segment[g_order_total_outputs] = g_order_current_segment;
    }
    int luma = order_sample_luma(out);
    long long matched_pts = matched_meta >= 0 ? (long long)g_order_meta[matched_meta].raw_pts : -1;
    char line[256];
    snprintf(line, sizeof(line),
             "vdec: order out_seq=%d seg=%d phase=%s input_idx=%d flush=%d luma=%d matched_display=%d matched_pts=%lld expected_display=%d delta=%d fifo=%d",
             g_order_total_outputs, g_order_current_segment, phase, input_index, flush_index, luma,
             matched_display, matched_pts, expected_display, delta, fifo);
    wiliwili_boot_log(line);
    ++g_order_total_outputs;
    ++g_order_segment_outputs;
}
static void order_account_output(const DecoderConfigInfo *config, FrameBuffer *frame, OutputInfo *out, int input_index,
                                 int flush_index, int *decoded, int *decode_errors, int *accepted_count,
                                 int *format_mismatch, int *p010_count) {
    if (!out->valid) return;
    ++*decoded;
    if (frame->accepted) ++*accepted_count;
    int p010 = out->pitch_bytes != 0 && out->pitch_bytes == out->pitch * 2u;
    if (p010) ++*p010_count;
    if (p010 != g_vdec_main10) ++*format_mismatch;
    uintptr_t base = (uintptr_t)g_frame_pool;
    uintptr_t end = base + g_frame_size * PIPELINE_SLOTS;
    uintptr_t address = (uintptr_t)out->buffer;
    int in_pool = address >= base && address < end;
    uint64_t row_bytes = out->pitch_bytes != 0 ? out->pitch_bytes : (uint64_t)out->pitch * (p010 ? 2u : 1u);
    uint64_t expected_pitch_bytes = (uint64_t)out->pitch * (p010 ? 2u : 1u);
    uint64_t required = row_bytes * ((uint64_t)out->height + ((uint64_t)out->height + 1u) / 2u);
    int invalid = !frame->accepted || out->error || out->picture_count != 1 || out->codec != config->codec_type ||
                  out->width == 0 || out->height == 0 || out->width > (uint32_t)config->max_width ||
                  out->height > (uint32_t)config->max_height || out->pitch < out->width ||
                  out->pitch_bytes != expected_pitch_bytes || out->buffer == NULL || out->buffer_size < required || !in_pool;
    if (invalid) ++*decode_errors;
    char line[256];
    snprintf(line, sizeof(line),
             "vdec: order frame input_idx=%d flush=%d valid=%u err=%u pics=%u codec=%u %ux%u pitch=%u pitch_bytes=%u fmt=%u buf=%llu accepted=%u in_pool=%d",
             input_index, flush_index, out->valid, out->error, out->picture_count, out->codec, out->width,
             out->height, out->pitch, out->pitch_bytes, out->frame_format, (unsigned long long)out->buffer_size,
             frame->accepted, in_pool);
    wiliwili_boot_log(line);
    order_report_output(flush_index >= 0 ? "flush" : "decode", input_index, flush_index, out);
}

static void order_drain(const DecoderConfigInfo *config, const char *reason, int *slot, int *decoded,
                        int *decode_errors, int *accepted_count, int *format_mismatch, int *p010_count,
                        uint64_t *total_us) {
    for (int flush_index = 0; flush_index < MAX_AU + 8; ++flush_index) {
        FrameBuffer frame;
        memset(&frame, 0, sizeof(frame));
        frame.size = sizeof(frame);
        frame.buffer = (uint8_t *)g_frame_pool + (size_t)*slot * g_frame_size;
        frame.buffer_size = g_frame_size;
        OutputInfo out;
        memset(&out, 0, sizeof(out));
        out.size = sizeof(out);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int32_t rc = sceVideodec2Flush(g_decoder, &frame, &out);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint32_t elapsed_us = vdec_elapsed_us(&t0, &t1);
        if (g_decode_count < MAX_AU) g_decode_us[g_decode_count++] = elapsed_us;
        *total_us += elapsed_us;
        if (rc != 0) {
            ++*decode_errors;
            char line[192];
            snprintf(line, sizeof(line), "vdec: order flush reason=%s idx=%d rc=%d accepted=%u", reason, flush_index,
                     rc, frame.accepted);
            wiliwili_boot_log(line);
            break;
        }
        if (!out.valid) {
            char line[160];
            snprintf(line, sizeof(line), "vdec: order flush reason=%s idx=%d empty=1", reason, flush_index);
            wiliwili_boot_log(line);
            break;
        }
        order_account_output(config, &frame, &out, -1, flush_index, decoded, decode_errors, accepted_count,
                             format_mismatch, p010_count);
        *slot = (*slot + 1) % PIPELINE_SLOTS;
    }
}

static void run_order_experiment(const DecoderConfigInfo *config, int *decoded, int *buffered, int *decode_errors,
                                 int *accepted_count, int *format_mismatch, int *p010_count, uint64_t *total_us) {
    int slot = 0;
    int current_segment = g_order_meta[0].segment;
    for (g_au_index = 0; g_au_index < g_au_count; ++g_au_index) {
        const OrderMeta *meta = &g_order_meta[g_au_index];
        if (meta->segment != current_segment) {
            order_drain(config, "segment", &slot, decoded, decode_errors, accepted_count, format_mismatch, p010_count,
                        total_us);
            int32_t reset_rc = sceVideodec2Reset(g_decoder);
            char reset_line[160];
            snprintf(reset_line, sizeof(reset_line), "vdec: order segment=%d reset_rc=%d", meta->segment, reset_rc);
            wiliwili_boot_log(reset_line);
            if (reset_rc != 0) ++*decode_errors;
            current_segment = meta->segment;
            order_begin_segment(current_segment);
        }
        if ((uint64_t)g_au_size[g_au_index] > AU_SLOT_SIZE) {
            wiliwili_boot_log("vdec: order AU exceeds 8MiB slot");
            ++*decode_errors;
            break;
        }
        uint8_t *au_slot = (uint8_t *)g_au_pool + (size_t)slot * AU_SLOT_SIZE;
        memcpy(au_slot, g_stream + g_au_offset[g_au_index], (size_t)g_au_size[g_au_index]);
        InputData input;
        memset(&input, 0, sizeof(input));
        input.size = sizeof(input);
        input.au = au_slot;
        input.au_size = (uint64_t)g_au_size[g_au_index];
        input.pts = meta->fed_pts;
        input.dts = g_order_unknown_dts ? UINT64_MAX : meta->fed_dts;
        char input_line[256];
        snprintf(input_line, sizeof(input_line),
                 "vdec: order in idx=%d seg=%d raw_pts=%lld raw_dts=%lld fed_pts=%llu fed_dts=%llu dts_unknown=%d idr=%d display=%d luma=%d",
                 g_au_index, meta->segment, (long long)meta->raw_pts, (long long)meta->raw_dts,
                 (unsigned long long)meta->fed_pts, (unsigned long long)meta->fed_dts, g_order_unknown_dts,
                 au_is_idr(g_au_index), meta->display_index, meta->expected_luma);
        wiliwili_boot_log(input_line);

        FrameBuffer frame;
        memset(&frame, 0, sizeof(frame));
        frame.size = sizeof(frame);
        frame.buffer = (uint8_t *)g_frame_pool + (size_t)slot * g_frame_size;
        frame.buffer_size = g_frame_size;
        OutputInfo out;
        memset(&out, 0, sizeof(out));
        out.size = sizeof(out);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int32_t rc = sceVideodec2Decode(g_decoder, &input, &frame, &out);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint32_t elapsed_us = vdec_elapsed_us(&t0, &t1);
        if (g_decode_count < MAX_AU) g_decode_us[g_decode_count++] = elapsed_us;
        *total_us += elapsed_us;
        if (rc != 0) {
            ++*decode_errors;
            char error_line[224];
            snprintf(error_line, sizeof(error_line), "vdec: order error idx=%d seg=%d rc=%d valid=%u accepted=%u",
                     g_au_index, meta->segment, rc, out.valid, frame.accepted);
            wiliwili_boot_log(error_line);
        } else if (out.valid) {
            order_account_output(config, &frame, &out, g_au_index, -1, decoded, decode_errors, accepted_count,
                                 format_mismatch, p010_count);
        } else {
            ++*buffered;
            char buffered_line[192];
            snprintf(buffered_line, sizeof(buffered_line), "vdec: order buffered idx=%d seg=%d", g_au_index,
                     meta->segment);
            wiliwili_boot_log(buffered_line);
        }
        slot = (slot + 1) % PIPELINE_SLOTS;
    }
    order_drain(config, "eof", &slot, decoded, decode_errors, accepted_count, format_mismatch, p010_count, total_us);
}

void wiliwili_videodec2_probe(void) {
    const char *codec = getenv("WILIWILI_VDEC_CODEC");
    const char *path = getenv("WILIWILI_VDEC_PATH");
    const char *meta_path = getenv("WILIWILI_VDEC_META");
    const char *width_env = getenv("WILIWILI_VDEC_WIDTH");
    const char *height_env = getenv("WILIWILI_VDEC_HEIGHT");
    const int is_hevc = codec != NULL && strcmp(codec, "hevc") == 0;
    g_vdec_p0 = getenv("WILIWILI_VDEC_P0") != NULL;
    g_order_mode = g_vdec_p0 && getenv("WILIWILI_VDEC_ORDER") != NULL && meta_path != NULL && meta_path[0] != '\0';
    g_order_unknown_dts = getenv("WILIWILI_VDEC_DTS_UNKNOWN") != NULL;
    g_order_meta_count = 0;
    g_order_total_outputs = 0;
    g_order_fifo_failures = 0;
    g_order_unknown_outputs = 0;
    g_order_duplicate_outputs = 0;
    g_vdec_draw = !g_vdec_p0 || getenv("WILIWILI_VDEC_DRAW") != NULL;
    g_vdec_codec = is_hevc ? CODEC_HEVC : CODEC_AVC;
    g_vdec_main10 = is_hevc && getenv("WILIWILI_VDEC_MAIN10") != NULL;
    g_vdec_width = width_env != NULL ? atoi(width_env) : (g_vdec_p0 ? 3840 : 640);
    g_vdec_height = height_env != NULL ? atoi(height_env) : (g_vdec_p0 ? 2160 : 368);
    if (g_vdec_width <= 0 || g_vdec_height <= 0) {
        wiliwili_boot_log("vdec: invalid requested dimensions");
        return;
    }

    char line[256];
    snprintf(line, sizeof(line), "vdec: enter p0=%d order=%d dts_unknown=%d codec=%s main10=%d visible=%dx%d", g_vdec_p0,
             g_order_mode, g_order_unknown_dts, is_hevc ? "hevc" : "avc", g_vdec_main10, g_vdec_width, g_vdec_height);
    wiliwili_boot_log(line);

    int32_t rc = sceSysmoduleLoadModule(SCE_SYSMODULE_VIDEODEC2);
    log2("vdec: sysmodule207 rc=%d", rc, 0);
    if (rc != 0) return;

    uint64_t limit = (uint64_t)sceKernelGetDirectMemorySize();
    log2("vdec: direct_limit=0x%lx", (long)limit, 0);

    ComputeMemoryInfo cm;
    memset(&cm, 0, sizeof(cm));
    cm.size = sizeof(cm);
    rc = sceVideodec2QueryComputeMemoryInfo(&cm);
    log2("vdec: query_compute rc=%d size=0x%lx", rc, (long)cm.cpu_gpu_size);
    if (rc != 0) return;
    uint64_t cm_size = align16k(cm.cpu_gpu_size);
    cm.cpu_gpu = alloc_direct(limit, cm_size, 0x33);
    cm.cpu_gpu_size = cm_size;
    if (cm.cpu_gpu == NULL) {
        wiliwili_boot_log("vdec: compute memory allocation failed");
        return;
    }

    ComputeConfigInfo cc;
    memset(&cc, 0, sizeof(cc));
    cc.size = sizeof(cc);
    void *compute_queue = NULL;
    rc = sceVideodec2AllocateComputeQueue(&cc, &cm, &compute_queue);
    log2("vdec: compute_queue rc=%d", rc, 0);
    if (rc != 0) return;

    int config_height = (g_vdec_height + 15) & ~15;
    if (g_vdec_height >= 2160) config_height = 2176;
    DecoderConfigInfo config;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.resource_type = RESOURCE_COMPUTE;
    config.codec_type = g_vdec_codec;
    config.profile = is_hevc ? (g_vdec_main10 ? 2u : 1u) : 100u;
    config.max_level = is_hevc ? (g_vdec_width >= 3840 ? 153 : 123) : (g_vdec_width >= 3840 ? 52 : 51);
    config.max_width = g_vdec_width;
    config.max_height = config_height;
    config.max_dpb_frames = -1; /* B站样本的 ref count 可能超过 4，交给 VDEC AUTO。 */
    config.pipeline_depth = 1;
    config.compute_queue = (uint64_t)compute_queue;
    config.cpu_affinity = 0x3F;
    config.cpu_priority = 700;
    config.optimize_progressive = 1;
    snprintf(line, sizeof(line), "vdec: config codec=%u profile=%u level=%d max=%dx%d dpb=%d depth=%u", config.codec_type,
             config.profile, config.max_level, config.max_width, config.max_height, config.max_dpb_frames,
             config.pipeline_depth);
    wiliwili_boot_log(line);

    DecoderMemoryInfo mem;
    memset(&mem, 0, sizeof(mem));
    mem.size = sizeof(mem);
    rc = sceVideodec2QueryDecoderMemoryInfo(&config, &mem);
    log2("vdec: query_decoder rc=%d", rc, 0);
    if (rc != 0) return;
    snprintf(line, sizeof(line), "vdec: mem raw cpu=0x%lx gpu=0x%lx cpu_gpu=0x%lx frame=0x%lx align=%u",
             (long)mem.cpu_size, (long)mem.gpu_size, (long)mem.cpu_gpu_size, (long)mem.max_frame_size,
             mem.frame_alignment);
    wiliwili_boot_log(line);

    uint64_t cpu_size = align16k(mem.cpu_size);
    void *cpu_ws = NULL;
    int32_t cpu_rc = cpu_size ? sceKernelMapNamedFlexibleMemory(&cpu_ws, (size_t)cpu_size, 0x03, 0, "VdecCpu") : 0;
    mem.cpu = cpu_ws;
    mem.cpu_size = cpu_size;
    uint64_t gpu_size = align16k(mem.gpu_size);
    mem.gpu = gpu_size ? alloc_direct(limit, gpu_size, 0x32) : NULL;
    mem.gpu_size = gpu_size;
    uint64_t cpu_gpu_size = align16k(mem.cpu_gpu_size);
    mem.cpu_gpu = cpu_gpu_size ? alloc_direct(limit, cpu_gpu_size, 0x33) : NULL;
    mem.cpu_gpu_size = cpu_gpu_size;
    snprintf(line, sizeof(line), "vdec: mem aligned cpu=0x%lx gpu=0x%lx cpu_gpu=0x%lx map_rc=%d alloc=%d/%d/%d",
             (long)cpu_size, (long)gpu_size, (long)cpu_gpu_size, cpu_rc, cpu_ws != NULL,
             gpu_size == 0 || mem.gpu != NULL, cpu_gpu_size == 0 || mem.cpu_gpu != NULL);
    wiliwili_boot_log(line);
    if (cpu_rc != 0 || (gpu_size != 0 && mem.gpu == NULL) || (cpu_gpu_size != 0 && mem.cpu_gpu == NULL)) return;

    g_frame_size = align16k(mem.max_frame_size);
    g_au_pool = alloc_direct(limit, (uint64_t)AU_SLOT_SIZE * PIPELINE_SLOTS, 0x32);
    g_frame_pool = alloc_direct(limit, g_frame_size * PIPELINE_SLOTS, 0x32);
    snprintf(line, sizeof(line), "vdec: pools au=0x%lx frame=0x%lx frame_size=0x%lx ok=%d/%d",
             (long)AU_SLOT_SIZE * PIPELINE_SLOTS, (long)g_frame_size * PIPELINE_SLOTS, (long)g_frame_size,
             g_au_pool != NULL, g_frame_pool != NULL);
    wiliwili_boot_log(line);
    if (g_au_pool == NULL || g_frame_pool == NULL) return;

    rc = sceVideodec2CreateDecoder(&config, &mem, &g_decoder);
    log2("vdec: create_decoder rc=%d", rc, 0);
    if (rc != 0) return;
    rc = sceVideodec2Reset(g_decoder);
    log2("vdec: reset rc=%d", rc, 0);
    if (rc != 0) return;

    const char *default_path = is_hevc ? "/app0/assets/vdec-stream.hevc" : "/app0/assets/vdec-stream.h264";
    const char *stream_path = path != NULL && path[0] != '\0' ? path : default_path;
    int fd = open(stream_path, O_RDONLY);
    if (fd < 0) {
        snprintf(line, sizeof(line), "vdec: stream file missing path=%s", stream_path);
        wiliwili_boot_log(line);
        return;
    }
    g_stream_size = 0;
    while (g_stream_size < (int)sizeof(g_stream)) {
        ssize_t n = read(fd, g_stream + g_stream_size, sizeof(g_stream) - (size_t)g_stream_size);
        if (n <= 0) break;
        g_stream_size += (int)n;
    }
    close(fd);
    snprintf(line, sizeof(line), "vdec: stream path=%s bytes=%d truncated=%d", stream_path, g_stream_size,
             g_stream_size == (int)sizeof(g_stream));
    wiliwili_boot_log(line);
    if (g_stream_size <= 0) return;

    g_au_count = 0;
    if (g_order_mode) split_stream_order();
    else split_stream();
    snprintf(line, sizeof(line), "vdec: aus=%d split_codec=%s", g_au_count, is_hevc ? "hevc" : "avc");
    wiliwili_boot_log(line);
    if (g_au_count <= 0) {
        wiliwili_boot_log("vdec: no access units");
        return;
    }
    if (g_order_mode) {
        int meta_count = load_order_meta(meta_path);
        snprintf(line, sizeof(line), "vdec: order meta path=%s rc=%d count=%d", meta_path, meta_count,
                 g_order_meta_count);
        wiliwili_boot_log(line);
        if (meta_count < 0 || g_order_meta_count != g_au_count) {
            wiliwili_boot_log("vdec: order meta/AU count mismatch");
            return;
        }
        order_begin_segment(g_order_meta[0].segment);
    }

    int decoded = 0;
    int buffered = 0;
    int decode_errors = 0;
    int accepted_count = 0;
    int format_mismatch = 0;
    int p010_count = 0;
    uint64_t total_us = 0;
    g_decode_count = 0;
    int slot = 0;
    if (g_order_mode) {
        run_order_experiment(&config, &decoded, &buffered, &decode_errors, &accepted_count, &format_mismatch,
                             &p010_count, &total_us);
    } else {
        for (g_au_index = 0; g_au_index < g_au_count; ++g_au_index) {
        if ((uint64_t)g_au_size[g_au_index] > AU_SLOT_SIZE) {
            wiliwili_boot_log("vdec: AU exceeds 8MiB slot");
            ++decode_errors;
            break;
        }
        uint8_t *au_slot = (uint8_t *)g_au_pool + (size_t)slot * AU_SLOT_SIZE;
        memcpy(au_slot, g_stream + g_au_offset[g_au_index], (size_t)g_au_size[g_au_index]);

        InputData input;
        memset(&input, 0, sizeof(input));
        input.size = sizeof(input);
        input.au = au_slot;
        input.au_size = (uint64_t)g_au_size[g_au_index];
        input.pts = (uint64_t)g_au_index * 3000u; /* synthetic 90 kHz, 30 fps */
        input.dts = UINT64_MAX;

        FrameBuffer frame;
        memset(&frame, 0, sizeof(frame));
        frame.size = sizeof(frame);
        frame.buffer = (uint8_t *)g_frame_pool + (size_t)slot * g_frame_size;
        frame.buffer_size = g_frame_size;

        OutputInfo out;
        memset(&out, 0, sizeof(out));
        out.size = sizeof(out);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        rc = sceVideodec2Decode(g_decoder, &input, &frame, &out);
        int did_flush = 0;
        if (rc == 0 && out.valid == 0) {
            memset(&out, 0, sizeof(out));
            out.size = sizeof(out);
            rc = sceVideodec2Flush(g_decoder, &frame, &out);
            did_flush = 1;
            ++buffered;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint32_t elapsed_us = vdec_elapsed_us(&t0, &t1);
        if (g_decode_count < MAX_AU) g_decode_us[g_decode_count++] = elapsed_us;
        total_us += elapsed_us;

        if (rc != 0) {
            ++decode_errors;
            snprintf(line, sizeof(line), "vdec: error idx=%d pts=%llu flush=%d rc=%d accepted=%u", g_au_index,
                     (unsigned long long)input.pts, did_flush, rc, frame.accepted);
            wiliwili_boot_log(line);
        } else if (out.valid) {
            ++decoded;
            if (frame.accepted) ++accepted_count;
            int p010 = out.pitch_bytes != 0 && out.pitch_bytes == out.pitch * 2u;
            if (p010) ++p010_count;
            if (p010 != g_vdec_main10) ++format_mismatch;
            uintptr_t base = (uintptr_t)g_frame_pool;
            uintptr_t end = base + g_frame_size * PIPELINE_SLOTS;
            uintptr_t address = (uintptr_t)out.buffer;
            int in_pool = address >= base && address < end;
            uint64_t row_bytes = out.pitch_bytes != 0 ? out.pitch_bytes : (uint64_t)out.pitch * (p010 ? 2u : 1u);
            uint64_t expected_pitch_bytes = (uint64_t)out.pitch * (p010 ? 2u : 1u);
            uint64_t required = row_bytes * ((uint64_t)out.height + ((uint64_t)out.height + 1u) / 2u);
            if (!frame.accepted || out.error || out.picture_count != 1 || out.codec != config.codec_type ||
                out.width < (uint32_t)g_vdec_width || out.height < (uint32_t)g_vdec_height || out.pitch < out.width ||
                out.pitch_bytes != expected_pitch_bytes || out.buffer == NULL || out.buffer_size < required || !in_pool) {
                ++decode_errors;
            }
            snprintf(line, sizeof(line),
                     "vdec: out idx=%d in_pts=%llu valid=%u err=%u pics=%u codec=%u %ux%u pitch=%u pitch_bytes=%u fmt=%u buf=%llu accepted=%u p010=%d in_pool=%d required=%llu us=%u",
                     g_au_index, (unsigned long long)input.pts, out.valid, out.error, out.picture_count, out.codec,
                     out.width, out.height, out.pitch, out.pitch_bytes, out.frame_format,
                     (unsigned long long)out.buffer_size, frame.accepted, p010, in_pool,
                     (unsigned long long)required, elapsed_us);
            wiliwili_boot_log(line);

            if (g_vdec_draw && !p010 && out.width <= 1920 && out.height <= 1088 && out.pitch <= 1920) {
                int copy_w = (int)out.width;
                int copy_h = (int)out.height;
                g_y_width = copy_w;
                g_y_height = copy_h;
                g_y_pitch = (int)out.pitch;
                const uint8_t *src = (const uint8_t *)out.buffer;
                for (int y = 0; y < copy_h; ++y)
                    memcpy(g_y_storage + (size_t)y * copy_w, src + (size_t)y * out.pitch, (size_t)copy_w);
                const uint8_t *uv_src = src + (size_t)out.pitch * copy_h;
                for (int y = 0; y < copy_h / 2; ++y)
                    memcpy(g_uv_storage + (size_t)y * copy_w, uv_src + (size_t)y * out.pitch, (size_t)copy_w);
            }
        } else {
            snprintf(line, sizeof(line), "vdec: buffered idx=%d in_pts=%llu flush=%d us=%u", g_au_index,
                     (unsigned long long)input.pts, did_flush, elapsed_us);
            wiliwili_boot_log(line);
        }
        slot = (slot + 1) % PIPELINE_SLOTS;
    }
    }

    uint32_t p95_us = vdec_p95_us();
    uint32_t avg_us = g_decode_count > 0 ? (uint32_t)(total_us / (uint64_t)g_decode_count) : 0;
    if (g_order_mode) {
        snprintf(line, sizeof(line),
                 "vdec: stats order=1 decoded=%d inputs=%d buffered=%d accepted=%d errors=%d p010=%d/%d format_mismatch=%d avg_us=%u p95_us=%u",
                 decoded, g_au_count, buffered, accepted_count, decode_errors, p010_count, decoded, format_mismatch,
                 avg_us, p95_us);
        wiliwili_boot_log(line);
        snprintf(line, sizeof(line),
                 "vdec: order stats outputs=%d fifo_failures=%d unknown=%d duplicates=%d segments=%d",
                 g_order_total_outputs, g_order_fifo_failures, g_order_unknown_outputs, g_order_duplicate_outputs,
                 g_order_current_segment + 1);
        wiliwili_boot_log(line);
        wiliwili_boot_log("vdec: pts input=real_packet_pts_dts output_pts=not_in_abi");
    } else {
        snprintf(line, sizeof(line), "vdec: stats decoded=%d/%d buffered=%d accepted=%d errors=%d p010=%d/%d format_mismatch=%d avg_us=%u p95_us=%u",
                 decoded, g_au_count, buffered, accepted_count, decode_errors, p010_count, decoded, format_mismatch,
                 avg_us, p95_us);
        wiliwili_boot_log(line);
        wiliwili_boot_log("vdec: pts input=synthetic90k_step3000 output_pts=not_in_abi reorder=not_observable");
    }
    int complete = g_order_mode ? g_order_total_outputs > 0 : decoded == g_au_count && decoded > 0;
    int mapping_pass = !g_order_mode ||
                       (g_order_fifo_failures == 0 && g_order_unknown_outputs == 0 && g_order_duplicate_outputs == 0);
    int pass = complete && mapping_pass && decode_errors == 0 && format_mismatch == 0 &&
               (p010_count > 0) == g_vdec_main10;
    snprintf(line, sizeof(line), "vdec: result pass=%d complete=%d mapping_pass=%d expected_p010=%d decoded=%d",
             pass, complete, mapping_pass, g_vdec_main10, decoded);
    wiliwili_boot_log(line);
    log3("vdec: last %dx%d pitch=%d", g_y_width, g_y_height, g_y_pitch);
    if (decoded > 0) g_ready = 1;
    wiliwili_boot_log("vdec: stream done");
}

/* 由 borealis 帧循环在 nvgEndFrame 之后调用：raw GL 画全屏四边形，
 * 两个平面（Y=R8、UV=RG8）在片元着色器里做 BT.601 limited YUV→RGB。 */
void wiliwili_videodec2_draw(struct NVGcontext *vg); /* 定义在下面 */

/* 复用的 NV12 上屏（供播放器探针调用）：两平面纹理 + YUV 着色器。 */
void wiliwili_draw_nv12(struct NVGcontext *vg, const uint8_t *y_plane, const uint8_t *uv_plane, int width, int height) {
    if (!y_plane || width <= 0) return;
    g_y_plane_ptr  = y_plane;
    g_uv_plane_ptr = uv_plane;
    g_y_width      = width;
    g_y_height     = height;
    wiliwili_videodec2_draw(vg);
}

void wiliwili_videodec2_draw(struct NVGcontext *vg) {
    (void)vg;
    /* 探针只有在 WILIWILI_TEST_VDEC 下才会产生帧；无帧时不触碰 GL 状态。 */
    if (g_y_width <= 0) return;

    typedef unsigned int GLenum_t;
    typedef void (*PFN_TexImage2D)(GLenum_t, int, int, int, int, int, GLenum_t, GLenum_t, const void *);
    typedef void (*PFN_TexSubImage2D)(GLenum_t, int, int, int, int, GLenum_t, GLenum_t, const void *);
    typedef void (*PFN_GenTextures)(int, unsigned int *);
    typedef void (*PFN_BindTexture)(GLenum_t, unsigned int);
    typedef void (*PFN_TexParameteri)(GLenum_t, GLenum_t, int);
    typedef void (*PFN_PixelStorei)(GLenum_t, int);
    typedef void (*PFN_ActiveTexture)(GLenum_t);
    typedef unsigned int (*PFN_CreateShader)(GLenum_t);
    typedef void (*PFN_ShaderSource)(unsigned int, int, const char *const *, const int *);
    typedef void (*PFN_CompileShader)(unsigned int);
    typedef void (*PFN_GetShaderiv)(unsigned int, GLenum_t, int *);
    typedef void (*PFN_GetShaderInfoLog)(unsigned int, int, int *, char *);
    typedef unsigned int (*PFN_CreateProgram)(void);
    typedef void (*PFN_AttachShader)(unsigned int, unsigned int);
    typedef void (*PFN_LinkProgram)(unsigned int);
    typedef void (*PFN_GetProgramiv)(unsigned int, GLenum_t, int *);
    typedef void (*PFN_UseProgram)(unsigned int);
    typedef int (*PFN_GetUniformLocation)(unsigned int, const char *);
    typedef void (*PFN_Uniform1i)(int, int);
    typedef void (*PFN_GenVertexArrays)(int, unsigned int *);
    typedef void (*PFN_BindVertexArray)(unsigned int);
    typedef void (*PFN_DrawArrays)(GLenum_t, int, int);
    typedef GLenum_t (*PFN_GetError)(void);

    static PFN_TexImage2D p_tex_image;
    static PFN_TexSubImage2D p_tex_sub;
    static PFN_GenTextures p_gen_textures;
    static PFN_BindTexture p_bind_texture;
    static PFN_TexParameteri p_tex_param;
    static PFN_PixelStorei p_pixel_store;
    static PFN_ActiveTexture p_active_texture;
    static PFN_CreateShader p_create_shader;
    static PFN_ShaderSource p_shader_source;
    static PFN_CompileShader p_compile_shader;
    static PFN_GetShaderiv p_get_shader_iv;
    static PFN_GetShaderInfoLog p_shader_log;
    static PFN_CreateProgram p_create_program;
    static PFN_AttachShader p_attach_shader;
    static PFN_LinkProgram p_link_program;
    static PFN_GetProgramiv p_get_program_iv;
    static PFN_UseProgram p_use_program;
    static PFN_GetUniformLocation p_uniform_location;
    static PFN_Uniform1i p_uniform1i;
    static PFN_GenVertexArrays p_gen_vaos;
    static PFN_BindVertexArray p_bind_vao;
    static PFN_DrawArrays p_draw_arrays;
    static PFN_GetError p_get_error;

    if (p_tex_image == NULL) {
        void *(*get)(const char *) = SDL_GL_GetProcAddress;
        p_tex_image                = (PFN_TexImage2D)get("glTexImage2D");
        p_tex_sub                  = (PFN_TexSubImage2D)get("glTexSubImage2D");
        p_gen_textures             = (PFN_GenTextures)get("glGenTextures");
        p_bind_texture             = (PFN_BindTexture)get("glBindTexture");
        p_tex_param                = (PFN_TexParameteri)get("glTexParameteri");
        p_pixel_store              = (PFN_PixelStorei)get("glPixelStorei");
        p_active_texture           = (PFN_ActiveTexture)get("glActiveTexture");
        p_create_shader            = (PFN_CreateShader)get("glCreateShader");
        p_shader_source            = (PFN_ShaderSource)get("glShaderSource");
        p_compile_shader           = (PFN_CompileShader)get("glCompileShader");
        p_get_shader_iv            = (PFN_GetShaderiv)get("glGetShaderiv");
        p_shader_log               = (PFN_GetShaderInfoLog)get("glGetShaderInfoLog");
        p_create_program           = (PFN_CreateProgram)get("glCreateProgram");
        p_attach_shader            = (PFN_AttachShader)get("glAttachShader");
        p_link_program             = (PFN_LinkProgram)get("glLinkProgram");
        p_get_program_iv           = (PFN_GetProgramiv)get("glGetProgramiv");
        p_use_program              = (PFN_UseProgram)get("glUseProgram");
        p_uniform_location         = (PFN_GetUniformLocation)get("glGetUniformLocation");
        p_uniform1i                = (PFN_Uniform1i)get("glUniform1i");
        p_gen_vaos                 = (PFN_GenVertexArrays)get("glGenVertexArrays");
        p_bind_vao                 = (PFN_BindVertexArray)get("glBindVertexArray");
        p_draw_arrays              = (PFN_DrawArrays)get("glDrawArrays");
        p_get_error                = (PFN_GetError)get("glGetError");
    }
    if (p_tex_sub == NULL || p_draw_arrays == NULL) return;

    static unsigned int tex_y, tex_uv, program, vao;
    static int u_y_loc, u_uv_loc, u_flip_loc, u_swap_loc, u_709_loc;
    static int pipeline_ready;

    static int last_w = -1;
    if (pipeline_ready && last_w != g_y_width) pipeline_ready = 0; /* 分辨率变化 ⇒ 重建纹理 */
    if (!pipeline_ready) {
        last_w = g_y_width;
        p_pixel_store(0x0CF5 /*UNPACK_ALIGNMENT*/, 1);

        p_gen_textures(1, &tex_y);
        p_bind_texture(0x0DE1, tex_y);
        p_tex_param(0x0DE1, 0x2801, 0x2600);
        p_tex_param(0x0DE1, 0x2800, 0x2600);
        p_tex_param(0x0DE1, 0x2802, 0x812F);
        p_tex_param(0x0DE1, 0x2803, 0x812F);
        p_tex_image(0x0DE1, 0, 0x8229 /*R8*/, g_y_width, g_y_height, 0, 0x1903 /*RED*/, 0x1401, g_y_plane_ptr);

        p_gen_textures(1, &tex_uv);
        p_bind_texture(0x0DE1, tex_uv);
        p_tex_param(0x0DE1, 0x2801, 0x2600);
        p_tex_param(0x0DE1, 0x2800, 0x2600);
        p_tex_param(0x0DE1, 0x2802, 0x812F);
        p_tex_param(0x0DE1, 0x2803, 0x812F);
        p_tex_image(0x0DE1, 0, 0x822B /*RG8*/, g_y_width / 2, g_y_height / 2, 0, 0x8227 /*RG*/, 0x1401, g_uv_plane_ptr);

        static const char *vs_src =
            "#version 330 core\n"
            "out vec2 v_uv;\n"
            "void main() {\n"
            "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
            "  v_uv = p;\n"
            "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        static const char *fs_src =
            "#version 330 core\n"
            "in vec2 v_uv;\n"
            "uniform int u_flip;\n" /* 0=不翻 1=水平 2=垂直 3=180 */
            "uniform int u_swap;\n"
            "uniform int u_709;\n" /* 1=交换 U/V */
            "uniform sampler2D u_y;\n"
            "uniform sampler2D u_uv;\n"
            "out vec4 o_color;\n"
            "void main() {\n"
            "  vec2 t = v_uv;\n"
            "  if (u_flip == 1 || u_flip == 3) t.x = 1.0 - t.x;\n"
            "  if (u_flip == 2 || u_flip == 3) t.y = 1.0 - t.y;\n"
            "  float y = texture(u_y, t).r;\n"
            "  vec2 uv = texture(u_uv, t).rg;\n"
            "  if (u_swap == 1) uv = uv.yx;\n"
            "  float Y = (y - 0.0625) * 1.164;\n"
            "  float U = uv.x - 0.5;\n"
            "  float V = uv.y - 0.5;\n"
            "  vec3 rgb;\n"
            "  if (u_709 == 1) rgb = vec3(Y + 1.793 * V, Y - 0.213 * U - 0.533 * V, Y + 2.112 * U);\n"
            "  else rgb = vec3(Y + 1.596 * V, Y - 0.391 * U - 0.813 * V, Y + 2.018 * U);\n"
            "  o_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
            "}\n";

        unsigned int vs = p_create_shader(0x8B31 /*VERTEX_SHADER*/);
        p_shader_source(vs, 1, &vs_src, NULL);
        p_compile_shader(vs);
        int ok = 0;
        p_get_shader_iv(vs, 0x8B81 /*COMPILE_STATUS*/, &ok);
        if (!ok) {
            char log[256];
            p_shader_log(vs, sizeof(log), NULL, log);
            wiliwili_boot_log(log);
            wiliwili_boot_log("vdec: vs compile failed");
            return;
        }
        unsigned int fs = p_create_shader(0x8B30 /*FRAGMENT_SHADER*/);
        p_shader_source(fs, 1, &fs_src, NULL);
        p_compile_shader(fs);
        p_get_shader_iv(fs, 0x8B81, &ok);
        if (!ok) {
            char log[256];
            p_shader_log(fs, sizeof(log), NULL, log);
            wiliwili_boot_log(log);
            wiliwili_boot_log("vdec: fs compile failed");
            return;
        }
        program = p_create_program();
        p_attach_shader(program, vs);
        p_attach_shader(program, fs);
        p_link_program(program);
        p_get_program_iv(program, 0x8B82 /*LINK_STATUS*/, &ok);
        if (!ok) {
            wiliwili_boot_log("vdec: program link failed");
            return;
        }
        u_y_loc    = p_uniform_location(program, "u_y");
        u_uv_loc   = p_uniform_location(program, "u_uv");
        u_flip_loc = p_uniform_location(program, "u_flip");
        u_swap_loc = p_uniform_location(program, "u_swap");
        u_709_loc  = p_uniform_location(program, "u_709");
        p_gen_vaos(1, &vao);
        pipeline_ready = 1;
        wiliwili_boot_log("vdec: yuv pipeline ready");
    }

    /* 每帧更新两个平面。只有**真的来了新数据**才重传：在 AGC 上两次 glTexImage2D
     * 要 4+ ms，而渲染线程每帧都调用这里，重传未变的数据纯属浪费。
     * 另外**必须**用 glTexImage2D，不能用 glTexSubImage2D：实测该 GL 实现的
     * glTexSubImage2D 对任何 (format,type) 组合都返回 GL_INVALID_ENUM(0x500)，
     * 包括 RGBA/U8 ⇒ 纹理只被首帧填充过一次，之后每次上传都被拒、画面停在第一帧。 */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (g_nv12_fresh) {
        p_bind_texture(0x0DE1, tex_y);
        p_tex_image(0x0DE1, 0, 0x8229 /*R8*/, g_y_width, g_y_height, 0, 0x1903 /*RED*/, 0x1401, g_y_plane_ptr);
        p_bind_texture(0x0DE1, tex_uv);
        p_tex_image(0x0DE1, 0, 0x822B /*RG8*/, g_y_width / 2, g_y_height / 2, 0, 0x8227 /*RG*/, 0x1401, g_uv_plane_ptr);
    }
    ++g_y_uploads;

    p_use_program(program);
    p_active_texture(0x84C0 /*TEXTURE0*/);
    p_bind_texture(0x0DE1, tex_y);
    p_uniform1i(u_y_loc, 0);
    p_active_texture(0x84C1 /*TEXTURE1*/);
    p_bind_texture(0x0DE1, tex_uv);
    p_uniform1i(u_uv_loc, 1);
    p_uniform1i(u_flip_loc, wiliwili_video_flip);
    p_uniform1i(u_swap_loc, wiliwili_video_swap);
    p_uniform1i(u_709_loc, wiliwili_video_709);
    p_bind_vao(vao);
    static unsigned gl_err_seen, gl_err_last;
    if (p_get_error) {
        GLenum_t e = 0;
        while ((e = p_get_error()) != 0) {
            ++gl_err_seen;
            gl_err_last = (unsigned)e;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_upload_ms_total += (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    p_draw_arrays(0x0004 /*GL_TRIANGLES*/, 0, 3);

    ++g_draws;
    static int report_at = 0;
    extern int wiliwili_trace_enabled(void);
    if (++report_at >= 60 && wiliwili_trace_enabled()) {
        report_at = 0;
        char line[200];
        snprintf(line, sizeof(line), "vdec: fps~%d upload_avg_x100=%d glerr=%u last=0x%x uploads=%u", (int)g_draws / 3,
                 (int)(g_upload_ms_total / (double)g_y_uploads * 100.0), gl_err_seen, gl_err_last,
                 (unsigned)g_y_uploads);
        wiliwili_boot_log(line);
        g_draws = 0;
    }
}
