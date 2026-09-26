/*
 * 原生线播放器探针（P2e 装配）：ffmpeg 解封装 → 硬解视频（sceVideodec2）→ NV12 上屏；
 * 音频 ffmpeg 解码 → sceAudioOut 阻塞输出（它就是时钟）。
 *
 * 三个零件都已在本树里单独真机验证过：
 *   - 硬解：videodec2_probe.c（序列 rc=0、30 帧流 1.46 ms/帧）
 *   - 上屏：Y(R8)+UV(RG8) 双平面 + raw GL 三角形 + BT.601 YUV→RGB（60 fps、颜色正确）
 *   - 音频：audio_probe.c（Init → Open(0xFF,0,0,256,48000,1) → Output 阻塞，正数是正常返回）
 *
 * 触发：assets/wiliwili-options.txt 的 WILIWILI_TEST_PLAYER=<url>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

struct NVGcontext;

extern void wiliwili_boot_log(const char *message);
extern void *SDL_GL_GetProcAddress(const char *proc);
extern void wiliwili_draw_nv12(struct NVGcontext *vg, const uint8_t *y, const uint8_t *uv, int width, int height);

/* ---- 系统导入 ---- */
int32_t sceSysmoduleLoadModule(unsigned short id);
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd, size_t length, size_t alignment,
                                      int32_t memoryType, int64_t *physicalAddr);
int32_t sceKernelMapDirectMemory(void **addr, size_t length, int32_t prot, int32_t flags, int64_t physicalAddr,
                                 size_t alignment);
int32_t sceKernelMapNamedFlexibleMemory(void **addr, size_t length, int32_t prot, int32_t flags, const char *name);
int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int handle, const void *ptr);
int sceAudioOutClose(int handle);
int32_t sceVideodec2QueryComputeMemoryInfo(void *memory);
int32_t sceVideodec2AllocateComputeQueue(const void *config, const void *memory, void **queue);
int32_t sceVideodec2QueryDecoderMemoryInfo(const void *config, void *memory);
int32_t sceVideodec2CreateDecoder(const void *config, const void *memory, void **decoder);
int32_t sceVideodec2Reset(void *decoder);
int32_t sceVideodec2Decode(void *decoder, void *input, void *frame, void *output);
int32_t sceVideodec2Flush(void *decoder, void *frame, void *output);

#define SCE_SYSMODULE_VIDEODEC2 0xCF
#define CODEC_AVC 1u
#define RESOURCE_COMPUTE 1u
/* 环尺寸与 DPB 必须回到**真机已验证可用**的那组值：
 * 探针（30 帧连解 rc 全 0）与 21:32 那次真实 B 站视频成功播放，用的都是
 * `pipeline_depth=1` + DPB=4 + 单一环索引。
 * 教训（两次踩同一个坑）：照抄 EVO 的 `pipeline_depth=4` / 帧池 8 / DPB 16 都会让
 * `sceVideodec2Decode` 对**每一帧**返回 0x811D0302/0303（valid=0）——那不是"容量不够"，
 * 而是与驱动的内部假设冲突。改这些数之前先在真机上证明它比现在更好。 */
#define AU_SLOTS 4          /* 输入 AU 环 */
#define FRAME_SLOTS 4       /* 帧池（与输入环一致） */
/* `max_dpb_frames` 必须是 SCE_VIDEODEC2_AUTO_FRAMES(-1)，让**解码器按码流自定尺寸**。
 * 写死 4 会让"参考帧多于 4"的流整段解不出来：实测某 B 站 360P 流的 SPS 声明
 * `max_num_ref_frames=7`，解码器每帧返回 0x811D0302、valid=0——现象只有声音、画面全白。
 * （早期笔记里"EVO 用 -1"是对的，后来把它改成 4 是回归。） */
#define MAX_DPB_FRAMES (-1)
#define PIPELINE_DEPTH 1
#define AUDIO_GRAIN 256 /* 每块 256 帧（EVO 的可用实现用的就是它） */
#define AUDIO_FREQ 48000

/* ---- videodec2 结构体（同 videodec2_probe.c） ---- */
typedef struct {
    uint64_t size; uint32_t resource_type, codec_type, profile, max_level;
    int32_t max_width, max_height, max_dpb_frames;
    uint32_t pipeline_depth; uint64_t compute_queue, cpu_affinity;
    int32_t cpu_priority; uint32_t optimize_progressive, check_memory_type, reserved;
} DecoderConfigInfo;
typedef struct {
    uint64_t size, cpu_size; void *cpu; uint64_t gpu_size; void *gpu;
    uint64_t cpu_gpu_size; void *cpu_gpu; uint64_t max_frame_size;
    uint32_t frame_alignment, reserved;
} DecoderMemoryInfo;
typedef struct { uint64_t size; uint16_t pipe_id, queue_id; uint8_t check_memory_type, reserved0; uint16_t reserved1; } ComputeConfigInfo;
typedef struct { uint64_t size, cpu_gpu_size; void *cpu_gpu; } ComputeMemoryInfo;
typedef struct { uint64_t size; void *au; uint64_t au_size, pts, dts, attached; } InputData;
typedef struct { uint64_t size; void *buffer; uint64_t buffer_size; uint32_t accepted, reserved; } FrameBuffer;
typedef struct {
    uint64_t size; uint8_t valid, error, picture_count, padding;
    uint32_t codec, width, pitch, height, reserved;
    void *buffer; uint64_t buffer_size; uint32_t frame_format, pitch_bytes;
} OutputInfo;

/* ---- 状态 ---- */
static void *g_au_pool, *g_frame_pool, *g_decoder;
static uint64_t g_frame_size;
static uint8_t g_y_plane[1920 * 1088];
static uint8_t g_uv_plane[1920 * 544];
static int g_y_width, g_y_height;
static int g_ready;
/* 硬解环索引与自增序号：随 open 重置，且只能由唯一生产者推进。 */
static int g_au_slot;
/* AU 环索引：输入槽与帧槽都以它为模（EVO 的做法是各自取模，见 decode_au）。
 * 随 open/close 归零；只能由唯一生产者推进。 */
static unsigned g_au_ring;
static uint64_t g_au_seq;
static int g_dbg_au;
/* 帧搬运：工作线程写、渲染线程读。锁保护，只搬运"最新一帧"。
 * 详见文件后部"播放线程"小节对单生产者约束的说明。 */
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_frame_new;                 /* 搬运缓冲里有尚未上传的帧 */
static uint8_t g_stage_y[1920 * 1088];  /* Y 面 */
static uint8_t g_stage_uv[1920 * 544];  /* NV12 的 UV 面只有 Y 的一半 */
/* 工作线程私有的解码目标：解码在这里做（不持锁），解完才在锁内拷进 g_stage_*。 */
static uint8_t g_work_y[1920 * 1088];
static uint8_t g_work_uv[1920 * 544];
static int g_stage_w, g_stage_h;

/* ffmpeg */
static AVFormatContext *g_fmt;
static int g_video_index = -1, g_audio_index = -1;
static AVFormatContext *g_afmt; /* 音频单独一条 URL 时（B 站 DASH）用它 */
static int g_video_eof;
static AVBSFContext *g_bsf;
static AVCodecContext *g_adec;
static struct SwrContext *g_swr;
static AVPacket *g_pkt;
static AVFrame *g_aframe;
static int64_t g_last_video_pts_us;

