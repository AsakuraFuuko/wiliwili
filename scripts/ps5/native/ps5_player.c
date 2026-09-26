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
/* 环大小取已验证探针的值（pipeline_depth=1 时 3 槽足够；探针用 3 连解 30 帧无问题）。
 * EVO 的 8/12 是针对它自己 DecodeInputQueueDepth=4 的配置，别照抄。 */
/* 1080p 下解码器吞吐慢，3 槽不够：第 4 个 AU 会覆盖解码器仍持有的第 1 槽
 * （实测 1920x896 流在第 3~4 帧野指针崩溃；640x368 的小流却不会）。
 * 按 EVO 的经验值放到 8/12（它的 DecodeInputQueueDepth=4、帧环留 3 个富余）。 */
#define AU_SLOTS 8
#define FRAME_SLOTS 12
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
#define PCM_TARGET_BLOCKS 4
static int16_t g_pcm[AUDIO_GRAIN * 2 * PCM_TARGET_BLOCKS];
static int g_pcm_frames;
static int g_audio_handle = -1;
static unsigned long long g_audio_blocks;
static int g_audio_eof;
static int g_pending_video; /* g_pkt 里留着一个未到播放时间的视频包 */
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
    config.max_dpb_frames       = 16; /* 1080p 流参考帧可能多于 4（EVO 用 -1）；太小会导致 Decode 不出帧 */
    config.pipeline_depth       = 1; /* 保持 1：AU 环只有 3 槽，提高会互相覆盖 */
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
        const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
        plog1("player: filter=%d", filter != NULL);
        if (filter == NULL || av_bsf_alloc(filter, &g_bsf) != 0) {
            wiliwili_boot_log("player: bsf alloc failed");
            return;
        }
        wiliwili_boot_log("player: bsf alloc ok");
        avcodec_parameters_copy(g_bsf->par_in, g_fmt->streams[g_video_index]->codecpar);
        wiliwili_boot_log("player: bsf params copied");
        int bsf_rc = av_bsf_init(g_bsf);
        plog1("player: bsf init rc=%d", bsf_rc);
        if (bsf_rc != 0) return;
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

    g_pkt    = av_packet_alloc();
    g_aframe = av_frame_alloc();
    g_ready  = 1;
    wiliwili_boot_log("player: ready");
}

/* 把一个视频包送进硬解，出帧就拷进稳定缓冲（Y/UV）。 */
static void video_submit_packet(AVPacket *pkt) {
    static int slot      = 0; /* AU 环 */
    static int fslot     = 0; /* 帧环（与 AU 环独立，避免互相覆盖） */
    static uint64_t au_seq = 0;
    static int dbg_au = 0;
    if (!g_bsf) return;
    AVPacket *out = av_packet_alloc();
    int send_rc   = av_bsf_send_packet(g_bsf, pkt);
    if (send_rc != 0) {
        plog1("player: bsf send rc=%d", send_rc);
        av_packet_free(&out);
        return;
    }
    int recv_rc = av_bsf_receive_packet(g_bsf, out);
    while (recv_rc == 0) {
        if (out->data == NULL || out->size <= 0 || out->size > 0x800000) {
            /* bsf 可能给出空包/异常长度：直接跳过，别把 memcpy 送到野指针上。 */
            plog1("player: skip bsf packet size=%d", (long)out->size);
            av_packet_unref(out);
            recv_rc = av_bsf_receive_packet(g_bsf, out);
            continue;
        }
        uint8_t *au_slot = (uint8_t *)g_au_pool + (size_t)slot * 0x800000u;
        memcpy(au_slot, out->data, (size_t)out->size);
        InputData input;
        memset(&input, 0, sizeof(input));
        input.size    = sizeof(input);
        input.au      = au_slot;
        input.au_size = (uint64_t)out->size;
        /* pts 用自增序号：已验证的探针就是 0,1,2…（bsf 给的 mp4 pts 在流时间基下不是这种语义，
         * 解码器对它的校验/重排假设会不同）。A/V 同步靠音频时钟那边的 g_last_video_pts_us。 */
        input.pts     = (uint64_t)(au_seq++);
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
        int flush_rc = 0;
        if (rc == 0 && oi.valid == 0) {
            memset(&oi, 0, sizeof(oi));
            oi.size = sizeof(oi);
            flush_rc = sceVideodec2Flush(g_decoder, &frame, &oi);
        }
        if (dbg_au < 12) {
            ++dbg_au;
            const uint8_t *h = (const uint8_t *)out->data;
            char lb[220];
            snprintf(lb, sizeof(lb),
                     "player: au%d size=%d head=%02x%02x%02x%02x%02x dec=%d valid=%d flush=%d oi=%d %dx%d",
                     dbg_au, (int)out->size, h[0], h[1], h[2], h[3], h[4], rc, oi.valid, flush_rc, (int)oi.buffer_size,
                     (int)oi.width, (int)oi.height);
            wiliwili_boot_log(lb);
        }
        if (oi.valid && oi.buffer) {
            int w = (int)oi.width, h = (int)oi.height;
            if (w > 1920) w = 1920;
            if (h > 1088) h = 1088;
            g_y_width  = w;
            g_y_height = h;
            const uint8_t *src = (const uint8_t *)oi.buffer;
            for (int y = 0; y < h; ++y)
                memcpy(g_y_plane + (size_t)y * w, src + (size_t)y * oi.pitch, (size_t)w);
            const uint8_t *uv = src + (size_t)oi.pitch * h;
            for (int y = 0; y < h / 2; ++y)
                memcpy(g_uv_plane + (size_t)y * w, uv + (size_t)y * oi.pitch, (size_t)w);
            if (out->pts != AV_NOPTS_VALUE)
                g_last_video_pts_us = av_rescale_q(out->pts, g_fmt->streams[g_video_index]->time_base, AV_TIME_BASE_Q);
        }
        slot  = (slot + 1) % AU_SLOTS;
        fslot = (fslot + 1) % FRAME_SLOTS;
        av_packet_unref(out);
        recv_rc = av_bsf_receive_packet(g_bsf, out);
    }
    av_packet_free(&out);
}

