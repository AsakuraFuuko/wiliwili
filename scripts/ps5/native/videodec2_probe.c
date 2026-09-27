/*
 * sceVideodec2 硬解探针（原生标题 app slot 专用）。
 *
 * P2a（已通过，2026-09-26 真机）：完整 bring-up + 喂一帧 IDR ⇒ 全部 rc=0、出 NV12 帧。
 * P2b（本次）：连续解一段 Annex-B 流（30 帧，含 P 帧），并把解码出的 Y 平面当纹理每帧
 *             全屏绘制，测量"呈现"这一步的真实代价——这决定最终走哪条呈现路线
 *             （NV12 直接上传 / AGC 零拷贝）。
 *
 * 序列、结构体与常量取自 EVO-PLAYER-PS5 的 sce_videodec2.h 与 videodec2-abi.md（GPL-3.0）。
 * 触发：assets/wiliwili-options.txt 的 WILIWILI_TEST_VDEC=1；片源：assets/vdec-stream.h264。
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
#define RESOURCE_COMPUTE 1u
#define PIPELINE_SLOTS 3
#define MAX_AU 4096

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

/* ---- 状态 ---- */
static void *g_au_pool;
static void *g_frame_pool;
static uint64_t g_frame_size;
static void *g_decoder;

static uint8_t g_stream[0x100000];
static int g_stream_size;
static int g_au_offset[MAX_AU];
static int g_au_size[MAX_AU];
static int g_au_count;
static int g_au_index;

static uint8_t g_y_storage[1920 * 1088];  /* 稳定副本（帧池槽会被复用） */
static uint8_t g_uv_storage[1920 * 544];  /* NV12 的 UV 平面：半高度、交织 CbCr */
static const uint8_t *g_y_plane_ptr  = g_y_storage;
static const uint8_t *g_uv_plane_ptr = g_uv_storage;
static int g_y_width, g_y_height, g_y_pitch;
static unsigned long long g_y_uploads;
/* 每帧是否真的来了新数据（由播放器在取到新帧时置位）。为 0 时跳过纹理上传，
 * 只重画上一次的纹理——重传同样的数据在 AGC 上要 4+ ms/帧，纯属浪费。 */
static int g_nv12_fresh = 1;
void wiliwili_nv12_mark_fresh(int fresh) { g_nv12_fresh = fresh; }

/* ── 帧耗时分段统计（原生线调优） ─────────────────────────────────────────
 * 由 borealis 帧循环在三个位置调用：GL 提交开始 / 视频上屏结束 / swap 结束。
 * 每 30 帧汇总一行：submit 与 swap 各占多少毫秒——用来判断"卡"到底卡在
 * 提交（UI+视频绘制）还是卡在呈现（swap）。
 * ──────────────────────────────────────────────────────────────────────── */