/* 音频：按 256 帧一块喂；块数就是时钟 */
/* PCM 缓冲要装下"补料目标"整批（AUDIO_GRAIN * PCM_TARGET_BLOCKS 帧）。
 * 曾经只开 1 块（AUDIO_GRAIN*2 个 int16）却按 g_pcm_frames 偏移连着转 4 块：
 * 第 2 次 swr_convert 起就越界，正好写进紧邻的 video_submit_packet.slot，
 * 表现为 AU 槽索引变成 PCM 采样值(0xB3E4A9C) → memcpy 野指针崩溃。 */
/* 每帧补料目标。**必须 >= 每秒渲染帧数 × 单帧所需块数**：播放页只有 6~8 fps
 * （截图 FPS:6），4 块时每秒只能推进 24 块 ≈ 128ms 音频 ⇒ 画面像冻住（真机实测
 * clock_ms 100 秒才走 2.5 秒）。32 块 ≈ 170ms，8fps 下就够实时。 */
#define PCM_TARGET_BLOCKS 32
static int16_t g_pcm[AUDIO_GRAIN * 2 * PCM_TARGET_BLOCKS];
static int g_pcm_frames;
static int g_audio_handle = -1;
static unsigned long long g_audio_blocks;
static int g_audio_eof;
static int g_paused;
static char g_url[1024]; /* 当前打开的 URL（用于幂等判断） */
static int g_overlay; /* 探针模式：在 UI 之后重复画一遍，便于肉眼确认 */
static float g_rect_x, g_rect_y, g_rect_w, g_rect_h; /* 播放器区域（nvg 左上角原点） */

static void plog1(const char *fmt, long a) {
    char line[160];
    snprintf(line, sizeof(line), fmt, a);
    wiliwili_boot_log(line);
}
static void plog2(const char *fmt, long a, long b) {
    char line[192];
    snprintf(line, sizeof(line), fmt, a, b);
    wiliwili_boot_log(line);
}
static void plog3(const char *fmt, long a, long b, long c) {
    char line[192];
    snprintf(line, sizeof(line), fmt, a, b, c);
    wiliwili_boot_log(line);
}

static uint64_t align16k(uint64_t v) { return (v + 0x3FFF) & ~0x3FFFuLL; }

static void *alloc_direct(uint64_t limit, uint64_t size, int32_t prot) {
    int64_t start = 0;
    if (sceKernelAllocateDirectMemory(0, (int64_t)limit, (size_t)size, 0x4000, 12, &start) != 0) return NULL;
    void *address = NULL;
    if (sceKernelMapDirectMemory(&address, (size_t)size, prot, 0, start, 0x4000) != 0) return NULL;
    return address;
}

/* ---- 硬解初始化（照抄已验证的序列） ---- */
static int decoder_init(int width, int height) {
    int32_t rc = sceSysmoduleLoadModule(SCE_SYSMODULE_VIDEODEC2);
    plog1("player: sysmodule rc=%d", rc);
    if (rc != 0) return -1;

    uint64_t limit = (uint64_t)sceKernelGetDirectMemorySize();
    ComputeMemoryInfo cm;
    memset(&cm, 0, sizeof(cm));
    cm.size = sizeof(cm);
    rc      = sceVideodec2QueryComputeMemoryInfo(&cm);
    if (rc != 0) return -1;
    uint64_t cm_size = align16k(cm.cpu_gpu_size);
    cm.cpu_gpu       = alloc_direct(limit, cm_size, 0x33);
    cm.cpu_gpu_size  = cm_size;
    if (!cm.cpu_gpu) return -1;

    ComputeConfigInfo cc;
    memset(&cc, 0, sizeof(cc));
    cc.size             = sizeof(cc);
    void *compute_queue = NULL;
    if (sceVideodec2AllocateComputeQueue(&cc, &cm, &compute_queue) != 0) return -1;

    DecoderConfigInfo config;
    memset(&config, 0, sizeof(config));
    config.size                 = sizeof(config);
    config.resource_type        = RESOURCE_COMPUTE;
    config.codec_type           = CODEC_AVC;
    config.profile              = 100;
    config.max_level            = 51;
    /* 宽高必须按 16 对齐再交给解码器：对齐后的 max_frame_size 才是解码器真正要写的尺寸。
     * 用未对齐的 360 会把帧缓冲算小 → 解码器越界写 → 前几帧看着正常、随后崩溃。
     * 已验证的探针里写的是 640x368（= 360 对齐到 368）。 */
    config.max_width            = (width + 15) & ~15;
    config.max_height           = (height + 15) & ~15;
    config.max_dpb_frames       = MAX_DPB_FRAMES; /* -1 = 让解码器按码流自定（见上方说明） */
    config.pipeline_depth       = PIPELINE_DEPTH; /* 1 = 已验证值，勿提高（见上方说明） */
    config.compute_queue        = (uint64_t)compute_queue;
    config.cpu_affinity         = 0x3F;
    config.cpu_priority         = 700;
    config.optimize_progressive = 1;

    DecoderMemoryInfo mem;
    memset(&mem, 0, sizeof(mem));
    mem.size = sizeof(mem);
    if (sceVideodec2QueryDecoderMemoryInfo(&config, &mem) != 0) return -1;

    uint64_t cpu_size = align16k(mem.cpu_size);
    void *cpu_ws      = NULL;
    if (cpu_size) sceKernelMapNamedFlexibleMemory(&cpu_ws, (size_t)cpu_size, 0x03, 0, "VdecCpu");
    mem.cpu      = cpu_ws;
    mem.cpu_size = cpu_size;
    uint64_t gpu_size = align16k(mem.gpu_size);
    mem.gpu           = gpu_size ? alloc_direct(limit, gpu_size, 0x32) : NULL;
    uint64_t cpu_gpu_size = align16k(mem.cpu_gpu_size);
    mem.cpu_gpu           = cpu_gpu_size ? alloc_direct(limit, cpu_gpu_size, 0x33) : NULL;
    mem.cpu_gpu_size      = cpu_gpu_size;

    g_frame_size = align16k(mem.max_frame_size);
    {
        char lb[200];
        snprintf(lb, sizeof(lb), "player: dec mem frame_size=%d cpu=%d gpu=%d cpu_gpu=%d", (int)g_frame_size,
                 (int)mem.cpu_size, (int)mem.gpu_size, (int)mem.cpu_gpu_size);
        wiliwili_boot_log(lb);
    }
    g_au_pool    = alloc_direct(limit, 0x800000u * AU_SLOTS, 0x32);
    g_frame_pool = alloc_direct(limit, g_frame_size * FRAME_SLOTS, 0x32);
        if (!g_au_pool || !g_frame_pool) return -1;

    if (sceVideodec2CreateDecoder(&config, &mem, &g_decoder) != 0) return -1;
    plog1("player: decoder reset rc=%d", sceVideodec2Reset(g_decoder));
    return 0;
}

/* ---- 音频：把缓存的 PCM 按块送出（Output 阻塞 = 时钟） ---- */
static void audio_push_blocks(int max_blocks) {
    while (g_pcm_frames >= AUDIO_GRAIN && max_blocks > 0) {
        int rc = sceAudioOutOutput(g_audio_handle, g_pcm);
        if (rc < 0) {
            plog1("player: audio out rc=%d", rc);
            g_audio_eof = 1;
            return;
        }
        ++g_audio_blocks;
        g_pcm_frames -= AUDIO_GRAIN;
        if (g_pcm_frames > 0) memmove(g_pcm, g_pcm + AUDIO_GRAIN * 2, (size_t)g_pcm_frames * 2 * sizeof(int16_t));
        --max_blocks;
    }
}