/* DASH（音视频两条 URL）：每帧从视频源读**一个**视频包送硬解，落后音频超 300ms 就等。 */
static void video_step(void) {
    if (g_video_index < 0 || g_video_eof) return;
    /* 音频源断流时**不要跟着卡死**：B 站会下发 mcdn 这类 P2P CDN，实测"能开、放几秒、然后断"
     * （真机表现为每次固定停在 clock_ms=2560）。此时改为每帧送一包的自走节奏。 */
    if (!g_audio_eof && g_last_video_pts_us > 0 && g_last_video_pts_us > g_audio_clock_us() + 300000) return;
    int guard = 0;
    while (guard++ < 64) {
        if (av_read_frame(g_fmt, g_pkt) < 0) {
            g_video_eof = 1;
            return;
        }
        if (g_pkt->stream_index != g_video_index) {
            av_packet_unref(g_pkt);
            continue;
        }
        video_submit_packet(g_pkt);
        av_packet_unref(g_pkt);
        return;
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
    if (g_audio_handle >= 1) sceAudioOutClose(g_audio_handle);
    g_audio_handle = -1;
    if (g_afmt) avformat_close_input(&g_afmt);
    if (g_fmt) avformat_close_input(&g_fmt);
    if (g_bsf) av_bsf_free(&g_bsf);
    if (g_adec) avcodec_free_context(&g_adec);
    if (g_swr) swr_free(&g_swr);
    if (g_pkt) av_packet_free(&g_pkt);
    if (g_aframe) av_frame_free(&g_aframe);
    g_afmt = NULL;
    g_fmt  = NULL;
    g_bsf  = NULL;
    g_adec = NULL;
    g_swr  = NULL;
    g_pkt  = NULL;
    g_aframe = NULL;
    g_ready  = 0;
    g_paused = 0;
    g_audio_eof  = 0;
    g_video_eof  = 0;
    g_pending_video = 0;
    g_pcm_frames = 0;
    g_audio_blocks = 0;
    g_y_width = 0;
    g_y_height = 0;
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
    have_rect = 0; /* 见上：先全屏直画 */
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
    if (g_afmt) {
        /* 两条独立 URL：音频从 g_afmt 读，视频单独走 video_step（单源路径不动） */
        int aguard = 0;
        while (g_pcm_frames < AUDIO_GRAIN * PCM_TARGET_BLOCKS && !g_audio_eof && aguard++ < 256) {
            if (av_read_frame(g_afmt, g_pkt) < 0) {
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
        audio_push_blocks(64);
        wiliwili_draw_nv12(vg, g_y_plane, g_uv_plane, g_y_width, g_y_height);
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

    int guard = 0;
    /* 上一帧留下的、还没到播放时间的视频包先送出去 */
    if (g_pending_video) {
        video_submit_packet(g_pkt);
        av_packet_unref(g_pkt);
        g_pending_video = 0;
    }
    while (g_pcm_frames < AUDIO_GRAIN * PCM_TARGET_BLOCKS && !g_audio_eof && guard++ < 256) {
        int read_rc = av_read_frame(g_fmt, g_pkt);
        if (read_rc < 0) {
            g_audio_eof = 1;
            break;
        }
        if (g_pkt->stream_index == g_video_index && !g_audio_eof && g_last_video_pts_us > 0 &&
            g_last_video_pts_us > g_audio_clock_us() + 300000) {
            /* 视频跑到音频前面 300 ms 以上就停手，把包留到下一帧（否则几秒内解完整个文件） */
            g_pending_video = 1;
            break;
        }
        if (g_pkt->stream_index == g_audio_index && g_adec) {
            avcodec_send_packet(g_adec, g_pkt);
            audio_decode_one_frame();
        } else if (g_pkt->stream_index == g_video_index) {
            video_submit_packet(g_pkt); /* 不再丢弃 */
        }
        if (!g_pending_video) av_packet_unref(g_pkt);
    }
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