static long long g_phase_submit_us, g_phase_swap_us;
static int g_phase_frames;
static long long phase_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000LL;
}
void wiliwili_frame_phase_begin(void) { g_phase_submit_us = phase_now_us(); }
void wiliwili_frame_phase_submit(void) {
    long long now = phase_now_us();
    g_phase_submit_us = now - g_phase_submit_us;
    g_phase_swap_us   = now;
}
void wiliwili_frame_phase_swap(void) {
    long long now = phase_now_us();
    g_phase_swap_us = now - g_phase_swap_us;
    extern int wiliwili_trace_enabled(void);
    if (++g_phase_frames >= 30 && wiliwili_trace_enabled()) {
        char lb[160];
        snprintf(lb, sizeof(lb), "frame: submit=%lldms swap=%lldms",
                 g_phase_submit_us / 1000, g_phase_swap_us / 1000);
        wiliwili_boot_log(lb);
        g_phase_frames = 0;
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

/* 把 Annex-B 流切成 AU：新的 slice（类型 1/5）到来且已有累积时切分。 */
static void split_stream(void) {
    int au_start   = 0;
    int seen_slice = 0;
    for (int i = 0; i + 3 < g_stream_size;) {
        if (g_stream[i] == 0 && g_stream[i + 1] == 0 && g_stream[i + 2] == 1) {
            int type = g_stream[i + 3] & 0x1F;
            if (type == 1 || type == 5) {
                if (seen_slice && g_au_count < MAX_AU) {
                    g_au_offset[g_au_count] = au_start;
                    g_au_size[g_au_count]   = i - au_start;
                    ++g_au_count;
                    au_start = i;
                }
                seen_slice = 1;
            }
            i += 3;
        } else {
            ++i;
        }
    }
    if (au_start < g_stream_size && g_au_count < MAX_AU) {
        g_au_offset[g_au_count] = au_start;
        g_au_size[g_au_count]   = g_stream_size - au_start;
        ++g_au_count;
    }
}

void wiliwili_videodec2_probe(void) {
    wiliwili_boot_log("vdec: enter");

    int32_t rc = sceSysmoduleLoadModule(SCE_SYSMODULE_VIDEODEC2);
    log2("vdec: sysmodule207 rc=%d", rc, 0);
    if (rc != 0) return;

    uint64_t limit = (uint64_t)sceKernelGetDirectMemorySize();
    log2("vdec: direct_limit=0x%lx", (long)limit, 0);

    ComputeMemoryInfo cm;
    memset(&cm, 0, sizeof(cm));
    cm.size = sizeof(cm);
    rc      = sceVideodec2QueryComputeMemoryInfo(&cm);
    log2("vdec: query_compute rc=%d size=0x%lx", rc, (long)cm.cpu_gpu_size);
    if (rc != 0) return;
    uint64_t cm_size = align16k(cm.cpu_gpu_size);
    cm.cpu_gpu       = alloc_direct(limit, cm_size, 0x33);
    cm.cpu_gpu_size  = cm_size;
    if (cm.cpu_gpu == NULL) return;

    ComputeConfigInfo cc;
    memset(&cc, 0, sizeof(cc));
    cc.size            = sizeof(cc);
    void *compute_queue = NULL;
    rc                 = sceVideodec2AllocateComputeQueue(&cc, &cm, &compute_queue);
    log2("vdec: compute_queue rc=%d", rc, 0);
    if (rc != 0) return;

    DecoderConfigInfo config;
    memset(&config, 0, sizeof(config));
    config.size                 = sizeof(config);
    config.resource_type        = RESOURCE_COMPUTE;
    config.codec_type           = CODEC_AVC;
    config.profile              = 100;
    config.max_level            = 51;
    config.max_width            = 640;
    config.max_height           = 368;
    config.max_dpb_frames       = 4;
    config.pipeline_depth       = 1;
    config.compute_queue        = (uint64_t)compute_queue;
    config.cpu_affinity         = 0x3F;
    config.cpu_priority         = 700;
    config.optimize_progressive = 1;

    DecoderMemoryInfo mem;
    memset(&mem, 0, sizeof(mem));
    mem.size = sizeof(mem);
    rc       = sceVideodec2QueryDecoderMemoryInfo(&config, &mem);
    log2("vdec: query_decoder rc=%d", rc, 0);
    if (rc != 0) return;

    uint64_t cpu_size = align16k(mem.cpu_size);
    void *cpu_ws      = NULL;
    if (cpu_size) sceKernelMapNamedFlexibleMemory(&cpu_ws, (size_t)cpu_size, 0x03, 0, "VdecCpu");
    mem.cpu      = cpu_ws;
    mem.cpu_size = cpu_size;
    uint64_t gpu_size = align16k(mem.gpu_size);
    mem.gpu           = gpu_size ? alloc_direct(limit, gpu_size, 0x32) : NULL;
    mem.gpu_size      = gpu_size;
    uint64_t cpu_gpu_size = align16k(mem.cpu_gpu_size);
    mem.cpu_gpu           = cpu_gpu_size ? alloc_direct(limit, cpu_gpu_size, 0x33) : NULL;
    mem.cpu_gpu_size      = cpu_gpu_size;

    g_frame_size = align16k(mem.max_frame_size);
    g_au_pool    = alloc_direct(limit, 0x800000u * PIPELINE_SLOTS, 0x32);
    g_frame_pool = alloc_direct(limit, g_frame_size * PIPELINE_SLOTS, 0x32);
    log2("vdec: pools au=%d frame=%d", g_au_pool != NULL, g_frame_pool != NULL);
    if (g_au_pool == NULL || g_frame_pool == NULL) return;

    rc = sceVideodec2CreateDecoder(&config, &mem, &g_decoder);
    log2("vdec: create_decoder rc=%d", rc, 0);
    if (rc != 0) return;
    log2("vdec: reset rc=%d", sceVideodec2Reset(g_decoder), 0);

    /* 读流 */
    int fd = open("/app0/assets/vdec-stream.h264", O_RDONLY);
    if (fd < 0) {
        wiliwili_boot_log("vdec: stream file missing");
        return;
    }
    g_stream_size = (int)read(fd, g_stream, sizeof(g_stream));
    close(fd);
    log2("vdec: stream bytes=%d", g_stream_size, 0);
    if (g_stream_size <= 0) return;

    split_stream();
    log2("vdec: aus=%d", g_au_count, 0);

    /* 连续解：环状 AU/帧槽，valid 时把 Y 平面拷进稳定缓冲 */
    int decoded  = 0;
    int buffered = 0;
    double total_ms = 0.0;
    int slot = 0;
    for (g_au_index = 0; g_au_index < g_au_count; ++g_au_index) {
        uint8_t *au_slot = (uint8_t *)g_au_pool + (size_t)slot * 0x800000u;
        memcpy(au_slot, g_stream + g_au_offset[g_au_index], (size_t)g_au_size[g_au_index]);

        InputData input;
        memset(&input, 0, sizeof(input));
        input.size    = sizeof(input);
        input.au      = au_slot;
        input.au_size = (uint64_t)g_au_size[g_au_index];
        input.pts     = (uint64_t)g_au_index;
        input.dts     = UINT64_MAX;

        FrameBuffer frame;
        memset(&frame, 0, sizeof(frame));
        frame.size        = sizeof(frame);
        frame.buffer      = (uint8_t *)g_frame_pool + (size_t)slot * g_frame_size;
        frame.buffer_size = g_frame_size;

        OutputInfo out;
        memset(&out, 0, sizeof(out));
        out.size = sizeof(out);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        rc = sceVideodec2Decode(g_decoder, &input, &frame, &out);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        total_ms += (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

        if (rc == 0 && out.valid == 0) {
            memset(&out, 0, sizeof(out));
            out.size = sizeof(out);
            rc       = sceVideodec2Flush(g_decoder, &frame, &out);
            ++buffered;
        }
        if (rc == 0 && out.valid) {
            ++decoded;
            /* 拷一份 Y 平面（NV12：Y 在前，pitch 个采样一行） */
            int copy_w = (int)out.width < 1920 ? (int)out.width : 1920;
            int copy_h = (int)out.height < 1088 ? (int)out.height : 1088;
            g_y_width  = copy_w;
            g_y_height = copy_h;
            g_y_pitch  = (int)out.pitch;
            const uint8_t *src = (const uint8_t *)out.buffer;
            /* Y：前 height 行；UV：紧跟其后、半高度（NV12 交织 CbCr）。都紧打包，便于直接上传。 */
            for (int y = 0; y < copy_h; ++y)
                memcpy(g_y_storage + (size_t)y * copy_w, src + (size_t)y * out.pitch, (size_t)copy_w);
            const uint8_t *uv_src = src + (size_t)out.pitch * copy_h;
            for (int y = 0; y < copy_h / 2; ++y)
                memcpy(g_uv_storage + (size_t)y * copy_w, uv_src + (size_t)y * out.pitch, (size_t)copy_w);
        }
        slot = (slot + 1) % PIPELINE_SLOTS;
    }
    log2("vdec: stream decoded=%d buffered=%d", decoded, buffered);
    log2("vdec: avg_decode_x100=%d", (long)(total_ms / (double)(g_au_count ? g_au_count : 1) * 100.0), 0);
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
    /* 注意：**不要**在这里判 g_ready —— 它是本探针自己的状态，只有 WILIWILI_TEST_VDEC 会置位。
     * 播放器（scripts/ps5/native/ps5_player.c）复用本函数做 NV12 上屏，一旦依赖它就永远
     * 在第一行 return：真机表现为"解码全部 valid=1、也有声音，但屏幕全白"。只用尺寸判断。 */
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
        p_tex_image      = (PFN_TexImage2D)get("glTexImage2D");
        p_tex_sub        = (PFN_TexSubImage2D)get("glTexSubImage2D");
        p_gen_textures   = (PFN_GenTextures)get("glGenTextures");
        p_bind_texture   = (PFN_BindTexture)get("glBindTexture");
        p_tex_param      = (PFN_TexParameteri)get("glTexParameteri");
        p_pixel_store    = (PFN_PixelStorei)get("glPixelStorei");
        p_active_texture = (PFN_ActiveTexture)get("glActiveTexture");
        p_create_shader  = (PFN_CreateShader)get("glCreateShader");
        p_shader_source  = (PFN_ShaderSource)get("glShaderSource");
        p_compile_shader = (PFN_CompileShader)get("glCompileShader");
        p_get_shader_iv  = (PFN_GetShaderiv)get("glGetShaderiv");
        p_shader_log     = (PFN_GetShaderInfoLog)get("glGetShaderInfoLog");
        p_create_program = (PFN_CreateProgram)get("glCreateProgram");
        p_attach_shader  = (PFN_AttachShader)get("glAttachShader");
        p_link_program   = (PFN_LinkProgram)get("glLinkProgram");
        p_get_program_iv = (PFN_GetProgramiv)get("glGetProgramiv");
        p_use_program    = (PFN_UseProgram)get("glUseProgram");
        p_uniform_location = (PFN_GetUniformLocation)get("glGetUniformLocation");
        p_uniform1i      = (PFN_Uniform1i)get("glUniform1i");
        p_gen_vaos       = (PFN_GenVertexArrays)get("glGenVertexArrays");
        p_bind_vao       = (PFN_BindVertexArray)get("glBindVertexArray");
        p_draw_arrays    = (PFN_DrawArrays)get("glDrawArrays");
        p_get_error      = (PFN_GetError)get("glGetError");
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
        p_tex_image(0x0DE1, 0, 0x822B /*RG8*/, g_y_width / 2, g_y_height / 2, 0, 0x8227 /*RG*/, 0x1401,
                    g_uv_plane_ptr);

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
            "uniform int u_flip;\n"      /* 0=不翻 1=水平 2=垂直 3=180 */
            "uniform int u_swap;\n"
            "uniform int u_709;\n"      /* 1=交换 U/V */
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
        u_y_loc  = p_uniform_location(program, "u_y");
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
        p_tex_image(0x0DE1, 0, 0x822B /*RG8*/, g_y_width / 2, g_y_height / 2, 0, 0x8227 /*RG*/, 0x1401,
                    g_uv_plane_ptr);
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
        while ((e = p_get_error()) != 0) { ++gl_err_seen; gl_err_last = (unsigned)e; }
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
                 (int)(g_upload_ms_total / (double)g_y_uploads * 100.0), gl_err_seen, gl_err_last, (unsigned)g_y_uploads);
        wiliwili_boot_log(line);
        g_draws = 0;
    }
}