static void audio_decode_one_frame(void) {
    if (g_audio_index < 0 || !g_adec) return;
    if (avcodec_receive_frame(g_adec, g_aframe) != 0) return;

    int room               = AUDIO_GRAIN * PCM_TARGET_BLOCKS - g_pcm_frames;
    uint8_t *out[1]        = {(uint8_t *)g_pcm + (size_t)g_pcm_frames * 2 * sizeof(int16_t)};
    int out_samples = room > 0 ? swr_convert(g_swr, out, room, (const uint8_t **)g_aframe->data, g_aframe->nb_samples) : 0;
    if (out_samples > 0) g_pcm_frames += out_samples;
    av_frame_unref(g_aframe);
}

/* ---- 主循环（由帧循环驱动）：喂音频、按音频时钟解视频、上屏 ---- */
static int64_t g_audio_clock_us(void) { return (int64_t)(g_audio_blocks * AUDIO_GRAIN * 1000000ULL / AUDIO_FREQ); }


void wiliwili_ps5player_close(void); /* 定义在下面：换片时先收尾 */
/* 播放线程（定义在文件后部）。变量只在这里声明一次：worker 是否在跑、线程句柄。 */
static void *player_worker(void *arg);
static volatile int g_worker_run;
static volatile int g_worker_started;
static pthread_t g_worker_tid;

/* ── 位流过滤器（Annex-B 转换 + 参数集注入） ──────────────────────────────
 * `h264_mp4toannexb` 把 mp4 的 avcC 参数集（SPS/PPS）注入到**它的第一个输出包**
 * （即首个 IDR）上——这是**一次性**的。`av_bsf_flush()` 会丢掉缓冲状态但**不会**
 * 重新武装这次注入，因此换片/seek 之后的首个 IDR 会带着空的参数集到解码器，
 * 解码器对每一帧返回 0x811D0302/0303（valid=0）⇒ "有声音、画面全白"。
 * 正解（EVO-PLAYER-PS5 的 #57 同款处理）：**重建过滤器**，新过滤器会重新注入。
 * ──────────────────────────────────────────────────────────────────────── */
static AVCodecParameters *g_bsf_par; /* par_in 的副本，供重建时用 */

static int bsf_rebuild(void) {
    /* 只释放旧过滤器；`g_bsf_par` 是**输入**，必须保留（曾经顺手在这里把它 free 掉，
     * 结果每次重建都因参数为空返回 -1，视频路径被整体禁用）。 */
    if (g_bsf) av_bsf_free(&g_bsf);
    g_bsf = NULL;
    if (g_bsf_par == NULL) {
        wiliwili_boot_log("player: bsf rebuild: params missing");
        return -1;
    }
    const AVBitStreamFilter *bf = av_bsf_get_by_name("h264_mp4toannexb");
    if (bf == NULL) {
        wiliwili_boot_log("player: bsf rebuild: filter not registered");
        return -1;
    }
    if (av_bsf_alloc(bf, &g_bsf) < 0) {
        g_bsf = NULL;
        wiliwili_boot_log("player: bsf rebuild: alloc failed");
        return -1;
    }
    if (avcodec_parameters_copy(g_bsf->par_in, g_bsf_par) < 0) {
        av_bsf_free(&g_bsf);
        wiliwili_boot_log("player: bsf rebuild: params copy failed");
        return -1;
    }
    if (av_bsf_init(g_bsf) < 0) {
        av_bsf_free(&g_bsf);
        wiliwili_boot_log("player: bsf rebuild: init failed");
        return -1;
    }
    return 0;
}

