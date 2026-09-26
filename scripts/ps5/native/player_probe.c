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
#define AU_SLOTS 3
#define FRAME_SLOTS 3
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
    config.max_dpb_frames       = 4;
    config.pipeline_depth       = 1;
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


void wiliwili_player_probe(const char *url) {
    char line[192];
    plog1("player: enter %d", url != NULL);

    if (decoder_init(640, 368) != 0) {
        wiliwili_boot_log("player: decoder init failed");
        return;
    }

    /* 音频：照抄可用配方（0xFF, 0, 0, 256, 48000, 1）；句柄 < 1 才算失败；Output 只认负数失败 */
    sceAudioOutInit();
    g_audio_handle = sceAudioOutOpen(0xFF, 0, 0, AUDIO_GRAIN, AUDIO_FREQ, 1);
    plog1("player: audio handle=%d", g_audio_handle);
    if (g_audio_handle < 1) {
        wiliwili_boot_log("player: audio open failed");
        return;
    }

    avformat_network_init();
    if (avformat_open_input(&g_fmt, url, NULL, NULL) < 0) {
        wiliwili_boot_log("player: open input failed");
        return;
    }
    if (avformat_find_stream_info(g_fmt, NULL) < 0) {
        wiliwili_boot_log("player: find stream info failed");
        return;
    }
    g_video_index = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    g_audio_index = av_find_best_stream(g_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    plog2("player: streams v=%d a=%d", g_video_index, g_audio_index);

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
        const AVCodec *adec = avcodec_find_decoder(g_fmt->streams[g_audio_index]->codecpar->codec_id);
        if (adec) {
            g_adec = avcodec_alloc_context3(adec);
            avcodec_parameters_to_context(g_adec, g_fmt->streams[g_audio_index]->codecpar);
            wiliwili_boot_log("player: audio codec found");
            if (avcodec_open2(g_adec, adec, NULL) == 0) {
                wiliwili_boot_log("player: audio decoder open");
                AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&g_swr, &out_layout, AV_SAMPLE_FMT_S16, AUDIO_FREQ,
                                        &g_adec->ch_layout, g_adec->sample_fmt, g_adec->sample_rate, 0, NULL) == 0) {
                    swr_init(g_swr);
                }
                plog2("player: audio codec id=%d sr=%d", (long)adec->id, g_adec->sample_rate);
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
        if (rc == 0 && oi.valid == 0) {
            memset(&oi, 0, sizeof(oi));
            oi.size = sizeof(oi);
            sceVideodec2Flush(g_decoder, &frame, &oi);
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

/* 帧循环：每帧推**一块**音频（Output 阻塞 ≈ 5.3 ms，自然节拍），视频包读到就立刻送硬解。 */
void wiliwili_player_draw(struct NVGcontext *vg) {
    if (!g_ready) return;
    static int draw_calls = 0;
    ++draw_calls;
    if (draw_calls <= 3) plog1("player: draw enter n=%d", (long)draw_calls);

    int guard = 0;
    while (g_pcm_frames < AUDIO_GRAIN * PCM_TARGET_BLOCKS && !g_audio_eof && guard++ < 256) {
        int read_rc = av_read_frame(g_fmt, g_pkt);
        if (read_rc < 0) {
            g_audio_eof = 1;
            break;
        }
        if (g_pkt->stream_index == g_audio_index && g_adec) {
            avcodec_send_packet(g_adec, g_pkt);
            audio_decode_one_frame();
        } else if (g_pkt->stream_index == g_video_index) {
            video_submit_packet(g_pkt); /* 不再丢弃 */
        }
        av_packet_unref(g_pkt);
    }
    if (draw_calls <= 3) plog1("player: refilled pcm_frames=%d", (long)g_pcm_frames);
    audio_push_blocks(1);
    if (draw_calls <= 3) plog1("player: pushed blocks=%d", (long)g_audio_blocks);

    wiliwili_draw_nv12(vg, g_y_plane, g_uv_plane, g_y_width, g_y_height);
    if (draw_calls <= 3) plog1("player: drew %d", (long)g_y_width);

    static int report_at = 0;
    if (++report_at >= 120) {
        report_at = 0;
        plog3("player: clock_ms=%d blocks=%d pts_ms=%d", (long)(g_audio_clock_us() / 1000), (long)g_audio_blocks,
              (long)(g_last_video_pts_us / 1000));
    }
}
