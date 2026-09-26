/**

██     ██ ██ ██      ██ ██     ██ ██ ██      ██
██     ██ ██ ██      ██ ██     ██ ██ ██      ██
██  █  ██ ██ ██      ██ ██  █  ██ ██ ██      ██
██ ███ ██ ██ ██      ██ ██ ███ ██ ██ ██      ██
 ███ ███  ██ ███████ ██  ███ ███  ██ ███████ ██

 Licensed under the GPL-3.0 license
*/

#include <borealis.hpp>

extern "C" void wiliwili_videodec2_probe(void); /* 硬解探针，见 scripts/ps5/native/videodec2_probe.c */
extern "C" void wiliwili_audio_probe(void); /* 音频探针，见 scripts/ps5/native/audio_probe.c */
extern "C" void wiliwili_audio2_probe(void); /* PS5 原生音频探针，见 scripts/ps5/native/audio2_probe.c */
extern "C" void wiliwili_ps5player_open(const char *url); /* 自管播放器，见 scripts/ps5/native/ps5_player.c */
extern "C" void wiliwili_ps5player_set_overlay(int on);
#ifdef PS5
#include <ps5/klog.h>
extern "C" int sceSystemServiceLoadExec(const char*, const char**);
extern "C" void wiliwili_boot_log(const char*);
#define WILI_BOOT_LOG(message) wiliwili_boot_log(message)
#else
#define WILI_BOOT_LOG(message) (void)0
#endif

#ifdef PS5
#include <exception>
#include <string>
/* An uncaught exception would otherwise abort silently in a title sandbox. */
static void wiliwili_terminate_handler() {
    if (auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& error) {
            wiliwili_boot_log((std::string("terminate: ") + error.what()).c_str());
        } catch (...) {
            wiliwili_boot_log("terminate: unknown exception");
        }
    } else {
        wiliwili_boot_log("terminate: no active exception");
    }
    abort();
}
#endif


#include <chrono>
#include <thread>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cpr/cpr.h>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <netdb.h>
#include "utils/config_helper.hpp"
#include "utils/activity_helper.hpp"
#include "view/mpv_core.hpp"

#ifdef IOS
#include <SDL2/SDL_main.h>
#endif

#if defined(WILIWILI_OSMESA_PROBE)
extern "C" void wiliwili_osmesa_probe_now(void);
#endif


#if defined(PS5_NATIVE_APP)
extern "C" void wiliwili_video_test_start(const char *url);

/* 诊断探针：确认 ffmpeg 在原生标题沙箱里可用（demux → 解码 → swscale）。
 * 由 assets/wiliwili-options.txt 的 WILIWILI_TEST_FFMPEG=<url> 触发。 */
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}
static void wiliwili_ffmpeg_probe(const char *url) {
    char line[192];
    wiliwili_boot_log("fmpeg: enter");
    avformat_network_init();
    snprintf(line, sizeof(line), "fmpeg: version=%s", av_version_info());
    wiliwili_boot_log(line);

    AVFormatContext *fmt = nullptr;
    int rc = avformat_open_input(&fmt, url, nullptr, nullptr);
    snprintf(line, sizeof(line), "fmpeg: open rc=%d", rc);
    wiliwili_boot_log(line);
    if (rc < 0) return;

    rc = avformat_find_stream_info(fmt, nullptr);
    snprintf(line, sizeof(line), "fmpeg: streams=%u find_rc=%d", fmt->nb_streams, rc);
    wiliwili_boot_log(line);

    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    snprintf(line, sizeof(line), "fmpeg: video_stream=%d", vs);
    wiliwili_boot_log(line);
    if (vs < 0) { avformat_close_input(&fmt); return; }

    AVStream *st = fmt->streams[vs];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    snprintf(line, sizeof(line), "fmpeg: codec=%s %dx%d", dec ? dec->name : "(none)", st->codecpar->width,
             st->codecpar->height);
    wiliwili_boot_log(line);
    if (!dec) { avformat_close_input(&fmt); return; }

    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    rc = avcodec_open2(ctx, dec, nullptr);
    snprintf(line, sizeof(line), "fmpeg: decoder_open rc=%d", rc);
    wiliwili_boot_log(line);
    if (rc < 0) { avcodec_free_context(&ctx); avformat_close_input(&fmt); return; }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm  = av_frame_alloc();
    int got = 0;
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vs && avcodec_send_packet(ctx, pkt) == 0) {
            if (avcodec_receive_frame(ctx, frm) == 0) {
                got = 1;
                snprintf(line, sizeof(line), "fmpeg: frame %dx%d pix_fmt=%d", frm->width, frm->height, frm->format);
                wiliwili_boot_log(line);
                SwsContext *sws = sws_getContext(frm->width, frm->height, (AVPixelFormat)frm->format, frm->width,
                                                 frm->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (sws) {
                    uint8_t *dst[4]      = {(uint8_t *)malloc((size_t)frm->width * 4 * frm->height), nullptr, nullptr,
                                            nullptr};
                    int dst_stride[4]    = {frm->width * 4, 0, 0, 0};
                    int lines            = sws_scale(sws, frm->data, frm->linesize, 0, frm->height, dst, dst_stride);
                    snprintf(line, sizeof(line), "fmpeg: sws_scale lines=%d px=%02x%02x%02x", lines, dst[0][0],
                             dst[0][1], dst[0][2]);
                    wiliwili_boot_log(line);

                    /* 把整帧发回开发机，用于肉眼核对解码是否正确（颜色/几何）。 */
                    {
                        int fd = socket(AF_INET, SOCK_STREAM, 0);
                        if (fd >= 0) {
                            struct sockaddr_in addr = {};
                            addr.sin_family         = AF_INET;
                            addr.sin_port           = htons(9998);
                            inet_pton(AF_INET, "192.168.100.7", &addr.sin_addr);
                            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                                char header[64];
                                int header_len = snprintf(header, sizeof(header), "%d %d %d\n", frm->width,
                                                          frm->height, frm->width * 4);
                                (void)send(fd, header, header_len, 0);
                                size_t total = (size_t)frm->width * 4 * frm->height;
                                size_t sent  = 0;
                                while (sent < total) {
                                    ssize_t n = send(fd, dst[0] + sent, total - sent, 0);
                                    if (n <= 0) break;
                                    sent += (size_t)n;
                                }
                                snprintf(line, sizeof(line), "fmpeg: frame sent %zu/%zu bytes", sent, total);
                                wiliwili_boot_log(line);
                            } else {
                                wiliwili_boot_log("fmpeg: frame send connect failed");
                            }
                            close(fd);
                        }
                    }
                    free(dst[0]);
                    sws_freeContext(sws);
                }
            }
        }
        av_packet_unref(pkt);
    }
    snprintf(line, sizeof(line), "fmpeg: probe done frame=%d", got);
    wiliwili_boot_log(line);
    av_packet_free(&pkt);
    av_frame_free(&frm);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
}
#endif