void wiliwili_ps5player_open(const char *url, const char *audio_url) {
    char line[192];
    plog1("player: enter %d", url != NULL);

    /* app 会为一个视频多次调用 setUrl/setBackupUrl。不设防的话每次都会重建解码器、
     * 重开音频句柄、重分配缓存 ⇒ 播放被打乱（真机：画面卡住）甚至崩溃（addr=0x10）。
     * 同一个 URL 直接忽略；换片则先真正收尾。 */
    if (g_ready && url != NULL && strcmp(g_url, url) == 0) {
        wiliwili_boot_log("player: same url, ignored");
        return;
    }
    if (g_ready) {
        wiliwili_boot_log("player: switching source, closing previous");
        wiliwili_ps5player_close();
    }
    if (url != NULL) snprintf(g_url, sizeof(g_url), "%s", url);

    /* 音频：照抄可用配方（0xFF, 0, 0, 256, 48000, 1）；句柄 < 1 才算失败；Output 只认负数失败 */
    sceAudioOutInit();
    g_audio_handle = sceAudioOutOpen(0xFF, 0, 0, AUDIO_GRAIN, AUDIO_FREQ, 1);
    plog1("player: audio handle=%d", g_audio_handle);
    if (g_audio_handle < 1) {
        wiliwili_boot_log("player: audio open failed");
        return;
    }

    avformat_network_init();
    {
        char lb[220];
        snprintf(lb, sizeof(lb), "player: url=%.150s", url ? url : "(null)");
        wiliwili_boot_log(lb);
        if (url && strlen(url) > 150) {
            snprintf(lb, sizeof(lb), "player: url2=%.150s", url + 150);
            wiliwili_boot_log(lb);
        }
        if (audio_url && audio_url[0]) {
            snprintf(lb, sizeof(lb), "player: audio_url=%.80s", audio_url);
            wiliwili_boot_log(lb);
        }
    }
    /* B 站是 https：给 ffmpeg 的 TLS 指 CA（随包安装，绝不关闭校验）。 */
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "ca_file", "/app0/assets/ca-bundle.crt", 0);
    /* B 站 CDN 强制校验 Referer（app 给 mpv 设的就是 "https://www.bilibili.com"，
     * 见 video_view.cpp:812），不带就是 403 ⇒ 打不开 ⇒ 播放器全白。UA 也用浏览器串。 */
    av_dict_set(&opts, "referer", "https://www.bilibili.com", 0);
    av_dict_set(&opts, "user_agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 "
                "Safari/537.36",
                0);
    int open_rc = avformat_open_input(&g_fmt, url, NULL, &opts);
    av_dict_free(&opts);
    plog1("player: open rc=%d", open_rc);
    if (open_rc < 0) {
        wiliwili_boot_log("player: open input failed");
        return;
    }
    if (avformat_find_stream_info(g_fmt, NULL) < 0) {
        wiliwili_boot_log("player: find stream info failed");
        return;
    }
    /* 报出探测到的流数与时长：`moov` 在文件尾且走 HTTP 顺序读时，ffmpeg 拿不到完整
     * 索引（nb_streams/duration 全 0 或异常），表现是"open rc=0 但一个包都读不出来"。
     * 这一行能让该现象一眼可见，不必再靠逐层打点。 */
    {
        char lb[200];
        snprintf(lb, sizeof(lb), "player: fmt streams=%d duration=%lld start_time=%lld", (int)g_fmt->nb_streams,
                 (long long)g_fmt->duration, (long long)g_fmt->start_time);
        wiliwili_boot_log(lb);
    }
    extern int wiliwili_video_flip, wiliwili_video_swap, wiliwili_video_709;
    {
        /* 默认 180° 旋转（真机实测颠倒）；可用环境变量覆盖以便逐一比对。 */
        const char *f = getenv("WILIWILI_VIDEO_FLIP");
        const char *w = getenv("WILIWILI_VIDEO_SWAP");
        if (f) wiliwili_video_flip = atoi(f);
        if (w) wiliwili_video_swap = atoi(w);
    }
    g_video_index = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    g_audio_index = av_find_best_stream(g_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    /* B 站 DASH：视频与音频是两条独立 URL（app 通过 audios 参数传进来）。
     * 音频单独开一个上下文，视频仍走 g_fmt。二者都只有一条流时最省事。 */
    if (audio_url != NULL && audio_url[0] != '\0' && strcmp(audio_url, url) != 0) {
        /* audio_url 可能是多行候选（app 的 audios 列表 = base + backup）：逐个试，
         * 第一个能开的就用。B 站会下发 mcdn 这类 P2P CDN，实测会打不开（EIO），
         * 只取第一个就是"有画面没声音"或反过来。 */
        const char *p   = audio_url;
        int tries       = 0;
        while (*p != '\0' && g_afmt == NULL && tries < 6) {
            const char *nl = strchr(p, '\n');
            size_t len     = nl ? (size_t)(nl - p) : strlen(p);
            char one[1024];
            if (len >= sizeof(one)) len = sizeof(one) - 1;
            memcpy(one, p, len);
            one[len] = '\0';
            ++tries;
            AVDictionary *aopts = NULL;
            av_dict_set(&aopts, "ca_file", "/app0/assets/ca-bundle.crt", 0);
            av_dict_set(&aopts, "referer", "https://www.bilibili.com", 0);
            av_dict_set(&aopts, "user_agent",
                        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                        "Chrome/120.0.0.0 Safari/537.36",
                        0);
            int arc = avformat_open_input(&g_afmt, one, NULL, &aopts);
            av_dict_free(&aopts);
            plog2("player: audio open try=%d rc=%d", (long)tries, (long)arc);
            if (arc >= 0) {
                avformat_find_stream_info(g_afmt, NULL);
                g_audio_index = av_find_best_stream(g_afmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
                wiliwili_boot_log("player: separate audio url");
            } else {
                g_afmt = NULL;
            }
            if (nl == NULL) break;
            p = nl + 1;
        }
    }
    plog2("player: streams v=%d a=%d", g_video_index, g_audio_index);

    /* 解码器必须按**真实流尺寸**建：写死小尺寸会让解码器越界写帧缓冲（1080p 必崩）。 */
    {
        int vw = 640, vh = 368;
        if (g_video_index >= 0) {
            AVCodecParameters *vp = g_fmt->streams[g_video_index]->codecpar;
            if (vp->width > 0) vw = vp->width;
            if (vp->height > 0) vh = vp->height;
            if (vp->codec_id != AV_CODEC_ID_H264) {
                /* 非 H.264（B 站 4K 是 HEVC）：先明确报出来，别装作能播。 */
                plog1("player: unsupported codec id=%d", (long)vp->codec_id);
                wiliwili_boot_log("player: only H.264 supported for now");
                return;
            }
        }
        plog2("player: video size w=%d h=%d", vw, vh);
        wiliwili_video_709 = (vh >= 720) ? 1 : 0; /* HD 用 BT.709，SD 用 BT.601 */
        {
            char lb[160];
            snprintf(lb, sizeof(lb), "player: display flip=%d swap=%d 709=%d", wiliwili_video_flip, wiliwili_video_swap,
                     wiliwili_video_709);
            wiliwili_boot_log(lb);
        }
        if (g_decoder == NULL) {
            if (decoder_init(vw, vh) != 0) {
                wiliwili_boot_log("player: decoder init failed");
                return;
            }
        } else {
            plog1("player: decoder reuse reset rc=%d", sceVideodec2Reset(g_decoder));
        }
    }

/* H.264：mp4 是 AVCC，硬解要 Annex-B，用 bitstream filter 转换（并附上 SPS/PPS） */
    if (g_video_index >= 0) {
        wiliwili_boot_log("player: bsf lookup");
        if (g_bsf_par) avcodec_parameters_free(&g_bsf_par);
        g_bsf_par = avcodec_parameters_alloc();
        if (g_bsf_par == NULL ||
            avcodec_parameters_copy(g_bsf_par, g_fmt->streams[g_video_index]->codecpar) < 0) {
            wiliwili_boot_log("player: bsf params copy failed");
            return;
        }
        plog1("player: bsf rebuild rc=%d", bsf_rebuild());
        if (g_bsf_par && g_bsf_par->extradata) {
        }
        if (g_bsf == NULL) {
            wiliwili_boot_log("player: bsf unavailable, video path disabled");
            return;
        }
    }

    /* 音频解码 + 重采样到 S16 立体声 48k */
    if (g_audio_index >= 0) {
        /* 音频源的流必须从**音频上下文**取 codecpar：DASH 时 g_audio_index 是 g_afmt 里的下标，
         * 拿去索引 g_fmt 会建出错误的解码器（表现：没有 PCM、时钟停在 0、视频被一起拖住）。 */
        AVFormatContext *asrc = g_afmt ? g_afmt : g_fmt;
        const AVCodec *adec  = avcodec_find_decoder(asrc->streams[g_audio_index]->codecpar->codec_id);
        if (adec) {
            g_adec = avcodec_alloc_context3(adec);
            avcodec_parameters_to_context(g_adec, asrc->streams[g_audio_index]->codecpar);
            wiliwili_boot_log("player: audio codec found");
            if (avcodec_open2(g_adec, adec, NULL) == 0) {
                wiliwili_boot_log("player: audio decoder open");
                AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&g_swr, &out_layout, AV_SAMPLE_FMT_S16, AUDIO_FREQ,
                                        &g_adec->ch_layout, g_adec->sample_fmt, g_adec->sample_rate, 0, NULL) == 0) {
                    swr_init(g_swr);
                }
                plog2("player: audio codec id=%d sr=%d", (long)adec->id, g_adec->sample_rate);
                wiliwili_boot_log("player: audio decoder ready");
            } else {
                g_adec = NULL;
            }
        }
    }

    if (g_adec == NULL) {
        /* 没有可用的音频源：直接视作 eof，让视频按帧自走（否则会被"时钟+300ms"冻住）。 */
        g_audio_eof = 1;
        wiliwili_boot_log("player: no audio source, video free-runs");
    }

    g_pkt    = av_packet_alloc();
    g_aframe = av_frame_alloc();
    /* 环索引随每次 open 归零：跨片累加会让解码器仍持有的槽被覆盖（野指针崩溃）。 */
    g_au_slot    = 0;
    g_au_seq     = 0;
    g_dbg_au     = 0;
    g_au_ring    = 0;
    g_frame_new  = 0;
    g_ready  = 1;

    /* DASH（双 URL）时把读取/解码/音频推送搬到独立线程：渲染线程只有 6~8fps，
     * 留在渲染线程里播放速度会被帧率绑死（实测慢约 8 倍）。 */
    if (g_afmt != NULL && getenv("WILIWILI_PLAYER_THREAD") == NULL) {
        g_worker_run = 1;
        if (pthread_create(&g_worker_tid, NULL, player_worker, NULL) != 0) {
            g_worker_run = 0;
            wiliwili_boot_log("player: worker thread create failed");
        } else {
            wiliwili_boot_log("player: worker thread started");
        }
    }
    wiliwili_boot_log("player: ready");
}

/* ── Annex-B AU 聚合 ───────────────────────────────────────────────────────
 * `h264_mp4toannexb` 的输出**不是**访问单元：它把每个 NAL 单独成包（SEI、SPS、PPS、
 * 片各自一包）。直接把这些包逐个喂给解码器，解码器永远收不到"含参数集的关键帧 AU"，
 * 每帧返回 0x811D0303（valid=0）——一帧都出不来。已验证的探针是按 **AU 边界**切分
 * （下一个 VCL 片之前一切归上一个 AU），这里照做：
 *   把 bsf 的每个输出包追加进 8 MB 的 AU 缓冲；检测到"新的 VCL 片起始"且缓冲里
 *   已经有过一个 VCL 片时，就先把攒好的 AU 送去解码。
 * ──────────────────────────────────────────────────────────────────────── */

