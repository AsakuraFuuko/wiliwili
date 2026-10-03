/*
 * 自管视频探针（原生标题专用）：ffmpeg 解封装/解码 → swscale 成 RGBA → nanovg 纹理 → 全屏绘制。
 *
 * 目的：mpv 在 app slot 里初始化即崩（见 run-continuation/ps5-port-status.md 与 notes/05），
 * 而 ffmpeg 实测可用（notes/02），所以 P1 的形态是"完全绕开 mpv、视频自管"。
 * 这个文件是该路线第一里程碑：把"解码 → 上屏"跑通，并给出真实解码耗时。
 *
 * 由 assets/wiliwili-options.txt 的 WILIWILI_TEST_FFMPEG=<url> 触发；不影响其它平台。
 */
#include <borealis.hpp>
#include <nanovg.h>

#if defined(PS5_NATIVE_APP)
#include <SDL2/SDL.h>
#if !defined(BOREALIS_USE_AGC)
/* 只为拿到 nvglCreateImageFromHandleGL3 的声明；GL 类型的最小前置定义。 */
typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
#define NANOVG_GL3 1
#include <nanovg_gl.h>
#endif
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" void wiliwili_boot_log(const char *message);

namespace {

struct VideoTest {
    AVFormatContext *fmt    = nullptr;
    AVCodecContext *decoder = nullptr;
    SwsContext *sws         = nullptr;
    AVPacket *packet        = nullptr;
    AVFrame *frame          = nullptr;
    uint8_t *planes[4]      = {nullptr, nullptr, nullptr, nullptr};
    int strides[4]          = {0, 0, 0, 0};
    int width               = 0;
    int height              = 0;
    int video_stream        = -1;
    int texture             = 0;
    unsigned int gl_texture = 0;   /* 原生 GL 纹理，与 nanovg 纹理对照上传耗时 */
    int upload_mode         = 0;   /* 0 = nanovg nvgUpdateImage，1 = glTexSubImage2D */
    bool eof                = false;
    double frame_seconds    = 1.0 / 30.0; /* 当前帧的呈现时间（视频时间轴） */
    std::chrono::steady_clock::time_point playback_start{};
    unsigned long long decoded = 0;
    unsigned long long draws     = 0;   /* 本窗口内探针绘制次数 = 应用帧率 */
    double decode_ms           = 0.0;
    double upload_ms           = 0.0;
    std::chrono::steady_clock::time_point next_report{};
};

VideoTest g_video;

void log_line(const std::string &text) { wiliwili_boot_log(text.c_str()); }

/* 解出下一帧并转成 RGBA；成功返回 true。 */
bool decode_next_frame() {
    if (g_video.eof) return false;
    while (true) {
        if (av_read_frame(g_video.fmt, g_video.packet) < 0) {
            g_video.eof = true;
            return false;
        }
        if (g_video.packet->stream_index != g_video.video_stream) {
            av_packet_unref(g_video.packet);
            continue;
        }

        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        int send_rc = avcodec_send_packet(g_video.decoder, g_video.packet);
        av_packet_unref(g_video.packet);
        if (send_rc < 0) continue;
        if (avcodec_receive_frame(g_video.decoder, g_video.frame) != 0) continue;

        sws_scale(g_video.sws, g_video.frame->data, g_video.frame->linesize, 0, g_video.height, g_video.planes,
                  g_video.strides);
        g_video.decode_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++g_video.decoded;

        int64_t pts         = g_video.frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = g_video.frame->pts;
        AVStream *stream    = g_video.fmt->streams[g_video.video_stream];
        g_video.frame_seconds = (pts != AV_NOPTS_VALUE) ? pts * av_q2d(stream->time_base) : g_video.frame_seconds + 1.0 / 30.0;
        return true;
    }
}

}  // namespace