int main(int argc, char* argv[]) {
#ifdef PS5
    klog_puts("wiliwili: main entered");
#endif
#if defined(WILIWILI_OSMESA_PROBE)
    /* Constructors have run by now, so Mesa's own initialisation is complete. */
    wiliwili_osmesa_probe_now();
#endif
#ifdef PS5
    std::set_terminate(wiliwili_terminate_handler);
#endif

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-d") == 0) {
            brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        } else if (std::strcmp(argv[i], "-v") == 0) {
            brls::Application::enableDebuggingView(true);
        } else if (std::strcmp(argv[i], "-t") == 0) {
            MPVCore::TERMINAL = true;
        } else if (std::strcmp(argv[i], "-o") == 0) {
            const char* path = (i + 1 < argc) ? argv[++i] : "wiliwili.log";
            brls::Logger::setLogOutput(std::fopen(path, "w+"));
        }
    }

    // Load cookies and settings
    ProgramConfig::instance().init();

    // Init the app and i18n
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init application");
        return EXIT_FAILURE;
    }

    // Return directly to the desktop when closing the application (only for NX)
    brls::Application::getPlatform()->exitToHomeMode(true);

    brls::Application::createWindow("wiliwili");
    brls::Logger::info("createWindow done");

    // Register custom view\theme\style
    Register::initCustomView();
    Register::initCustomTheme();
    Register::initCustomStyle();

    brls::Application::getPlatform()->disableScreenDimming(false);

    if (brls::Application::getPlatform()->isApplicationMode()) {
        /* Debugging entry point: the player is otherwise only reachable with a
         * controller, and a failure inside mpv has to be reproducible. Set
         * WILIWILI_TEST_BV=<bvid> in assets/wiliwili-options.txt to boot
         * straight into that video; the normal path stays untouched. */
        const char* testVideo = getenv("WILIWILI_TEST_BV");
        const char* testDelay = getenv("WILIWILI_TEST_BV_DELAY");
        if (testVideo != nullptr && testVideo[0] != '\0') {
            if (testDelay != nullptr && atoi(testDelay) > 0) {
                /* 诊断用：等主界面正常起来后再进播放器，复现"启动瞬间直进"之外的路径 */
                int seconds = atoi(testDelay);
                std::string bv(testVideo);
                WILI_BOOT_LOG("main: test video scheduled");
                std::thread([bv, seconds]() {
                    std::this_thread::sleep_for(std::chrono::seconds(seconds));
                    brls::sync([bv]() { Intent::openBV(bv); });
                }).detach();
            } else {
                WILI_BOOT_LOG("main: opening test video");
                Intent::openBV(testVideo);
            }
        } else {
            const char* ffmpegUrl = getenv("WILIWILI_TEST_FFMPEG");
            if (ffmpegUrl != nullptr && ffmpegUrl[0] != '\0') {
#if defined(PS5_NATIVE_APP)
                /* 一次性可用性探针（历史）＋ 自管视频测试（解码→上屏） */
                wiliwili_ffmpeg_probe(ffmpegUrl);
                wiliwili_video_test_start(ffmpegUrl);
#endif
            }
#if defined(PS5_NATIVE_APP)
            {
                const char *vdec = getenv("WILIWILI_TEST_VDEC");
                if (vdec != nullptr && vdec[0] != '\0') wiliwili_videodec2_probe();
                const char *aud = getenv("WILIWILI_TEST_AUDIO");
                if (aud != nullptr && aud[0] != '\0') wiliwili_audio_probe();
                const char *aud2 = getenv("WILIWILI_TEST_AUDIO2");
                if (aud2 != nullptr && aud2[0] != '\0') wiliwili_audio2_probe();
                const char *playerUrl = getenv("WILIWILI_TEST_PLAYER");
                if (playerUrl != nullptr && playerUrl[0] != '\0') {
                    wiliwili_ps5player_open(playerUrl);
                    wiliwili_ps5player_set_overlay(1); /* 探针模式：画在 UI 之上，便于肉眼确认 */
                }
            }
#endif
            /* 音频后端探针：自管播放器要自己出声音，先确认 SDL2 音频可用。 */
            {
                char aline[128];
                int audio_rc = SDL_InitSubSystem(SDL_INIT_AUDIO);
                const char *driver = SDL_GetCurrentAudioDriver();
                snprintf(aline, sizeof(aline), "audio: SDL_InitSubSystem rc=%d driver=%s", audio_rc,
                         driver ? driver : "(none)");
                wiliwili_boot_log(aline);
                if (audio_rc == 0) {
                    SDL_AudioSpec want = {}, have = {};
                    want.freq     = 48000;
                    want.format   = AUDIO_S16SYS;
                    want.channels = 2;
                    want.samples  = 1024;
                    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
                    snprintf(aline, sizeof(aline), "audio: open device=%u freq=%d ch=%d", (unsigned)dev, have.freq,
                             have.channels);
                    wiliwili_boot_log(aline);
                    if (dev) SDL_CloseAudioDevice(dev);
                }
            }
            Intent::openMain();
        }
        // Uncomment these lines to debug activities
        //        Intent::openBV("BV1Da411Y7U4");  // 弹幕防遮挡 (横屏)
        //        Intent::openBV("BV1iN4y1m7J3");  // 弹幕防遮挡 (竖屏)
        //        Intent::openBV("BV1kT4y1s7od");  // 高级弹幕 测试0
        //        Intent::openBV("BV1eN4y147bC");  // 高级弹幕 测试1
        //        Intent::openBV("BV16x411D7NK");  // 高级弹幕 测试2
        //        Intent::openBV("BV1uW411e7gt");  // bas 弹幕 (Bilibili Animation Script)
        //        Intent::openBV("BV1zb4y1j7vz");  // flv 模式报错：HTTP 424
        //        Intent::openBV("BV1jL41167ZG");  // 充电视频
        //        Intent::openBV("BV1dx411c7Av");  // flv拼接视频
        //        Intent::openBV("BV15z4y1Z734");  // 4K HDR 视频
        //        Intent::openBV("BV1qM4y1w716");  // 8K
        //        Intent::openBV("BV1PN4y1G7u2");  // up主视频自动跳转番剧
        //        Intent::openBV("BV1sK411s7zq");  // 多P视频测试
        //        Intent::openBV("BV1Cg411j76F");  // 多字幕测试
        //        Intent::openBV("BV1A44y1u7PF");  // 测试FFMPEG在switch上的bug（加载时间过长）
        //        Intent::openBV("BV1eD4y1b7Jv");  // 测试 MPV 在switch上的bug（长时间播放崩溃）
        //        Intent::openBV("BV1U3411c7Qx");  // 测试长标题
        //        Intent::openBV("BV1fG411W7Px");  // 测试弹幕
        //        Intent::openSeasonByEpId(323434);// 测试电影
        //        Intent::openLive(1942240);       // 测试直播
        //        Intent::openSearch("harry");     // 测试搜索影片
        //        Intent::openTVSearch();          // 测试TV搜索模式
        //        Intent::openHint();              // 应用开启教程页面
        //        Intent::openCollection("2511565362"); // 测试打开收藏夹
        //        Intent::openPgcFilter("/page/home/pgc/more?type=2&index_type=2&area=2&order=2&season_status=-1&season_status=3,6"); // 影片分类索引
        //        Intent::openSetting();  //  设置页面
    } else {
        Intent::openHint();
    }

    GA("open_app", {{"version", APPVersion::instance().getVersionStr()},
                    {"language", brls::Application::getLocale()},
                    {"window", fmt::format("{}x{}", brls::Application::windowWidth, brls::Application::windowHeight)}})
    APPVersion::instance().checkUpdate();

    // Run the app
    // brls::Application::setLimitedFPS(60);
    while (brls::Application::mainLoop()) {
    }

    brls::Logger::info("mainLoop done");

    // Cleanup curl and Check whether restart is required
    ProgramConfig::instance().exit(argv);

    // Return control to the Homebrew Launcher instead of leaving VideoOut owned by this process.
#ifdef PS5
    sceSystemServiceLoadExec("exit", nullptr);
#endif
    return EXIT_SUCCESS;
}

#ifdef __WINRT__
#include <borealis/core/main.hpp>
#endif