/* ── 访问单元（AU）提交 ─────────────────────────────────────────────────────
 * 关键设计（照 EVO-PLAYER-PS5 的既有实现，见 evo_vdec_native.c）：
 * **不要自己扫描 Annex-B 起始码来切 AU**。解复用器给出的每个样本就是一个 AU，
 * `h264_mp4toannexb` 的输出**保持该样本边界** ⇒ 「每个 bsf 输出包 = 一个 AU」。
 * 手写扫描会在片数据里误把 `00 00 01` 当 NAL 边界，把 AU 切碎，解码器对每一帧
 * 返回 0x811D0302/0303（valid=0），现象是"有声音、画面全白"。
 *
 * 参数集（SPS/PPS）：`h264_mp4toannexb` 的注入是**一次性**的（首个 IDR 才有）。
 * 需要重新注入时——例如换片/seek 之后——**重建 bsf**（不是 `av_bsf_flush`），
 * 新建的过滤器会在它的第一个输出包上重新注入参数集。
 * ──────────────────────────────────────────────────────────────────────── */

static void decode_au(const uint8_t *au, size_t len, uint8_t *dst_y, uint8_t *dst_uv, int *out_w, int *out_h,
                      int64_t pts_us) {
    if (!g_decoder || len == 0 || len > 0x800000) return;
    unsigned islot = g_au_ring % AU_SLOTS;
    unsigned fslot = g_au_ring % FRAME_SLOTS;
    ++g_au_ring;
    uint8_t *au_slot = (uint8_t *)g_au_pool + (size_t)islot * 0x800000u;
    if (au != au_slot) memcpy(au_slot, au, len);
    InputData input;
    memset(&input, 0, sizeof(input));
    input.size    = sizeof(input);
    input.au      = au_slot;
    input.au_size = (uint64_t)len;
    /* pts 用自增序号：已验证的探针就是 0,1,2…。A/V 同步靠音频时钟的 g_last_video_pts_us。 */
    input.pts     = (uint64_t)(g_au_seq++);
    input.dts     = UINT64_MAX;
    FrameBuffer frame;
    memset(&frame, 0, sizeof(frame));
    frame.size        = sizeof(frame);
    frame.buffer      = (uint8_t *)g_frame_pool + (size_t)fslot * g_frame_size;
    frame.buffer_size = g_frame_size;
    OutputInfo oi;
    memset(&oi, 0, sizeof(oi));
    oi.size = sizeof(oi);
    int rc  = sceVideodec2Decode(g_decoder, &input, &frame, &oi);
    if (rc == 0 && oi.valid == 0) {
        /* 还没攒够帧：Flush 把已解出的帧吐出来（复用同一个 `frame`，同已验证探针）。 */
        memset(&oi, 0, sizeof(oi));
        oi.size = sizeof(oi);
        (void)sceVideodec2Flush(g_decoder, &frame, &oi);
    }
    /* 前几个 AU 的提交结果：解码器一旦拒绝（rc != 0 或 valid = 0）在这里就能看到。 */
    if (g_dbg_au < 4) {
        ++g_dbg_au;
        plog3("player: au%d size=%d dec=%d", (long)g_dbg_au, (long)len, (long)rc);
    }
    /* A/V 节流用真实 PTS。 */
    if (pts_us >= 0) g_last_video_pts_us = pts_us;
    /* 几何量必须自检后再用：解码器失败/未填充时会留下未初始化的 width/pitch，
     * 照着它逐行 memcpy 就是野读野写（实测崩在映像内部，addr=0x86caed）。 */
    if (oi.valid && oi.buffer && oi.width > 0 && oi.height > 0 && oi.pitch >= (uint32_t)oi.width &&
        oi.width <= 1920 && oi.height <= 1088 && oi.pitch <= 4096) {
        int w = (int)oi.width, h = (int)oi.height;
        const uint8_t *src = (const uint8_t *)oi.buffer;
        if (oi.buffer_size >= (uint64_t)oi.pitch * (h + h / 2)) {
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            for (int y = 0; y < h; ++y)
                memcpy(dst_y + (size_t)y * w, src + (size_t)y * oi.pitch, (size_t)w);
            const uint8_t *uv = src + (size_t)oi.pitch * h;
            for (int y = 0; y < h / 2; ++y)
                memcpy(dst_uv + (size_t)y * w, uv + (size_t)y * oi.pitch, (size_t)w);
        }
    } else if (oi.valid && oi.error) {
        static int bad = 0;
        if (bad++ < 4) plog2("player: decode error=%d rc=%d", (long)oi.error, (long)rc);
    }
}

/* 把一包（= 一个 AU）经 bsf 之后送进解码器。 */
static void video_submit_packet(AVPacket *pkt, uint8_t *dst_y, uint8_t *dst_uv, int *out_w, int *out_h) {
    if (!g_bsf) return;
    AVPacket *out = av_packet_alloc();
    if (av_bsf_send_packet(g_bsf, pkt) != 0) {
        av_packet_free(&out);
        return;
    }
    while (av_bsf_receive_packet(g_bsf, out) == 0) {
        if (out->data != NULL && out->size > 0 && out->size <= 0x800000) {
            int64_t pts_us = -1;
            if (out->pts != AV_NOPTS_VALUE && g_fmt && g_video_index >= 0)
                pts_us = av_rescale_q(out->pts, g_fmt->streams[g_video_index]->time_base, AV_TIME_BASE_Q);
            decode_au(out->data, (size_t)out->size, dst_y, dst_uv, out_w, out_h, pts_us);
        }
        av_packet_unref(out);
    }
    av_packet_free(&out);
}

/* 流结束：把 bsf 里缓冲的尾部刷出来（发送 NULL 包 = drain）。 */
static void video_submit_flush(uint8_t *dst_y, uint8_t *dst_uv, int *out_w, int *out_h) {
    if (!g_bsf) return;
    AVPacket *out = av_packet_alloc();
    if (av_bsf_send_packet(g_bsf, NULL) == 0) {
        while (av_bsf_receive_packet(g_bsf, out) == 0) {
            if (out->data != NULL && out->size > 0 && out->size <= 0x800000)
                decode_au(out->data, (size_t)out->size, dst_y, dst_uv, out_w, out_h, -1);
            av_packet_unref(out);
        }
    }
    av_packet_free(&out);
    /* 解码器内部还有重排缓冲，Flush 出来。 */
    if (g_decoder && g_frame_pool) {
        FrameBuffer fb;
        OutputInfo oi;
        for (int i = 0; i < 8; ++i) {
            memset(&fb, 0, sizeof(fb));
            fb.size        = sizeof(fb);
            fb.buffer      = (uint8_t *)g_frame_pool + (size_t)(g_au_ring % FRAME_SLOTS) * g_frame_size;
            fb.buffer_size = g_frame_size;
            memset(&oi, 0, sizeof(oi));
            oi.size = sizeof(oi);
            if (sceVideodec2Flush(g_decoder, &fb, &oi) != 0) break;
            if (!(oi.valid && !oi.error && oi.picture_count)) break;
            ++g_au_ring;
            if (dst_y && oi.buffer && oi.width <= 1920 && oi.height <= 1088) {
                int w = (int)oi.width, h = (int)oi.height;
                const uint8_t *src = (const uint8_t *)oi.buffer;
                if (oi.pitch >= (uint32_t)w && w > 0 && h > 0 && oi.buffer_size >= (uint64_t)oi.pitch * (h + h / 2)) {
                    for (int y = 0; y < h; ++y)
                        memcpy(dst_y + (size_t)y * w, src + (size_t)y * oi.pitch, (size_t)w);
                    const uint8_t *uv = src + (size_t)oi.pitch * h;
                    for (int y = 0; y < h / 2; ++y)
                        memcpy(dst_uv + (size_t)y * w, uv + (size_t)y * oi.pitch, (size_t)w);
                    if (out_w) *out_w = w;
                    if (out_h) *out_h = h;
                }
            }
        }
    }
}