extern "C" void wiliwili_video_test_start(const char *url) {
    log_line("vt: start " + std::string(url));
    if (const char *mode = getenv("WILIWILI_VIDEO_UPLOAD")) g_video.upload_mode = atoi(mode);

    avformat_network_init();
    if (avformat_open_input(&g_video.fmt, url, nullptr, nullptr) < 0) {
        log_line("vt: open failed");
        return;
    }
    if (avformat_find_stream_info(g_video.fmt, nullptr) < 0) {
        log_line("vt: find_stream_info failed");
        return;
    }

    g_video.video_stream = av_find_best_stream(g_video.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (g_video.video_stream < 0) {
        log_line("vt: no video stream");
        return;
    }

    AVStream *stream     = g_video.fmt->streams[g_video.video_stream];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        log_line("vt: no decoder");
        return;
    }

    g_video.decoder = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(g_video.decoder, stream->codecpar);
    if (avcodec_open2(g_video.decoder, codec, nullptr) < 0) {
        log_line("vt: decoder open failed");
        return;
    }

    g_video.width  = g_video.decoder->width;
    g_video.height = g_video.decoder->height;
    g_video.sws    = sws_getContext(g_video.width, g_video.height, g_video.decoder->pix_fmt, g_video.width,
                                    g_video.height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (g_video.sws == nullptr) {
        log_line("vt: sws failed");
        return;
    }

    if (av_image_alloc(g_video.planes, g_video.strides, g_video.width, g_video.height, AV_PIX_FMT_RGBA, 1) < 0) {
        log_line("vt: image alloc failed");
        return;
    }

    g_video.packet = av_packet_alloc();
    g_video.frame  = av_frame_alloc();
    g_video.frame_seconds = 0.0;
    g_video.playback_start = std::chrono::steady_clock::now();
    g_video.next_report    = g_video.playback_start + std::chrono::seconds(3);
    /* 先解出第一帧，之后的推进由 pts 控制。 */
    decode_next_frame();

    char line[160];
    snprintf(line, sizeof(line), "vt: ready codec=%s %dx%d frame0=%.3fs", codec->name, g_video.width, g_video.height,
             g_video.frame_seconds);
    log_line(line);
}

/* 由 borealis 的帧循环在 nvgEndFrame 之前调用（见 application.cpp 的钩子）。 */
extern "C" void wiliwili_video_test_draw(NVGcontext *vg) {
    if (g_video.fmt == nullptr || g_video.planes[0] == nullptr) return;

    /* 按呈现时间推进：视频时间轴落后于已播时长时才解下一帧（等价于按帧率播放）。 */
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_video.playback_start).count();
    if (elapsed >= g_video.frame_seconds && !g_video.eof) decode_next_frame();

    /* 对照实验：同一帧分别用 nanovg 与原生 GL 上传，比较耗时。 */
#if !defined(BOREALIS_USE_AGC)
    if (g_video.upload_mode == 1) {
        typedef void (*TexImageFn)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
        typedef void (*TexSubImageFn)(unsigned int, int, int, int, int, unsigned int, unsigned int, const void *);
        typedef void (*GenTexturesFn)(int, unsigned int *);
        typedef void (*BindTextureFn)(unsigned int, unsigned int);
        typedef void (*TexParameteriFn)(unsigned int, unsigned int, int);
        static TexImageFn        p_tex_image = (TexImageFn)SDL_GL_GetProcAddress("glTexImage2D");
        static TexSubImageFn     p_tex_sub   = (TexSubImageFn)SDL_GL_GetProcAddress("glTexSubImage2D");
        static GenTexturesFn     p_gen_tex   = (GenTexturesFn)SDL_GL_GetProcAddress("glGenTextures");
        static BindTextureFn     p_bind_tex  = (BindTextureFn)SDL_GL_GetProcAddress("glBindTexture");
        static TexParameteriFn   p_tex_param = (TexParameteriFn)SDL_GL_GetProcAddress("glTexParameteri");
        if (p_tex_image == nullptr || p_gen_tex == nullptr) return;

        if (g_video.gl_texture == 0) {
            p_gen_tex(1, &g_video.gl_texture);
            p_bind_tex(0x0DE1 /*GL_TEXTURE_2D*/, g_video.gl_texture);
            p_tex_param(0x0DE1, 0x2801 /*MIN_FILTER*/, 0x2600 /*NEAREST*/);
            p_tex_param(0x0DE1, 0x2800 /*MAG_FILTER*/, 0x2600);
            p_tex_param(0x0DE1, 0x2802 /*WRAP_S*/, 0x812F /*CLAMP_TO_EDGE*/);
            p_tex_param(0x0DE1, 0x2803 /*WRAP_T*/, 0x812F);
            p_tex_image(0x0DE1, 0, 0x8058 /*RGBA8*/, g_video.width, g_video.height, 0, 0x1908 /*RGBA*/,
                        0x1401 /*UNSIGNED_BYTE*/, g_video.planes[0]);
            /* 交给 nanovg 绘制这块已有纹理。 */
            g_video.texture = nvglCreateImageFromHandleGL3(vg, g_video.gl_texture, g_video.width, g_video.height, 0);
        } else {
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            p_bind_tex(0x0DE1, g_video.gl_texture);
            p_tex_sub(0x0DE1, 0, 0, g_video.width, g_video.height, 0x1908, 0x1401, g_video.planes[0]);
            g_video.upload_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        if (g_video.texture == 0) return;
    } else
#endif
    {
        if (g_video.texture == 0) {
        g_video.texture = nvgCreateImageRGBA(vg, g_video.width, g_video.height,
                                                        NVG_IMAGE_STREAMING | NVG_IMAGE_COPY_SWAP | NVG_IMAGE_NEAREST,
                                                        g_video.planes[0]);
        if (g_video.texture == 0) return;
        } else {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        nvgUpdateImage(vg, g_video.texture, g_video.planes[0]);
        g_video.upload_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
    }

    float screen_w = (float)brls::Application::windowWidth;
    float screen_h = (float)brls::Application::windowHeight;
    NVGpaint paint = nvgImagePattern(vg, 0, 0, screen_w, screen_h, 0, g_video.texture, 1.0f);
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, screen_w, screen_h);
    nvgFillPaint(vg, paint);
    nvgFill(vg);

    ++g_video.draws;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (now >= g_video.next_report) {
        g_video.next_report = now + std::chrono::seconds(3);
        char line[220];
        snprintf(line, sizeof(line), "vt: fps=%.1f decoded=%llu avg_decode=%.2fms avg_upload=%.2fms eof=%d",
                 g_video.draws / 3.0, g_video.decoded,
                 g_video.decoded ? g_video.decode_ms / (double)g_video.decoded : 0.0,
                 g_video.decoded ? g_video.upload_ms / (double)g_video.decoded : 0.0, g_video.eof ? 1 : 0);
        log_line(line);
        g_video.draws = 0;
    }
}

/* 供其它探针（如 C 写的硬解探针）复用：把一块 GL 纹理当图片全屏绘制。
 * 它们不便包含 nanovg 头，就统一走这个 C++ 侧的小助手。 */
extern "C" void wiliwili_draw_gl_texture(NVGcontext *vg, unsigned int texture, int width, int height) {
    if (vg == nullptr || texture == 0) return;
#if defined(BOREALIS_USE_AGC)
    (void)vg;
    (void)texture;
    (void)width;
    (void)height;
    return;
#else
    float screen_w = (float)brls::Application::windowWidth;
    float screen_h = (float)brls::Application::windowHeight;
    int image = nvglCreateImageFromHandleGL3(vg, texture, width, height, 0);
    if (image == 0) return;
    NVGpaint paint = nvgImagePattern(vg, 0, 0, screen_w, screen_h, 0, image, 1.0f);
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, screen_w, screen_h);
    nvgFillPaint(vg, paint);
    nvgFill(vg);
#endif
}