/* ── 播放线程 ─────────────────────────────────────────────────────────────
 * 渲染线程只有 6~8 fps（播放页），而网络读取与阻塞式音频推送都必须在 ~190 次/秒的
 * 节奏上跑；把它们留在渲染线程里，播放速度就被帧率绑死（实测慢约 8 倍）。
 * 这里把「读取 + 解码 + 音频推送」搬到独立线程，渲染线程只做「取最新帧 → 上传纹理」。
 * 用 WILIWILI_PLAYER_THREAD=0 可退回旧的单线程路径以便对照。
 *
 * 关键约束（踩过）：**生产者只能有一个**。worker 在跑时，draw() 必须直接走
 * 「取帧 + 绘制」分支，绝不能同时再执行单线程补料循环——两条线程共用一个
 * `g_pkt`/`g_afmt`/`g_adec`/`g_pcm` 会立刻互相踩坏（表现为杂音、节奏乱、野指针崩溃）。 */


/* 从音频源补 PCM（工作线程里跑；只用于 DASH 双 URL 的情况）。 */
static int player_audio_fill(int *out_reads, long *out_usec) {
    int reads = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (g_afmt) {
        int aguard = 0;
        while (g_pcm_frames < AUDIO_GRAIN * PCM_TARGET_BLOCKS && !g_audio_eof && aguard++ < 2048) {
            if (av_read_frame(g_afmt, g_pkt) < 0) {
                g_audio_eof = 1;
                break;
            }
            ++reads;
            if (g_pkt->stream_index == g_audio_index && g_adec) {
                avcodec_send_packet(g_adec, g_pkt);
                audio_decode_one_frame();
            }
            av_packet_unref(g_pkt);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (out_reads) *out_reads = reads;
    if (out_usec)
        *out_usec = (long)((t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L);
    return reads;
}

/* 从视频源取包解码，直到真的产出一帧，然后搬进"待上传"暂存（工作线程调用）。
 * bsf 输出是"每个 NAL 一包"（SEI/SPS/PPS/片各自一包），必须多读几包才能凑齐一个 AU，
 * 所以这里循环到 decode_au 产出帧为止。 */
static void player_video_step_to_stage(void) {
    if (g_video_index < 0 || g_video_eof) return;
    if (!g_audio_eof && g_last_video_pts_us > 0 && g_last_video_pts_us > g_audio_clock_us() + 300000) return;
    int guard = 0;
    while (guard++ < 256) {
        if (av_read_frame(g_fmt, g_pkt) < 0) {
            /* 流末尾攒着的那个 AU 必须送出去，否则最后一帧永远不解（同样不持锁解码）。 */
            int w = 0, h = 0;
            g_video_eof = 1;
            video_submit_flush(g_work_y, g_work_uv, &w, &h);
            if (w > 0 && h > 0) {
                size_t ysz  = (size_t)w * (size_t)h;
                size_t uvsz = ysz / 2;
                pthread_mutex_lock(&g_frame_lock);
                memcpy(g_stage_y, g_work_y, ysz);
                memcpy(g_stage_uv, g_work_uv, uvsz);
                g_stage_w   = w;
                g_stage_h   = h;
                g_frame_new = 1;
                pthread_mutex_unlock(&g_frame_lock);
            }
            return;
        }
        if (g_pkt->stream_index != g_video_index) {
            av_packet_unref(g_pkt);
            continue;
        }
        /* 解码**不持锁**：临界区只做"发布"这一次拷贝。曾经把整段解码（+两个整帧
         * memcpy）放在锁内，而渲染侧用 trylock ⇒ 渲染线程每帧都可能撞上持锁窗口，
         * 彻底取不到帧（真机表现为 audec 正常、画面一直是首帧）。 */
        int w = 0, h = 0;
        video_submit_packet(g_pkt, g_work_y, g_work_uv, &w, &h);
        av_packet_unref(g_pkt);
        if (w > 0 && h > 0) {
            size_t ysz  = (size_t)w * (size_t)h;
            size_t uvsz = ysz / 2;
            pthread_mutex_lock(&g_frame_lock);
            memcpy(g_stage_y, g_work_y, ysz);
            memcpy(g_stage_uv, g_work_uv, uvsz);
            g_stage_w   = w;
            g_stage_h   = h;
            g_frame_new = 1;
            pthread_mutex_unlock(&g_frame_lock);
            return; /* 产出一帧，交回工作循环去推音频 */
        }
    }
}

static void *player_worker(void *arg) {
    (void)arg;
    g_worker_started = 1;
    while (g_worker_run) {
        int reads = 0;
        long usec = 0;
        player_audio_fill(&reads, &usec);
        player_video_step_to_stage();
        if (!g_worker_run) break;
        audio_push_blocks(64); /* 阻塞式：整块播完才返回，天然就是音频时钟 */
    }
    g_worker_started = 0;
    return NULL;
}

/* 渲染线程调用：把搬运缓冲里的最新帧取到上传用的平面上 */
static int player_take_frame(void) {
    int got = 0;
    /* 用阻塞锁：临界区只有两次 memcpy（发布拷贝在 worker 侧），而 trylock 会让渲染
     * 线程在偶发持锁时静默丢帧。 */
    pthread_mutex_lock(&g_frame_lock);
    {
        if (g_frame_new && g_stage_w > 0) {
            size_t ysz  = (size_t)g_stage_w * (size_t)g_stage_h;
            size_t uvsz = ysz / 2;
            memcpy(g_y_plane, g_stage_y, ysz);
            memcpy(g_uv_plane, g_stage_uv, uvsz);
            g_y_width  = g_stage_w;
            g_y_height = g_stage_h;
            g_frame_new = 0;
            got = 1;
        }
    }
    pthread_mutex_unlock(&g_frame_lock);
    return got;
}

/* 单源（mp4 音频+视频同一条流）/无 worker 时的推进：读包直到真的产出一个 AU。
 * 注意 bsf 的输出是"每个 NAL 一包"，一次 av_read_frame 通常只推进一个 NAL，
 * 所以必须循环到 decode_au 被触发（否则每帧只喂了个 SEI，永远不解码）。 */
static void video_step(void) {
    if (g_video_index < 0 || g_video_eof) return;
    /* 音频源断流时**不要跟着卡死**：B 站会下发 mcdn 这类 P2P CDN，实测"能开、放几秒、然后断"
     * （真机表现为每次固定停在 clock_ms=2560）。此时改为每帧送一包的自走节奏。 */
    if (!g_audio_eof && g_last_video_pts_us > 0 && g_last_video_pts_us > g_audio_clock_us() + 300000) return;
    int guard = 0;
    while (guard++ < 256) {
        if (av_read_frame(g_fmt, g_pkt) < 0) {
            g_video_eof = 1;
            video_submit_flush(g_y_plane, g_uv_plane, &g_y_width, &g_y_height);
            return;
        }
        if (g_pkt->stream_index != g_video_index) {
            av_packet_unref(g_pkt);
            continue;
        }
        int w = 0, h = 0;
        video_submit_packet(g_pkt, g_y_plane, g_uv_plane, &w, &h);
        av_packet_unref(g_pkt);
        if (w > 0 && h > 0) {
            g_y_width  = w;
            g_y_height = h;
            return; /* 已经产出完整一帧，交给下一次调用 */
        }
    }
}

/* 帧循环：每帧推**一块**音频（Output 阻塞 ≈ 5.3 ms，自然节拍），视频包读到就立刻送硬解。 */
void wiliwili_ps5player_pause(int paused) {
    /* app 的加载状态机会周期性调 pause()，一冻结就"画面卡住"（真机实测）。
     * 暂时忽略它：先保证能连续播放；用户暂停键的支持留待与 app 状态机打通后再说。
     * 打点一次，便于确认调用来源。 */
    static int logged = 0;
    if (!logged) {
        logged = 1;
        char lb[120];
        snprintf(lb, sizeof(lb), "player: pause(%d) ignored (app state machine)", paused);
        wiliwili_boot_log(lb);
    }
    (void)paused;
}

/* 诊断用：true 时额外在 nvg UI 通道之后重画一遍，让探针模式下的画面能盖住 UI 被肉眼看到
 * （PS5 侧没有屏幕截图手段，只能靠电视确认）。真实播放走"nvg 之前"的正常路径。 */
int wiliwili_ps5player_overlay(void) { return g_overlay; }

/* 已成功起流？供 VideoView 判断是否需要改用备用视频地址。 */
int wiliwili_ps5player_ready(void) { return g_ready; }

/* ── 状态查询：供 MPVCore 把"自管播放器"的状态喂给 UI（mpv 事件循环在 PS5 上不跑） ── */
int wiliwili_ps5player_paused(void) { return g_paused; }

long wiliwili_ps5player_position_ms(void) { return (long)(g_audio_clock_us() / 1000); }

long wiliwili_ps5player_duration_ms(void) {
    if (g_fmt == NULL || g_fmt->duration == AV_NOPTS_VALUE) return 0;
    return (long)(g_fmt->duration / (AV_TIME_BASE / 1000));
}

/* VideoView::draw 每帧上报自己的矩形（nvg 坐标：左上角原点）。视频按 16:9 letterbox 放进这个区域，
 * 区域之外（OSD、评论、标题）不受影响。 */
void wiliwili_ps5player_set_rect(float x, float y, float w, float h) {
    static int logged = 0;
    if (!logged) {
        logged = 1;
        char lb[200];
        snprintf(lb, sizeof(lb), "player: rect=%.0f,%.0f %.0fx%.0f", x, y, w, h);
        wiliwili_boot_log(lb);
    }
    g_rect_x = x;
    g_rect_y = y;
    g_rect_w = w;
    g_rect_h = h;
}

void wiliwili_ps5player_set_overlay(int on) { g_overlay = on; }

/* 收尾：关音频、清状态（句柄判定按实测：>=1 才是有效句柄）。 */
void wiliwili_ps5player_close(void) {
    if (g_worker_run) {
        g_worker_run = 0;
        pthread_join(g_worker_tid, NULL);
        wiliwili_boot_log("player: worker thread joined");
    }
    if (g_audio_handle >= 1) sceAudioOutClose(g_audio_handle);
    g_audio_handle = -1;
    if (g_afmt) avformat_close_input(&g_afmt);
    if (g_fmt) avformat_close_input(&g_fmt);
    if (g_bsf) av_bsf_free(&g_bsf);
    if (g_bsf_par) avcodec_parameters_free(&g_bsf_par);
    if (g_adec) avcodec_free_context(&g_adec);
    if (g_swr) swr_free(&g_swr);
    if (g_pkt) av_packet_free(&g_pkt);
    if (g_aframe) av_frame_free(&g_aframe);
    g_afmt = NULL;
    g_fmt  = NULL;
    g_bsf  = NULL;
    g_bsf_par = NULL;
    g_adec = NULL;
    g_swr  = NULL;
    g_pkt  = NULL;
    g_aframe = NULL;
    g_ready  = 0;
    g_paused = 0;
    g_audio_eof  = 0;
    g_video_eof  = 0;
    g_pcm_frames = 0;
    g_audio_blocks = 0;
    g_y_width = 0;
    g_y_height = 0;
    g_au_slot    = 0;
    g_au_seq     = 0;
    g_dbg_au     = 0;
    g_au_ring    = 0;
    g_frame_new  = 0;
    g_stage_w    = 0;
    g_stage_h    = 0;
    g_video_index = -1;
    g_audio_index = -1;
    g_url[0] = 0;
}

/* GL 视口：和本文件其它 GL 调用一样通过 SDL_GL_GetProcAddress 取（引擎是 C，不带 GL 头）。 */
static void pl_viewport(int x, int y, int w, int h) {
    typedef void (*Fn)(int, int, int, int);
    static Fn fn;
    if (!fn) fn = (Fn)SDL_GL_GetProcAddress("glViewport");
    if (fn) fn(x, y, w, h);
}
static void pl_get_viewport(int vp[4]) {
    typedef void (*Fn)(unsigned int, int *);
    static Fn fn;
    if (!fn) fn = (Fn)SDL_GL_GetProcAddress("glGetIntegerv");
    vp[0] = 0;
    vp[1] = 0;
    vp[2] = 1920;
    vp[3] = 1080;
    if (fn) fn(0x0BA2 /* GL_VIEWPORT */, vp);
}

void wiliwili_ps5player_draw(struct NVGcontext *vg) {
    static int why_logged = 0;
    if (!g_ready) {
        if (!why_logged) {
            why_logged = 1;
            wiliwili_boot_log("player: draw skip (not ready)");
        }
        return;
    }
    /* 暂停时**仍然画**（显示最后一帧），只是不推进——之前暂停会让整个画面消失。 */
    /* 画面按区域画：16:9 适配进 g_rect_*（GL 视口原点在左下，y 要翻转）。
     * 之前画全屏且在 nvgBeginFrame 之前，会被 VideoView 的不透明背景盖住 ⇒ 全白。 */
    int old_vp[4];
    pl_get_viewport(old_vp);
    int vp[4]     = {0, 0, 0, 0};
    int have_rect = (g_rect_w > 1.0f && g_rect_h > 1.0f);
    {
        static int logged_fit = 0;
        if (have_rect && !logged_fit) {
            logged_fit = 1;
            int src_ar_w = (g_y_width > 0) ? g_y_width : 1920;
            int src_ar_h = (g_y_height > 0) ? g_y_height : 1080;
            float ar     = (float)src_ar_w / (float)src_ar_h;
            float w      = g_rect_w;
            float h      = w / ar;
            if (h > g_rect_h) {
                h = g_rect_h;
                w = h * ar;
            }
            char lb[200];
            snprintf(lb, sizeof(lb), "player: fit rect=%.0f,%.0f %.0fx%.0f -> %.0fx%.0f vp_y=%.0f", g_rect_x,
                     g_rect_y, g_rect_w, g_rect_h, w, h, (float)old_vp[3] - (g_rect_y + (g_rect_h - h) * 0.5f) - h);
            wiliwili_boot_log(lb);
        }
    }
    /* 按 VideoView 上报的矩形做 16:9 letterbox：视频只占播放器区域，
     * 区域之外的 OSD/评论/标题不受影响。曾经为了排查"全白"临时改成全屏直画，
     * 那样会盖住 OSD——帧发布链路修好后必须回到按矩形绘制。 */
    if (have_rect) {
        int win_h = old_vp[3]; /* 当前视口高度（整窗），用于 y 翻转 */
        float src_ar = (g_y_width > 0 && g_y_height > 0) ? (float)g_y_width / (float)g_y_height : 16.0f / 9.0f;
        float w      = g_rect_w;
        float h      = w / src_ar;
        if (h > g_rect_h) {
            h = g_rect_h;
            w = h * src_ar;
        }
        vp[0] = (int)(g_rect_x + (g_rect_w - w) * 0.5f);
        vp[1] = (int)(win_h - (g_rect_y + (g_rect_h - h) * 0.5f) - h);
        vp[2] = (int)w;
        vp[3] = (int)h;
        if (vp[2] <= 0 || vp[3] <= 0) have_rect = 0;
    }
    if (have_rect) pl_viewport(vp[0], vp[1], vp[2], vp[3]);
    static int draw_calls = 0;
    ++draw_calls;
    if (draw_calls <= 3) plog1("player: draw enter n=%d", (long)draw_calls);

    if (g_paused) {
        wiliwili_draw_nv12(vg, g_y_plane, g_uv_plane, g_y_width, g_y_height);
        return;
    }

    /* 生产者只有一个：worker 在跑时，渲染线程**只**取帧上传。
     * 这个分支必须排在任何"自己读包"的路径之前——它曾排在其后，而 DASH（B 站常态）
     * 走的是前面的 g_afmt 分支，于是渲染线程与 worker 同时读同一条流、抢同一个
     * g_pkt，播放全程都在互相破坏（杂音、节奏乱、addr=0x86caed 崩溃）。 */
    if (g_worker_run) {
        player_take_frame();
        if (g_y_width > 0) wiliwili_draw_nv12(vg, g_y_plane, g_uv_plane, g_y_width, g_y_height);
        if (have_rect) pl_viewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
        static int report_w = 0;
        if (++report_w >= 30) {
            report_w = 0;
            /* 全部状态放在**同一行**：UDP 日志连续两行会丢后一行（实测），
             * 分两行打印会得到"clock 到了、REND 没到"的假象。 */
            char lb[220];
            snprintf(lb, sizeof(lb), "player: clock_ms=%d blocks=%d pts_ms=%d y=%dx%d new=%d stage=%d",
                     (int)(g_audio_clock_us() / 1000), (int)g_audio_blocks, (int)(g_last_video_pts_us / 1000),
                     g_y_width, g_y_height, g_frame_new, g_stage_w);
            wiliwili_boot_log(lb);
        }
        return;
    }

    if (g_afmt) {
        /* 两条独立 URL：音频从 g_afmt 读，视频单独走 video_step（单源路径不动） */
        int aguard = 0;
        int reads = 0;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (g_pcm_frames < AUDIO_GRAIN * PCM_TARGET_BLOCKS && !g_audio_eof && aguard++ < 1024) {
            if (av_read_frame(g_afmt, g_pkt) < 0) {
                g_audio_eof = 1;
                break;
            }
            ++reads;
            if (g_pkt->stream_index == g_audio_index && g_adec) {
                avcodec_send_packet(g_adec, g_pkt);
                audio_decode_one_frame();
            }
            av_packet_unref(g_pkt);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int before_push = g_pcm_frames;
        video_step();
        audio_push_blocks(64);
        {
            static int dbg = 0;
            if (dbg < 6) {
                ++dbg;
                char lb[220];
                snprintf(lb, sizeof(lb), "player: fill reads=%d usec=%ld pcm=%d pushed=%d eof=%d", reads,
                         (long)((t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L), before_push,
                         before_push - g_pcm_frames, (int)g_audio_eof);
                wiliwili_boot_log(lb);
            }
        }
        wiliwili_draw_nv12(vg, g_y_plane, g_uv_plane, g_y_width, g_y_height);
        if (have_rect) pl_viewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
        static int report_at2 = 0;
        if (++report_at2 >= 120) {
            report_at2 = 0;
            plog3("player: clock_ms=%d blocks=%d pts_ms=%d", (long)(g_audio_clock_us() / 1000), (long)g_audio_blocks,
                  (long)(g_last_video_pts_us / 1000));
        }
        return;
    }

    if (g_paused && g_y_width > 0) {
        /* 已有画面才允许"暂停冻结"。**第一帧之前不能返回**：app 在加载中就会调 pause()，
         * 若此时返回，就永远不会解码 ⇒ 永远空白 + 一直转圈（真机实测现象）。 */
        static int pause_logged = 0;
        if (!pause_logged) {
            pause_logged = 1;
            wiliwili_boot_log("player: paused with frame, freezing");
        }
        wiliwili_draw_nv12(vg, g_y_plane, g_uv_plane, g_y_width, g_y_height);
        return;
    }
    {
        static int first_logged = 0;
        if (!first_logged) {
            first_logged = 1;
            char lb[200];
            snprintf(lb, sizeof(lb), "player: loop start paused=%d y=%dx%d rect=%.0fx%.0f", g_paused, g_y_width,
                     g_y_height, g_rect_w, g_rect_h);
            wiliwili_boot_log(lb);
        }
    }

    if (g_worker_started) {
        /* 不可能到这里：worker 分支在上面（g_worker_run），这里保留兜底只为防御
         * “启动中/收尾中”的瞬态——worker 已置位但 run 已清零时仍只取帧、不读包。 */
        player_take_frame();
        if (g_y_width > 0) wiliwili_draw_nv12(vg, g_y_plane, g_uv_plane, g_y_width, g_y_height);
        return;
    }

    int guard = 0;
    /* 单线程兜底路径（只有音频/视频都在同一条流、且 worker 未启用时才会走到）。
     * 注意不能再"扣住一个没到时间的视频包"：bsf 的输出是单个 NAL，扣半个 AU 会让
     * 聚合缓冲停在半途。节流交给 g_last_video_pts_us 判断（跳过整个 AU）。
     * 视频推进走 video_step（它自己会循环读到凑齐一个 AU）。 */
    while (g_pcm_frames < AUDIO_GRAIN * PCM_TARGET_BLOCKS && !g_audio_eof && guard++ < 4096) {
        int read_rc = av_read_frame(g_fmt, g_pkt);
        if (read_rc < 0) {
            g_audio_eof = 1;
            break;
        }
        if (g_pkt->stream_index == g_audio_index && g_adec) {
            avcodec_send_packet(g_adec, g_pkt);
            audio_decode_one_frame();
        }
        av_packet_unref(g_pkt);
    }
    video_step();
    if (draw_calls <= 3) plog1("player: refilled pcm_frames=%d", (long)g_pcm_frames);
    /* 排空缓冲：sceAudioOutOutput 会阻塞到该块播完，节奏由它定（推 1 块会把音频
     * 绑死在渲染帧率上——实测 28 s 才走 1.28 s 音频）。 */
    audio_push_blocks(64);
    if (draw_calls <= 3) plog1("player: pushed blocks=%d", (long)g_audio_blocks);

    wiliwili_draw_nv12(vg, g_y_plane, g_uv_plane, g_y_width, g_y_height);
    if (have_rect) pl_viewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
    {
        static int drew_logged = 0;
        if (!drew_logged && g_y_width > 0) {
            drew_logged = 1;
            char lb[160];
            snprintf(lb, sizeof(lb), "player: drew %dx%d (uploaded)", g_y_width, g_y_height);
            wiliwili_boot_log(lb);
        }
    }

    static int report_at = 0;
    if (++report_at >= 120) {
        report_at = 0;
        plog3("player: clock_ms=%d blocks=%d pts_ms=%d", (long)(g_audio_clock_us() / 1000), (long)g_audio_blocks,
              (long)(g_last_video_pts_us / 1000));
    }
}
