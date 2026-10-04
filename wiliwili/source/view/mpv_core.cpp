#include <fstream>
#include <chrono>
#include <cstdio>

extern "C" void wiliwili_boot_log(const char *);
//

// Created by fang on 2022/8/12.
//

#include <cstdlib>
#include <clocale>
#include <cmath>
#include <pystring.h>
#include <borealis/core/thread.hpp>
#include <borealis/core/application.hpp>

#include "utils/config_helper.hpp"
#include "utils/number_helper.hpp"
#include "utils/crash_helper.hpp"
#include "view/mpv_core.hpp"

#ifdef MPV_BUNDLE_DLL
mpvSetOptionStringFunc mpvSetOptionString;
mpvObservePropertyFunc mpvObserveProperty;
mpvCreateFunc mpvCreate;
mpvInitializeFunc mpvInitialize;
mpvTerminateDestroyFunc mpvTerminateDestroy;
mpvSetWakeupCallbackFunc mpvSetWakeupCallback;
mpvCommandStringFunc mpvCommandString;
mpvErrorStringFunc mpvErrorString;
mpvWaitEventFunc mpvWaitEvent;
mpvGetPropertyFunc mpvGetProperty;
mpvCommandAsyncFunc mpvCommandAsync;
mpvGetPropertyStringFunc mpvGetPropertyString;
mpvFreeNodeContentsFunc mpvFreeNodeContents;
mpvSetOptionFunc mpvSetOption;
mpvFreeFunc mpvFree;
mpvRenderContextCreateFunc mpvRenderContextCreate;
mpvRenderContextSetUpdateCallbackFunc mpvRenderContextSetUpdateCallback;
mpvRenderContextRenderFunc mpvRenderContextRender;
mpvRenderContextReportSwapFunc mpvRenderContextReportSwap;
mpvRenderContextUpdateFunc mpvRenderContextUpdate;
mpvRenderContextFreeFunc mpvRenderContextFree;
mpvClientApiVersionFunc mpvClientApiVersion;
#endif

#ifdef MPV_USE_FB
#ifdef PS4
#include "utils/ps4_mpv_shaders.hpp"
#endif
#if defined(USE_GLES2)
#define SHADER_VERSION "#version 100\n"
#define SHADER_PRECISION "precision mediump float;\n"
#elif defined(USE_GLES3)
#define SHADER_VERSION "#version 300 es\n"
#define SHADER_PRECISION "precision mediump float;\n"
#else
#define SHADER_VERSION "#version 150 core\n"
#define SHADER_PRECISION ""
#endif

const char *vertexShaderSource = SHADER_VERSION
#ifdef USE_GLES2
    "attribute vec3 aPos;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 TexCoord;\n"
#else
    "in vec3 aPos;\n"
    "in vec2 aTexCoord;\n"
    "out vec2 TexCoord;\n"
#endif
    "void main()\n"
    "{\n"
    "   gl_Position = vec4(aPos, 1.0);\n"
    "   TexCoord = aTexCoord;\n"
    "}\0";
const char *fragmentShaderSource = SHADER_VERSION SHADER_PRECISION
#ifdef USE_GLES2
    "varying vec2 TexCoord;\n"
#else
    "in vec2 TexCoord;\n"
    "out vec4 FragColor;\n"
#endif
    "uniform sampler2D ourTexture;\n"
    "uniform float Alpha;\n"
    "void main()\n"
    "{\n"
#ifdef USE_GLES2
    "   gl_FragColor = texture2D(ourTexture, TexCoord);\n"
    "   gl_FragColor.a = Alpha;\n"
#else
    "   FragColor = texture(ourTexture, TexCoord);\n"
    "   FragColor.a = Alpha;\n"
#endif
    "}\n\0";

#define CONCATENATE3(s1, s2, s3) s1##s2##s3
#define CONCATENATE2(s1, s2) s1##s2
#define checkGLError(type, id)                                                 \
    {                                                                          \
        char *info = nullptr;                                                  \
        int length = 0;                                                        \
        CONCATENATE3(glGet, type, iv)(id, GL_COMPILE_STATUS, &length);         \
        if (length > 0) {                                                      \
            info = (char *)malloc(length);                                     \
            if (info) {                                                        \
                CONCATENATE3(glGet, type, InfoLog)(id, length, &length, info); \
            }                                                                  \
        }                                                                      \
        if (info) {                                                            \
            brls::Logger::error("Failed to load the shader: {}", info);        \
            free(info);                                                        \
        } else {                                                               \
            brls::Logger::error("Failed to load the shader");                  \
        }                                                                      \
        CONCATENATE2(glDelete, type)(id);                                      \
    }

static GLuint createShader(GLint type, const char *source) {
    GLuint shader = glCreateShader(type);
#ifdef PS4
    glShaderBinary(1, &shader, 2, type == GL_VERTEX_SHADER ? PS4_MPV_SHADER_VERT : PS4_MPV_SHADER_FRAG,
                   type == GL_VERTEX_SHADER ? PS4_MPV_SHADER_VERT_LENGTH : PS4_MPV_SHADER_FRAG_LENGTH);
#else
    int success;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    // check for shader compile errors
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        checkGLError(Shader, shader);
        return 0;
    }
#endif
    return shader;
}

static GLuint linkProgram(GLuint s1, GLuint s2) {
    if (s1 == 0 || s2 == 0) return 0;
    GLuint program = glCreateProgram();
    int success;
    glAttachShader(program, s1);
    glAttachShader(program, s2);
    glLinkProgram(program);
    // check for linking errors
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        checkGLError(Program, program);
        return 0;
    }
    return program;
}
#endif

#ifdef BOREALIS_USE_DEKO3D
#include <borealis/platforms/switch/switch_video.hpp>
#elif defined(BOREALIS_USE_D3D11)
#include <borealis/platforms/driver/d3d11.hpp>
extern std::unique_ptr<brls::D3D11Context> D3D11_CONTEXT;
#elif defined(BOREALIS_USE_GXM)
#include <borealis/platforms/psv/psv_video.hpp>
#include <borealis/extern/nanovg/nanovg_gxm.h>
#elif defined(USE_GL2)
#undef glBindFramebuffer
#define glBindFramebuffer(a, b) void()
#endif

#if defined(__linux__) && defined(__GLFW__) && defined(USE_EGL)
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#include <GLFW/glfw3native.h>
#endif

static const char *safeMpvText(const char *text) { return text != nullptr ? text : ""; }
#if defined(PS5_NATIVE_APP)
static std::string mpvPropertyText(mpv_handle *handle, const char *name) {
    char *value = mpvGetPropertyString(handle, name);
    if (value == nullptr) return {};
    std::string result(value);
    mpvFree(value);
    return result;
}
#endif
#if defined(PS5_NATIVE_APP)
/* The SW renderer's normal swscale flags add full chroma interpolation and
 * accurate rounding on top of the default scaler. `sws-fast` removes only
 * those expensive flags; it leaves the default scaler selection and output
 * surface unchanged. This is the smallest measured quality/performance tradeoff
 * between the default path and the all-in `sw-fast` profile. */
static void configureNativeMpvSw(mpv_handle *mpv) {
    mpvSetOptionString(mpv, "sws-fast", "yes");
}
#endif


static inline void check_error(int status) {
    if (status < 0) {
        brls::Logger::error("MPV ERROR ====> {}", safeMpvText(mpvErrorString(status)));
    }
}

#if defined(BOREALIS_USE_OPENGL) && !defined(MPV_SW_RENDER)
#if defined(PS5_NATIVE_APP) || defined(PS5)
/* 诊断：mpv 通过这个回调取 GL 入口；本机 GL bridge（ps5-opengl）不一定导出全部
 * 符号，缺失时回调返回 NULL，mpv 一调用就会跳到地址 0（SIGSEGV, rip≈0）。 */
static void *wiliwili_logged_gl_proc(void *handle, const char *name) {
    void *pointer = (void *)SDL_GL_GetProcAddress(name);
    if (pointer == nullptr) {
        std::string line = std::string("gl: MISSING ") + name;
        wiliwili_boot_log(line.c_str());
    }
    return pointer;
}
#endif

static void *get_proc_address(void *unused, const char *name) {
#ifdef __SDL2__
    SDL_GL_GetCurrentContext();
#if defined(PS5_NATIVE_APP) || defined(PS5)
    return wiliwili_logged_gl_proc(unused, name);
#else
    return (void *)SDL_GL_GetProcAddress(name);
#endif
#else
    glfwGetCurrentContext();
    return (void *)glfwGetProcAddress(name);
#endif
}
#endif

static inline float aspectConverter(const std::string &value) {
    try {
        if (value.empty()) {
            return -1;
        } else if (pystring::count(value, ":")) {
            // 比例模式
            auto num = pystring::split(value, ":");
            if (num.size() != 2) return -1;
            return std::stof(num[0]) / std::stof(num[1]);
        } else {
            // 纯数字
            return std::stof(value);
        }
    } catch (const std::exception &e) {
        return -1;
    }
}

void MPVCore::on_update(void *self) {
    brls::sync([]() {
        uint64_t flags = mpvRenderContextUpdate(MPVCore::instance().getContext());
#if defined(MPV_NO_FB) || defined(BOREALIS_USE_DEKO3D) || defined(BOREALIS_USE_D3D11)
        // 直接绘制到屏幕上，需要屏幕每刷新一次绘制一次，在 MPVCore 的绘制函数内部处理
        (void)flags;
#else
        // 绘制到 FBO、纹理、buffer...上，在主循环结尾进行绘制，MPVCore 的绘制函数内直接绘制对应的 FBO、纹理、buffer...
        MPVCore::instance().redraw = flags & MPV_RENDER_UPDATE_FRAME;
        if (MPVCore::instance().redraw) {
#ifdef MPV_SW_RENDER
            if (!MPVCore::instance().pixels) return;
            mpvRenderContextRender(MPVCore::instance().mpv_context, MPVCore::instance().mpv_params);
            mpvRenderContextReportSwap(MPVCore::instance().mpv_context);
#else
            mpvRenderContextRender(MPVCore::instance().mpv_context, MPVCore::instance().mpv_params);
#ifdef BOREALIS_USE_OPENGL
            glBindFramebuffer(GL_FRAMEBUFFER, MPVCore::instance().default_framebuffer);
            glViewport(0, 0, (GLsizei)brls::Application::windowWidth, (GLsizei)brls::Application::windowHeight);
#endif
            mpvRenderContextReportSwap(MPVCore::instance().mpv_context);
#endif
        }
#endif
    });
}

void MPVCore::on_wakeup(void *self) {
    brls::sync([]() { MPVCore::instance().eventMainLoop(); });
}

#if defined(MPV_BUNDLE_DLL)
template <typename Module, typename fnGetProcAddress>
/* Safety stubs used only by the bundled-dll loader when a module lookup fails. */
namespace {
    int stub_set_option_string(mpv_handle *, const char *, const char *) { return -1; }
    int stub_observe_property(mpv_handle *, uint64_t, const char *, mpv_format) { return -1; }
    mpv_handle *stub_create() { return nullptr; }
    int stub_initialize(mpv_handle *) { return -1; }
    void stub_terminate_destroy(mpv_handle *) {}
    void stub_set_wakeup_callback(mpv_handle *, void (*)(void *), void *) {}
    int stub_command_string(mpv_handle *, const char *) { return -1; }
    const char *stub_error_string(int) { return "mpv unavailable"; }
    mpv_event *stub_wait_event(mpv_handle *, double) {
        static mpv_event e{};
        e.event_id = MPV_EVENT_NONE;
        return &e;
    }
    int stub_get_property(mpv_handle *, const char *, mpv_format, void *) { return -1; }
    int stub_command_async(mpv_handle *, uint64_t, const char **) { return -1; }
    char *stub_get_property_string(mpv_handle *, const char *) {
        /* 绝不能返回 NULL：调用方会直接 std::string(ptr)，那是空指针构造（实测崩过）。 */
        static char empty[1] = {0};
        return empty;
    }
    void stub_free_node_contents(mpv_node *) {}
    int stub_set_option(mpv_handle *, const char *, mpv_format, void *) { return -1; }
    void stub_free(void *) {}
    int stub_rc_create(mpv_render_context **, mpv_handle *, mpv_render_param *) { return -1; }
    void stub_rc_set_update_callback(mpv_render_context *, mpv_render_update_fn, void *) {}
    int stub_rc_render(mpv_render_context *, mpv_render_param *) { return -1; }
    void stub_rc_report_swap(mpv_render_context *) {}
    uint64_t stub_rc_update(mpv_render_context *) { return 0; }
    void stub_rc_free(mpv_render_context *) {}
    unsigned long stub_client_api_version() { return 0; }

    void installMpvStubs() {
        mpvSetOptionString                = &stub_set_option_string;
        mpvObserveProperty                = &stub_observe_property;
        mpvCreate                         = &stub_create;
        mpvInitialize                     = &stub_initialize;
        mpvTerminateDestroy               = &stub_terminate_destroy;
        mpvSetWakeupCallback              = &stub_set_wakeup_callback;
        mpvCommandString                  = &stub_command_string;
        mpvErrorString                    = &stub_error_string;
        mpvWaitEvent                      = &stub_wait_event;
        mpvGetProperty                    = &stub_get_property;
        mpvCommandAsync                   = &stub_command_async;
        mpvGetPropertyString              = &stub_get_property_string;
        mpvFreeNodeContents               = &stub_free_node_contents;
        mpvSetOption                      = &stub_set_option;
        mpvFree                           = &stub_free;
        mpvRenderContextCreate            = &stub_rc_create;
        mpvRenderContextSetUpdateCallback = &stub_rc_set_update_callback;
        mpvRenderContextRender            = &stub_rc_render;
        mpvRenderContextReportSwap        = &stub_rc_report_swap;
        mpvRenderContextUpdate            = &stub_rc_update;
        mpvRenderContextFree              = &stub_rc_free;
        mpvClientApiVersion               = &stub_client_api_version;
    }
}  // namespace

void initMpvProc(Module dll, fnGetProcAddress pGetProcAddress) {
    mpvSetOptionString     = (mpvSetOptionStringFunc)pGetProcAddress(dll, "mpv_set_option_string");
    mpvObserveProperty     = (mpvObservePropertyFunc)pGetProcAddress(dll, "mpv_observe_property");
    mpvCreate              = (mpvCreateFunc)pGetProcAddress(dll, "mpv_create");
    mpvInitialize          = (mpvInitializeFunc)pGetProcAddress(dll, "mpv_initialize");
    mpvTerminateDestroy    = (mpvTerminateDestroyFunc)pGetProcAddress(dll, "mpv_terminate_destroy");
    mpvSetWakeupCallback   = (mpvSetWakeupCallbackFunc)pGetProcAddress(dll, "mpv_set_wakeup_callback");
    mpvCommandString       = (mpvCommandStringFunc)pGetProcAddress(dll, "mpv_command_string");
    mpvErrorString         = (mpvErrorStringFunc)pGetProcAddress(dll, "mpv_error_string");
    mpvWaitEvent           = (mpvWaitEventFunc)pGetProcAddress(dll, "mpv_wait_event");
    mpvGetProperty         = (mpvGetPropertyFunc)pGetProcAddress(dll, "mpv_get_property");
    mpvCommandAsync        = (mpvCommandAsyncFunc)pGetProcAddress(dll, "mpv_command_async");
    mpvGetPropertyString   = (mpvGetPropertyStringFunc)pGetProcAddress(dll, "mpv_get_property_string");
    mpvFreeNodeContents    = (mpvFreeNodeContentsFunc)pGetProcAddress(dll, "mpv_free_node_contents");
    mpvSetOption           = (mpvSetOptionFunc)pGetProcAddress(dll, "mpv_set_option");
    mpvFree                = (mpvFreeFunc)pGetProcAddress(dll, "mpv_free");
    mpvRenderContextCreate = (mpvRenderContextCreateFunc)pGetProcAddress(dll, "mpv_render_context_create");
    mpvRenderContextUpdate = (mpvRenderContextUpdateFunc)pGetProcAddress(dll, "mpv_render_context_update");
    mpvRenderContextFree   = (mpvRenderContextFreeFunc)pGetProcAddress(dll, "mpv_render_context_free");
    mpvRenderContextRender = (mpvRenderContextRenderFunc)pGetProcAddress(dll, "mpv_render_context_render");
    mpvRenderContextSetUpdateCallback =
        (mpvRenderContextSetUpdateCallbackFunc)pGetProcAddress(dll, "mpv_render_context_set_update_callback");
    mpvRenderContextReportSwap = (mpvRenderContextReportSwapFunc)pGetProcAddress(dll, "mpv_render_context_report_swap");
    mpvClientApiVersion        = (mpvClientApiVersionFunc)pGetProcAddress(dll, "mpv_client_api_version");
}
void initMpvProc(Module dll, fnGetProcAddress pGetProcAddress) {
    mpvSetOptionString     = (mpvSetOptionStringFunc)pGetProcAddress(dll, "mpv_set_option_string");
    mpvObserveProperty     = (mpvObservePropertyFunc)pGetProcAddress(dll, "mpv_observe_property");
    mpvCreate              = (mpvCreateFunc)pGetProcAddress(dll, "mpv_create");
    mpvInitialize          = (mpvInitializeFunc)pGetProcAddress(dll, "mpv_initialize");
    mpvTerminateDestroy    = (mpvTerminateDestroyFunc)pGetProcAddress(dll, "mpv_terminate_destroy");
    mpvSetWakeupCallback   = (mpvSetWakeupCallbackFunc)pGetProcAddress(dll, "mpv_set_wakeup_callback");
    mpvCommandString       = (mpvCommandStringFunc)pGetProcAddress(dll, "mpv_command_string");
    mpvErrorString         = (mpvErrorStringFunc)pGetProcAddress(dll, "mpv_error_string");
    mpvWaitEvent           = (mpvWaitEventFunc)pGetProcAddress(dll, "mpv_wait_event");
    mpvGetProperty         = (mpvGetPropertyFunc)pGetProcAddress(dll, "mpv_get_property");
    mpvCommandAsync        = (mpvCommandAsyncFunc)pGetProcAddress(dll, "mpv_command_async");
    mpvGetPropertyString   = (mpvGetPropertyStringFunc)pGetProcAddress(dll, "mpv_get_property_string");
    mpvFreeNodeContents    = (mpvFreeNodeContentsFunc)pGetProcAddress(dll, "mpv_free_node_contents");
    mpvSetOption           = (mpvSetOptionFunc)pGetProcAddress(dll, "mpv_set_option");
    mpvFree                = (mpvFreeFunc)pGetProcAddress(dll, "mpv_free");
    mpvRenderContextCreate = (mpvRenderContextCreateFunc)pGetProcAddress(dll, "mpv_render_context_create");
    mpvRenderContextUpdate = (mpvRenderContextUpdateFunc)pGetProcAddress(dll, "mpv_render_context_update");
    mpvRenderContextFree   = (mpvRenderContextFreeFunc)pGetProcAddress(dll, "mpv_render_context_free");
    mpvRenderContextRender = (mpvRenderContextRenderFunc)pGetProcAddress(dll, "mpv_render_context_render");
    mpvRenderContextSetUpdateCallback =
        (mpvRenderContextSetUpdateCallbackFunc)pGetProcAddress(dll, "mpv_render_context_set_update_callback");
    mpvRenderContextReportSwap = (mpvRenderContextReportSwapFunc)pGetProcAddress(dll, "mpv_render_context_report_swap");
    mpvClientApiVersion        = (mpvClientApiVersionFunc)pGetProcAddress(dll, "mpv_client_api_version");
}

#if defined(PS5_NATIVE_APP)
/*
 * 原生标题把 mpv 静态链接进 eboot，而链接脚本是 { local: *; }（PS5 模块转换器只
 * 发布导入、不支持应用导出），所以 dlsym 永远解析不到 mpv_*：按动态库方式取函数
 * 指针只能得到 NULL，调用时跳到地址 0（真机现象：VideoView 构造里 MPVCore 单例
 * 初始化处 SIGSEGV，fault addr = 0）。这里直接取链接进来的符号。 */
void initMpvProcLinked() {
    mpvSetOptionString                = &mpv_set_option_string;
    mpvObserveProperty                = &mpv_observe_property;
    mpvCreate                         = &mpv_create;
    mpvInitialize                     = &mpv_initialize;
    mpvTerminateDestroy               = &mpv_terminate_destroy;
    mpvSetWakeupCallback              = &mpv_set_wakeup_callback;
    mpvCommandString                  = &mpv_command_string;
    mpvErrorString                    = &mpv_error_string;
    mpvWaitEvent                      = &mpv_wait_event;
    mpvGetProperty                    = &mpv_get_property;
    mpvCommandAsync                   = &mpv_command_async;
    mpvGetPropertyString              = &mpv_get_property_string;
    mpvFreeNodeContents               = &mpv_free_node_contents;
    mpvSetOption                      = &mpv_set_option;
    mpvFree                           = &mpv_free;
    mpvRenderContextCreate            = &mpv_render_context_create;
    mpvRenderContextUpdate            = &mpv_render_context_update;
    mpvRenderContextFree              = &mpv_render_context_free;
    mpvRenderContextRender            = &mpv_render_context_render;
    mpvRenderContextSetUpdateCallback = &mpv_render_context_set_update_callback;
    mpvRenderContextReportSwap        = &mpv_render_context_report_swap;
    mpvClientApiVersion               = &mpv_client_api_version;
}
#endif
#endif

#if !defined(MPV_BUNDLE_DLL)
/* 直接链接 libmpv 时符号齐全（libmpv 对 NULL 句柄本身返回错误而不崩），桩留空实现。 */
void installMpvStubs() {}
#endif

MPVCore::MPVCore() {
#if defined(MPV_BUNDLE_DLL)
    HMODULE hMpv = ::LoadLibraryW(L"libmpv-2.dll");
    if (!hMpv) {
        HRSRC hSrc   = ::FindResource(nullptr, "MPV", RT_RCDATA);
        HGLOBAL hRes = ::LoadResource(nullptr, hSrc);
        DWORD dwSize = ::SizeofResource(nullptr, hSrc);
        dll          = MemoryLoadLibrary(::LockResource(hRes), dwSize);
        ::FreeResource(hRes);

        brls::Logger::info("Load bundled libmpv-2.dll, size: {}", dwSize);
        initMpvProc(dll, MemoryGetProcAddress);
    } else {
        char dllPath[MAX_PATH];
        ::GetModuleFileNameA(hMpv, dllPath, sizeof(dllPath));

        brls::Logger::info("Load external `{}`", dllPath);
        initMpvProc(hMpv, GetProcAddress);
    }
#endif
    this->init();
    // Destroy mpv when application exit
    brls::Application::getExitDoneEvent()->subscribe([this]() {
        this->clean();
#ifdef MPV_SW_RENDER
        if (pixels) {
            free(pixels);
            pixels             = nullptr;
            mpv_params[3].data = nullptr;
        }
#endif
    });
}

void MPVCore::init() {
#ifdef PS5_NATIVE_APP
    /* 原生标题正式使用静态链接的 libmpv：解复用、解码、A/V 时钟和 SW render
     * 都由 mpv 提供。之前只在 WILIWILI_TEST_MPV 下进入这里，生产标题因此装上
     * 空桩却仍走 mpv VideoView，点视频时在空 mpv_context 上崩溃。 */
    wiliwili_boot_log("mpv: entering real init path");
#else
    (void)0;
#endif /* PS5_NATIVE_APP */

    setlocale(LC_NUMERIC, "C");
    this->mpv = mpvCreate();
    if (!mpv) {
#ifdef PS5_NATIVE_APP
        /* 探测路径失败：回退到桩，应用继续跑（不 brls::fatal）。 */
        wiliwili_boot_log("mpv probe: mpv_create returned NULL");
        installMpvStubs();
        return;
#else
        brls::fatal("Error Create mpv Handle");
#endif
    }
#if defined(PS5_NATIVE_APP)
    wiliwili_boot_log("mpv probe: create ok");
#endif
    std::string confDir = ProgramConfig::instance().getConfigDir();
    // misc
    mpvSetOptionString(mpv, "config", "yes");
    mpvSetOptionString(mpv, "config-dir", confDir.c_str());
    mpvSetOptionString(mpv, "gpu-shader-cache-dir", fmt::format("{}/cache", confDir).c_str());
    mpvSetOptionString(mpv, "ytdl", "no");
    mpvSetOptionString(mpv, "audio-channels", "stereo");
    mpvSetOptionString(mpv, "idle", "yes");
    mpvSetOptionString(mpv, "loop-file", "no");
    mpvSetOptionString(mpv, "osd-level", "0");
    mpvSetOptionString(mpv, "video-timing-offset", "0");  // 60fps
    mpvSetOptionString(mpv, "keep-open", "yes");
    mpvSetOptionString(mpv, "hr-seek", "yes");
    mpvSetOptionString(mpv, "reset-on-next-file", "speed,pause");
    mpvSetOptionString(mpv, "vo", "libmpv");
    mpvSetOptionString(mpv, "pulse-latency-hacks", "no");

#if defined(PS5_NATIVE_APP) && !defined(WILIWILI_SOFTWARE_RENDER)
    /* 原生 GL 路线：nanovg 把最终颜色按 BGRA 写出（video-out 的格式，见
     * borealis/extern/nanovg/nanovg_gl.h 的 outColor = result.bgra），而 mpv 因为
     * MPV_NO_FB 直接渲染进同一个 framebuffer 且写 RGBA —— 视频会红蓝互换。这里挂
     * 一段只改 MAIN pass 的用户着色器把视频的颜色换回来（不额外增加渲染遍）。 */
    {
        std::string hook = MPVCore::colorSwapShaderPath();
        if (!hook.empty()) {
            std::ofstream out(hook, std::ios::trunc);
            if (out) {
                out << "//!DESC wiliwili: swap red/blue (video-out is BGRA, mpv writes RGBA)\n"
                       "//!HOOK MAIN\n"
                       "//!BIND HOOKED\n"
                       "vec4 hook() { return HOOKED_tex(HOOKED_pos).bgra; }\n";
                out.close();
                mpvSetOptionString(mpv, "glsl-shaders", hook.c_str());
            }
        }
    }
#endif

    mpvSetOption(mpv, "brightness", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_BRIGHTNESS);
    mpvSetOption(mpv, "contrast", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_CONTRAST);
    mpvSetOption(mpv, "saturation", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_SATURATION);
    mpvSetOption(mpv, "hue", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_HUE);
    mpvSetOption(mpv, "gamma", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_GAMMA);

    if (MPVCore::LOW_QUALITY) {
        // Less cpu cost
        brls::Logger::info("lavc: skip loop filter and set fast decode");
        mpvSetOptionString(mpv, "vd-lavc-skiploopfilter", "all");
        mpvSetOptionString(mpv, "vd-lavc-fast", "yes");
        if (mpvClientApiVersion() >= MPV_MAKE_VERSION(2, 2)) {
            mpvSetOptionString(mpv, "profile", "fast");
        } else {
            mpvSetOptionString(mpv, "scale", "bilinear");
            mpvSetOptionString(mpv, "dscale", "bilinear");
            mpvSetOptionString(mpv, "dither", "no");
            mpvSetOptionString(mpv, "correct-downscaling", "no");
            mpvSetOptionString(mpv, "linear-downscaling", "no");
            mpvSetOptionString(mpv, "sigmoid-upscaling", "no");
            mpvSetOptionString(mpv, "hdr-compute-peak", "no");
            mpvSetOptionString(mpv, "allow-delayed-peak-detect", "yes");
        }
    }

#if defined(PS5_NATIVE_APP)
    configureNativeMpvSw(mpv);
#endif

    if (MPVCore::INMEMORY_CACHE) {
        // cache
        brls::Logger::info("set memory cache: {}MB", MPVCore::INMEMORY_CACHE);
        mpvSetOptionString(mpv, "demuxer-max-bytes", fmt::format("{}MiB", MPVCore::INMEMORY_CACHE).c_str());
        mpvSetOptionString(mpv, "demuxer-max-back-bytes", fmt::format("{}MiB", MPVCore::INMEMORY_CACHE / 2).c_str());
    } else {
        mpvSetOptionString(mpv, "cache", "no");
    }

    // hardware decoding
    if (HARDWARE_DEC) {
        mpvSetOptionString(mpv, "hwdec", PLAYER_HWDEC_METHOD.c_str());
        brls::Logger::info("MPV hardware decode: {}", PLAYER_HWDEC_METHOD);
    } else {
        mpvSetOptionString(mpv, "hwdec", "no");
    }

    // Making the loading process faster
#if defined(__SWITCH__)
    mpvSetOptionString(mpv, "vd-lavc-dr", "no");
    mpvSetOptionString(mpv, "vd-lavc-threads", "4");
    // This should fix random crash, but I don't know why.
    mpvSetOptionString(mpv, "opengl-glfinish", "yes");
#elif defined(PS4)
    mpvSetOptionString(mpv, "vd-lavc-threads", "6");
#elif defined(__PSV__)
    mpvSetOptionString(mpv, "vd-lavc-threads", "4");
    mpvSetOptionString(mpv, "fbo-format", "rgba8");

    // Fix vo_wait_frame() cannot be wakeup
    mpvSetOptionString(mpv, "video-latency-hacks", "yes");
#endif
    // 过低的值可能导致部分直播流无法正确播放
    mpvSetOptionString(mpv, "demuxer-lavf-analyzeduration", "0.4");
    mpvSetOptionString(mpv, "demuxer-lavf-probescore", "24");

    // log
    // mpvSetOptionString(mpv, "msg-level", "ffmpeg=trace");
    // mpvSetOptionString(mpv, "msg-level", "all=no");
    /* PS5（payload 与原生标题）都不该打开它：本机 libmpv 没有终端输出后端，
     * terminal=yes 会让 mpv 内部跳到 NULL 地址（真机表现：走到 VideoView 构造里
     * 的 MPVCore::init 就 SIGSEGV，fault addr = 0）。日志走应用自己的通道。 */
#if !defined(PS5) && !defined(PS5_NATIVE_APP)
    if (MPVCore::TERMINAL) {
        mpvSetOptionString(mpv, "terminal", "yes");
        if (brls::Logger::getLogLevel() >= brls::LogLevel::LOG_DEBUG) {
            mpvSetOptionString(mpv, "msg-level", "all=v");
        }
    }
#endif

#if defined(PS5_NATIVE_APP)
    /* 内存诊断：应用槽的 flexible 内存上限约 450MB，而打开播放器前进程已占 ~414MB。
     * 这里把 mpv 的缓存/队列压到最小，用来判定"播不了"是否由内存引起。 */
    mpvSetOptionString(mpv, "demuxer-max-bytes", "16MiB");
    mpvSetOptionString(mpv, "demuxer-max-back-bytes", "4MiB");
    mpvSetOptionString(mpv, "demuxer-readahead-secs", "1");
    mpvSetOptionString(mpv, "vd-lavc-threads", "2");
    mpvSetOptionString(mpv, "demuxer-lavf-analyzeduration", "0.2");
    mpvSetOptionString(mpv, "demuxer-lavf-probescore", "16");
    mpvSetOptionString(mpv, "audio-buffer", "0.2");
    brls::Logger::info("native: mpv memory options minimised");
#endif

#if defined(PS5) || defined(PS5_NATIVE_APP)
    /* 让 mpv 不生成日志事件：本机 libmpv 没有终端后端，terminal=yes 会让它跳到 NULL 地址。
     * 排查期要看 mpv 自己的话：加 mpv_request_log_messages(mpv, "v") 并把 msg-level 放开，
     * 日志会经 eventMainLoop 的 LOG_MESSAGE 分支（做法见 notes/06 §10.9）。 */
    mpvSetOptionString(mpv, "msg-level", "all=no");
#endif

#if defined(PS5_NATIVE_APP)
    /* 原生线唯一的音频后端就是 SDL（libmpv 里编进了 audio_out_sdl；真机表现为
     * 音轨短暂出现后 `a=-`/`ao=` 空 ⇒ 默认 AO 选择没起来）。显式指定，避免 auto 选空。 */
    mpvSetOptionString(mpv, "ao", "sdl");
#endif

#if defined(PS5_NATIVE_APP)
    wiliwili_boot_log("mpv probe: initializing");
#endif
    if (mpvInitialize(mpv) < 0) {
#if defined(PS5_NATIVE_APP)
        wiliwili_boot_log("mpv probe: mpv_initialize FAILED");
        mpvTerminateDestroy(mpv);
        installMpvStubs();
        return;
#else
        mpvTerminateDestroy(mpv);
        brls::fatal("Could not initialize mpv context");
#endif
    }
#if defined(PS5_NATIVE_APP)
    wiliwili_boot_log("mpv probe: initialize ok");
#endif
#if defined(PS5_NATIVE_APP)
    if (getenv("WILIWILI_MPV_TRACE") != nullptr) {
        mpvSetOptionString(mpv, "msg-level", "all=v");
        mpv_request_log_messages(mpv, "v");
    }
#endif

    // set observe properties
    check_error(mpvObserveProperty(mpv, 1, "core-idle", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 2, "eof-reached", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 3, "duration", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 4, "playback-time", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 5, "cache-speed", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 6, "percent-pos", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 7, "paused-for-cache", MPV_FORMAT_FLAG));
    //    check_error(mpvObserveProperty(mpv, 8, "demuxer-cache-time", MPV_FORMAT_DOUBLE));
    //    check_error(mpvObserveProperty(mpv, 9, "demuxer-cache-state", MPV_FORMAT_NODE));
    check_error(mpvObserveProperty(mpv, 10, "speed", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 11, "volume", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 12, "pause", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 13, "playback-abort", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 14, "seeking", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 15, "hwdec-current", MPV_FORMAT_STRING));
    check_error(mpvObserveProperty(mpv, 16, "path", MPV_FORMAT_STRING));
    check_error(mpvObserveProperty(mpv, 17, "brightness", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 18, "contrast", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 19, "saturation", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 20, "gamma", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 21, "hue", MPV_FORMAT_DOUBLE));

    // init renderer params
#if defined(PS5_NATIVE_APP)
    /* 【研究探针】用 SW 渲染 API：mpv 把帧软件转成 RGBA 写进调用方缓冲，**完全不碰
     * GL/EGL 入口查找**。GL 渲染 API 在标题沙箱里会崩在 mpv_render_context_create
     * （mpv 自己 dlsym 平台入口拿到 NULL，见 notes/06 §10）。
     * payload 线的 MPV_SW_RENDER 就是这个形态。默认路径（未开探测）在此早已 return。 */
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
#elif defined(MPV_SW_RENDER)
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
#elif defined(BOREALIS_USE_DEKO3D)
    int advanced_control{1};
    auto switchPlatform = (brls::SwitchVideoContext *)brls::Application::getPlatform()->getVideoContext();
    mpv_deko3d_init_params deko_init_params{switchPlatform->getDeko3dDevice()};
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_DEKO3D)},
                              {MPV_RENDER_PARAM_DEKO3D_INIT_PARAMS, &deko_init_params},
                              {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
#elif defined(BOREALIS_USE_D3D11)
    mpv_dxgi_init_params init_params{D3D11_CONTEXT->getDevice(), D3D11_CONTEXT->getSwapChain()};
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_DXGI)},
        {MPV_RENDER_PARAM_DXGI_INIT_PARAMS, &init_params},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
#elif defined(BOREALIS_USE_GXM)
    auto *video_context            = (brls::PsvVideoContext *)brls::Application::getPlatform()->getVideoContext();
    auto *window                   = video_context->getWindow();
    auto *vg                       = brls::Application::getNVGContext();
    mpv_gxm_init_params gxm_params = {
        .context        = window->context,
        .shader_patcher = window->shader_patcher,
        .buffer_index   = 0,
        .msaa           = SCE_GXM_MULTISAMPLE_4X,
    };

    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_GXM},
                                 {MPV_RENDER_PARAM_GXM_INIT_PARAMS, &gxm_params},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};

    if (mpv_fbo.render_target == nullptr) {
        int texture_width     = DISPLAY_WIDTH;
        int texture_height    = DISPLAY_HEIGHT;
        int texture_stride    = ALIGN(texture_width, 8);
        nvg_image             = nvgCreateImageRGBA(vg, texture_width, texture_height, 0, nullptr);
        NVGXMtexture *texture = nvgxmImageHandle(vg, nvg_image);

        NVGXMframebufferInitOptions framebufferOpts = {
            .display_buffer_count = 1,  // Must be 1 for custom FBOs
            .scenesPerFrame       = 1,
            .render_target        = texture,
            .color_format         = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
            .color_surface_type   = SCE_GXM_COLOR_SURFACE_LINEAR,
            .display_width        = texture_width,
            .display_height       = texture_height,
            .display_stride       = texture_stride,
        };
        NVGXMframebuffer *fbo         = gxmCreateFramebuffer(&framebufferOpts);
        mpv_fbo.render_target         = fbo->gxm_render_target;
        mpv_fbo.color_surface         = &fbo->gxm_color_surfaces[0].surface;
        mpv_fbo.depth_stencil_surface = &fbo->gxm_depth_stencil_surface;
        mpv_fbo.w                     = texture_width;
        mpv_fbo.h                     = texture_height;
    }
#else
    int advanced_control{1};
    mpv_opengl_init_params gl_init_params{get_proc_address, nullptr};
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                              {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
                              {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
#if defined(GLFW_EXPOSE_NATIVE_X11)
                              {MPV_RENDER_PARAM_X11_DISPLAY, glfwGetX11Display()},
#endif
#if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
                              {MPV_RENDER_PARAM_WL_DISPLAY, glfwGetWaylandDisplay()},
#endif
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
#endif

#if defined(PS5_NATIVE_APP)
    wiliwili_boot_log("mpv probe: render context create");
#endif
    if (mpvRenderContextCreate(&mpv_context, mpv, params) < 0) {
#if defined(PS5_NATIVE_APP)
        wiliwili_boot_log("mpv probe: render context FAILED");
        mpvTerminateDestroy(mpv);
        installMpvStubs();
        return;
#else
        mpvTerminateDestroy(mpv);
        brls::fatal("failed to initialize mpv GL context");
#endif
    }
#if defined(PS5_NATIVE_APP)
    wiliwili_boot_log("mpv probe: SW render context ok -> mpv core usable");
    /* 原生线已改为**由 mpv 播放**（见 notes/06 §10.6）：保留 mpv_context 之后**不能早退**——
     * 后面的 `mpvSetWakeupCallback`（事件）与 `mpvRenderContextSetUpdateCallback`（每帧渲染）
     * 是播放的入口；漏掉它们会只剩 resize 时渲染一次、且收不到任何事件/日志（真机实测）。
     * `initializeVideo()` 在 MPV_SW_RENDER 下是空实现（守卫含 `!defined(MPV_SW_RENDER)`）。 */
#endif
#ifdef BOREALIS_USE_D3D11
    wiliwili::initCrashDump();
#endif
    brls::Logger::info("MPV Version: {}", safeMpvText(mpvGetPropertyString(mpv, "mpv-version")));
    brls::Logger::info("FFMPEG Version: {}", safeMpvText(mpvGetPropertyString(mpv, "ffmpeg-version")));
    command_async("set", "audio-client-name", APPVersion::getPackageName());
    setVolume(MPVCore::VIDEO_VOLUME);

    // set event callback
    mpvSetWakeupCallback(mpv, on_wakeup, this);
    // set render callback
    mpvRenderContextSetUpdateCallback(mpv_context, on_update, this);

    focusSubscription = brls::Application::getWindowFocusChangedEvent()->subscribe([this](bool focus) {
        static bool playing = false;
        static std::chrono::system_clock::time_point sleepTime{};
        if (focus) {
            // restore AUTO_PLAY
            AUTO_PLAY = ProgramConfig::instance().getBoolOption(SettingItem::PLAYER_AUTO_PLAY);
            // application is on top
            auto timeNow = std::chrono::system_clock::now();
            if (playing && timeNow < (sleepTime + std::chrono::seconds(120))) {
                resume();
            }
        } else {
            // application is sleep, save the current state
            playing   = isPlaying();
            sleepTime = std::chrono::system_clock::now();
            pause();
            // do not automatically play video
            AUTO_PLAY = false;
        }
    });

    brls::Application::getExitEvent()->subscribe([]() { disableDimming(false); });

    this->initializeVideo();
}

#ifdef MPV_BUNDLE_DLL
MPVCore::~MPVCore() { MemoryFreeLibrary(dll); }
#else
MPVCore::~MPVCore() = default;
#endif

void MPVCore::clean() {
    check_error(mpvCommandString(this->mpv, "quit"));

    brls::Application::getWindowFocusChangedEvent()->unsubscribe(focusSubscription);

    brls::Logger::info("uninitialize Video");
    this->uninitializeVideo();

    brls::Logger::info("trying free mpv context");
    if (this->mpv_context) {
        mpvRenderContextFree(this->mpv_context);
        this->mpv_context = nullptr;
    }

    brls::Logger::info("trying terminate mpv");
    if (this->mpv) {
        mpvTerminateDestroy(this->mpv);
        this->mpv = nullptr;
    }
}

void MPVCore::restart() {
    this->clean();
    this->init();
    setMirror(MPVCore::VIDEO_MIRROR);
    setShader(currentShaderProfile, currentShader, currentSetting, false);
    mpvCoreEvent.fire(MpvEventEnum::RESTART);

    // 如果正在播放视频时重启mpv，重启前后存在软硬解切，那么视频尺寸会不正确
    // 手动设置一次尺寸可以解决这个问题 (同 MPVCore::reset())
    setFrameSize(rect);
}

void MPVCore::uninitializeVideo() {
#ifdef MPV_USE_FB
    if (media_framebuffer != 0) glDeleteFramebuffers(1, &media_framebuffer);
    if (media_texture != 0) glDeleteTextures(1, &media_texture);
    if (shader.vbo != 0) glDeleteBuffers(1, &shader.vbo);
    if (shader.ebo != 0) glDeleteBuffers(1, &shader.ebo);
    if (shader.prog != 0) glDeleteProgram(shader.prog);
#ifdef MPV_USE_VAO
    if (shader.vao != 0) glDeleteVertexArrays(1, &shader.vao);
    shader.vao = 0;
#endif
    media_framebuffer = 0;
    media_texture     = 0;
    shader.vbo        = 0;
    shader.ebo        = 0;
    shader.prog       = 0;
#endif
}

void MPVCore::initializeVideo() {
#if defined(BOREALIS_USE_OPENGL) && !defined(MPV_SW_RENDER)
    // Get default framebuffer
#if defined(IOS)
    // SDL: OpenGL ES on iOS doesn't use the traditional system-framebuffer setup provided in other operating systems.
    default_framebuffer = 1;
#else
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &default_framebuffer);
#endif
#endif

#if defined(MPV_NO_FB)
    mpv_fbo.fbo = default_framebuffer;
    glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
#elif defined(MPV_USE_FB)
    if (this->media_texture != 0) return;
    brls::Logger::debug("initializeGL");

    // create texture
    glGenTextures(1, &this->media_texture);
    glBindTexture(GL_TEXTURE_2D, this->media_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (int)brls::Application::windowWidth, (int)brls::Application::windowHeight,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // create frame buffer
    glGenFramebuffers(1, &this->media_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, this->media_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->media_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        brls::Logger::error("glCheckFramebufferStatus failed");
        return;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
    this->mpv_fbo.fbo = (int)this->media_framebuffer;
    brls::Logger::debug("create fbo and texture done\n");

    // build and compile our shader program
    // ------------------------------------
    // vertex shader
    GLuint vertexShader = createShader(GL_VERTEX_SHADER, vertexShaderSource);
    // fragment shader
    GLuint fragmentShader = createShader(GL_FRAGMENT_SHADER, fragmentShaderSource);

    // link shaders
    GLuint shaderProgram = linkProgram(vertexShader, fragmentShader);

    if (vertexShader) glDeleteShader(vertexShader);
    if (fragmentShader) glDeleteShader(fragmentShader);
    this->shader.prog = shaderProgram;

    // set up vertex data (and buffer(s)) and configure vertex attributes
    // ------------------------------------------------------------------
    unsigned int indices[] = {0, 1, 3, 1, 2, 3};
    glGenBuffers(1, &this->shader.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, this->shader.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenBuffers(1, &this->shader.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->shader.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

#ifdef MPV_USE_VAO
    glGenVertexArrays(1, &this->shader.vao);
    glBindVertexArray(this->shader.vao);
    glBindBuffer(GL_ARRAY_BUFFER, this->shader.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->shader.ebo);

    GLuint aPos = glGetAttribLocation(shaderProgram, "aPos");
    glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)nullptr);
    glEnableVertexAttribArray(aPos);

    GLuint aTexCoord = glGetAttribLocation(shaderProgram, "aTexCoord");
    glVertexAttribPointer(aTexCoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(aTexCoord);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
#endif
    brls::Logger::debug("initializeGL done");
#endif
}

void MPVCore::setFrameSize(brls::Rect r) {
    rect = r;
    if (std::isnan(rect.getWidth()) || std::isnan(rect.getHeight())) return;

#ifdef MPV_SW_RENDER
    /* Native SW surfaces follow the player rectangle. The AGC NanoVG rectangle
     * already uses physical output pixels, so do not scale it a second time.
     * Recreate buffers only when the rectangle size changes. */
#ifdef BOREALIS_USE_D3D11
    // 使用 dx11 的拷贝交换，否则视频渲染异常
    const static int mpvImageFlags = NVG_IMAGE_STREAMING | NVG_IMAGE_COPY_SWAP;
#else
    const static int mpvImageFlags = 0;
#endif
    auto *vg = brls::Application::getNVGContext();
#ifdef PS5_NATIVE_APP
    /* Fullscreen keeps a 1920x1080 SW surface; WILIWILI_SW_SIZE=800x450 remains
     * an explicit fallback for low-memory or diagnostic runs. Small-player rects
     * still scale their surface down, so the detail-page 60 FPS path is unchanged. */
    int maxSwWidth  = 1920;
    int maxSwHeight = 1080;
    const char *requestedSwSize = std::getenv("WILIWILI_SW_SIZE");
    int requestedWidth = 0;
    int requestedHeight = 0;
    if (requestedSwSize != nullptr && std::sscanf(requestedSwSize, "%dx%d", &requestedWidth, &requestedHeight) == 2 &&
        requestedWidth > 0 && requestedHeight > 0) {
        maxSwWidth  = requestedWidth;
        maxSwHeight = requestedHeight;
    }
    const int rectWidth  = (int)std::ceil(rect.getWidth());
    const int rectHeight = (int)std::ceil(rect.getHeight());
    if (rectWidth <= 0 || rectHeight <= 0) return;
    int drawWidth  = rectWidth;
    int drawHeight = rectHeight;
    const bool fullScreen = rectWidth >= 1200 && rectHeight >= 650;
    if (fullScreen && maxSwWidth >= 1920 && maxSwHeight >= 1080) {
        drawWidth  = 1920;
        drawHeight = 1080;
    } else {
        const float scale = std::min(1.0f, std::min((float)maxSwWidth / rectWidth, (float)maxSwHeight / rectHeight));
        drawWidth         = std::max(1, (int)std::ceil(rectWidth * scale));
        drawHeight        = std::max(1, (int)std::ceil(rectHeight * scale));
    }

    if (pixels != nullptr && nvg_image != 0 && sw_size[0] == drawWidth && sw_size[1] == drawHeight) return;

    if (nvg_image != 0) {
        nvgDeleteImage(vg, nvg_image);
        nvg_image = 0;
    }
    free(pixels);
    pixels             = nullptr;
    mpv_params[3].data = nullptr;
    size_t frameSize   = (size_t)drawWidth * (size_t)drawHeight;
    if (posix_memalign(&pixels, 64, frameSize * PIXCEL_SIZE) != 0) pixels = nullptr;
    if (pixels == nullptr) return;
    mpv_params[3].data = pixels;
#else
    int drawWidth  = rect.getWidth();
    int drawHeight = rect.getHeight();
    if (drawWidth == 0 || drawHeight == 0) return;
    int frameSize = drawWidth * drawHeight;

    if (pixels != nullptr && frameSize > sw_size[0] * sw_size[1]) {
        brls::Logger::debug("Enlarge video surface buffer");
        free(pixels);
        pixels = nullptr;
    }

    if (pixels == nullptr) {
        pixels             = malloc(frameSize * PIXCEL_SIZE);
        mpv_params[3].data = pixels;
    }
#endif

#ifndef PS5_NATIVE_APP
    if (nvg_image) nvgDeleteImage(vg, nvg_image);
#endif
    /* 创建时不带数据：数据每帧由 nvgUpdateImage 上传，避免把未初始化缓冲交给驱动。 */
    nvg_image = nvgCreateImageRGBA(vg, drawWidth, drawHeight, mpvImageFlags, nullptr);
    /* 检查点：SW 视频面与渲染上下文就绪。ctx 为 NULL 时后面 mpv 渲染会在 mpv 内部
     * 对 NULL+0x70 取字段后跳 0（真机 addr=0x70/rip=0x0，见 notes/06 §10.8）。 */
    {
        char dline[224];
        snprintf(dline, sizeof(dline), "mpv-sw: surface=%d ctx=%p %dx%d display=%dx%d ptr=%p mod64=%zu",
                 nvg_image, (void *)mpv_context, drawWidth, drawHeight, (int)rect.getWidth(), (int)rect.getHeight(),
                 pixels, (size_t)pixels & 63u);
        wiliwili_boot_log(dline);
    }

    sw_size[0] = drawWidth;
    sw_size[1] = drawHeight;
    pitch      = PIXCEL_SIZE * drawWidth;

    // 在视频暂停时调整纹理尺寸，视频画面会被清空为黑色，强制重新绘制一次，避免这个问题
    mpvRenderContextRender(mpv_context, mpv_params);
    mpvRenderContextReportSwap(mpv_context);
    this->redraw = true; /* 新面已渲染一次 ⇒ 让 draw() 上传这一帧 */
#elif !defined(MPV_USE_FB)
    // Using default framebuffer
#if defined(BOREALIS_USE_GXM)
    // This line will be called between beginFrame() and endFrame() in Application::frame(),
    // but mpvRenderContextRender(...) will call functions similar to beginFrame() and endFrame() to draw content to FBO,
    // and that will cause error in GXM, so call in brls::sync to make the mpv drawing calls outside the brls::Application::frame().
    brls::sync([this]() {
        mpvRenderContextRender(mpv_context, mpv_params);
        mpvRenderContextReportSwap(mpv_context);
    });
#elif defined(BOREALIS_USE_D3D11)
#else
    this->mpv_fbo.w = brls::Application::windowWidth;
    this->mpv_fbo.h = brls::Application::windowHeight;
#endif
    if (brls::Application::contentWidth < rect.getMaxX() || brls::Application::contentHeight < rect.getMaxY()) return;
    command_async("set", "video-margin-ratio-right",
                  (brls::Application::contentWidth - rect.getMaxX()) / brls::Application::contentWidth);
    command_async("set", "video-margin-ratio-bottom",
                  (brls::Application::contentHeight - rect.getMaxY()) / brls::Application::contentHeight);
    command_async("set", "video-margin-ratio-top", rect.getMinY() / brls::Application::contentHeight);
    command_async("set", "video-margin-ratio-left", rect.getMinX() / brls::Application::contentWidth);
#else
    if (this->media_texture == 0) return;
    int drawWidth  = rect.getWidth() * brls::Application::windowScale;
    int drawHeight = rect.getHeight() * brls::Application::windowScale;

    if (drawWidth == 0 || drawHeight == 0) return;
    brls::Logger::debug("MPVCore::setFrameSize: {}/{}", drawWidth, drawHeight);
    glBindTexture(GL_TEXTURE_2D, this->media_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, drawWidth, drawHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    this->mpv_fbo.w = drawWidth;
    this->mpv_fbo.h = drawHeight;

    float new_min_x = rect.getMinX() / brls::Application::contentWidth * 2 - 1;
    float new_min_y = 1 - rect.getMinY() / brls::Application::contentHeight * 2;
    float new_max_x = rect.getMaxX() / brls::Application::contentWidth * 2 - 1;
    float new_max_y = 1 - rect.getMaxY() / brls::Application::contentHeight * 2;

    vertices[0]  = new_max_x;
    vertices[1]  = new_min_y;
    vertices[5]  = new_max_x;
    vertices[6]  = new_max_y;
    vertices[10] = new_min_x;
    vertices[11] = new_max_y;
    vertices[15] = new_min_x;
    vertices[16] = new_min_y;

    glBindBuffer(GL_ARRAY_BUFFER, this->shader.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // 在视频暂停时调整纹理尺寸，视频画面会被清空为黑色，强制重新绘制一次，避免这个问题
    mpvRenderContextRender(mpv_context, mpv_params);
    glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
    glViewport(0, 0, (GLsizei)brls::Application::windowWidth, (GLsizei)brls::Application::windowHeight);
    mpvRenderContextReportSwap(mpv_context);
#endif
}

bool MPVCore::isValid() { return mpv_context != nullptr; }

void MPVCore::draw(brls::Rect area, float alpha) {
    if (mpv_context == nullptr) return;
    if (!(this->rect == area)) setFrameSize(area);

#ifdef MPV_SW_RENDER
    if (!pixels) return;

    auto *vg = brls::Application::getNVGContext();
    /* 只在 mpv 产出新帧时上传；SW 面尺寸随播放器区域变化，避免暂停时重复上传整张
     * RGBA 纹理。全屏时为当前物理窗口尺寸，通常是 1920x1080。 */
    if (this->redraw) {
        this->redraw = false;
        nvgUpdateImage(vg, nvg_image, (const unsigned char *)pixels);
    }

    // draw black background
    nvgBeginPath(vg);
    NVGcolor bg{};
    bg.a = alpha;
    nvgFillColor(vg, bg);
    nvgRect(vg, rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
    nvgFill(vg);

    // draw video
    nvgBeginPath(vg);
    nvgRect(vg, rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
    // Pattern coordinates are absolute; an origin of (0, 0) shifts offset VideoViews.
    // That shift exposes/clips the right and bottom edges of the small player.
    nvgFillPaint(vg, nvgImagePattern(vg, rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight(), 0,
                                     nvg_image, alpha));
    nvgFill(vg);
#elif defined(BOREALIS_USE_GXM)
    auto *vg = brls::Application::getNVGContext();

    if (MPVCore::VIDEO_MIRROR) {
        nvgSave(vg);
        nvgTranslate(vg, area.getWidth() + area.getMinX() * 2, 0);
        nvgScale(vg, -1, 1);
    }
    NVGpaint img = nvgImagePattern(vg, 0, 0, brls::Application::contentWidth, brls::Application::contentHeight, 0,
                                   nvg_image, alpha);
    nvgBeginPath(vg);
    nvgRect(vg, area.getMinX(), area.getMinY(), area.getWidth(), area.getHeight());
    nvgFillPaint(vg, img);
    nvgFill(vg);
    if (MPVCore::VIDEO_MIRROR) {
        nvgRestore(vg);
    }
#elif defined(MPV_NO_FB) || defined(BOREALIS_USE_DEKO3D) || defined(BOREALIS_USE_D3D11)
    // 只在非透明时绘制视频，可以避免退出页面时视频画面残留
    if (alpha >= 1) {
#ifdef BOREALIS_USE_DEKO3D
        static auto videoContext = (brls::SwitchVideoContext *)brls::Application::getPlatform()->getVideoContext();
        mpv_fbo.tex              = videoContext->getFramebuffer();
        videoContext->queueSignalFence(&readyFence);
        videoContext->queueFlush();
#endif
        mpvRenderContextRender(this->mpv_context, mpv_params);
#ifdef BOREALIS_USE_DEKO3D
        videoContext->queueWaitFence(&doneFence);
#elif defined(BOREALIS_USE_D3D11)
        D3D11_CONTEXT->beginFrame();
#else
        glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
        glViewport(0, 0, brls::Application::windowWidth, brls::Application::windowHeight);
#endif
        mpvRenderContextReportSwap(this->mpv_context);

        // 画背景来覆盖mpv的黑色边框
        if (rect.getWidth() < brls::Application::contentWidth) {
            auto *vg = brls::Application::getNVGContext();
            nvgBeginPath(vg);
            nvgFillColor(vg, brls::Application::getTheme().getColor("brls/background"));
            nvgRect(vg, 0, 0, rect.getMinX(), brls::Application::contentHeight);
            nvgRect(vg, rect.getMaxX(), 0, brls::Application::contentWidth - rect.getMaxX(),
                    brls::Application::contentHeight);
            nvgRect(vg, rect.getMinX() - 1, 0, rect.getWidth() + 2, rect.getMinY());
            nvgRect(vg, rect.getMinX() - 1, rect.getMaxY(), rect.getWidth() + 2,
                    brls::Application::contentHeight - rect.getMaxY());
            nvgFill(vg);
        }
    }
#else
    // OpenGL 将 FBO 绘制到屏幕上
    glUseProgram(shader.prog);
    glBindTexture(GL_TEXTURE_2D, this->media_texture);
#ifdef MPV_USE_VAO
    glBindVertexArray(shader.vao);
#else
    glBindBuffer(GL_ARRAY_BUFFER, shader.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, shader.ebo);
    static GLuint aPos = glGetAttribLocation(shader.prog, "aPos");
    glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)nullptr);
    glEnableVertexAttribArray(aPos);

    static GLuint aTexCoord = glGetAttribLocation(shader.prog, "aTexCoord");
    glVertexAttribPointer(aTexCoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(aTexCoord);
#endif

    // Set alpha
    if (alpha < 1) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    static GLint alphaID = glGetUniformLocation(shader.prog, "Alpha");
    glUniform1f(alphaID, alpha);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
#endif
}

mpv_render_context *MPVCore::getContext() { return this->mpv_context; }

mpv_handle *MPVCore::getHandle() { return this->mpv; }

MPVEvent *MPVCore::getEvent() { return &this->mpvCoreEvent; }

std::string MPVCore::getCacheSpeed() const {
    if (cache_speed >> 20 > 0) {
        return fmt::format("{:.2f} MB/s", (cache_speed >> 10) / 1024.0f);
    } else if (cache_speed >> 10 > 0) {
        return fmt::format("{:.2f} KB/s", cache_speed / 1024.0f);
    } else {
        return fmt::format("{} B/s", cache_speed);
    }
}

void MPVCore::eventMainLoop() {
    while (true) {
        auto event = mpvWaitEvent(this->mpv, 0);
        switch (event->event_id) {
            case MPV_EVENT_NONE:
                return;
            case MPV_EVENT_LOG_MESSAGE: {
                auto log = (mpv_event_log_message *)event->data;
                if (log->log_level <= MPV_LOG_LEVEL_ERROR) {
                    brls::Logger::error("{}: {}", safeMpvText(log->prefix), safeMpvText(log->text));
                } else if (log->log_level <= MPV_LOG_LEVEL_WARN) {
                    brls::Logger::warning("{}: {}", safeMpvText(log->prefix), safeMpvText(log->text));
                } else if (log->log_level <= MPV_LOG_LEVEL_INFO) {
                    brls::Logger::info("{}: {}", safeMpvText(log->prefix), safeMpvText(log->text));
                } else if (log->log_level <= MPV_LOG_LEVEL_V) {
                    brls::Logger::debug("{}: {}", safeMpvText(log->prefix), safeMpvText(log->text));
                } else {
                    brls::Logger::verbose("{}: {}", safeMpvText(log->prefix), safeMpvText(log->text));
                }
            } break;
            case MPV_EVENT_FILE_LOADED: {
                brls::Logger::info("========> MPV_EVENT_FILE_LOADED");
                /* 启动/健康检查点（原生线专用）：mpv 的日志在本平台被关闭，播放状态只能靠
                 * 这两条事件落到启动日志里判断"流到底有没有加载、有没有真的开播"。 */
                wiliwili_boot_log("mpv: file loaded");
#if defined(PS5_NATIVE_APP)
                int64_t aid = -1;
                mpvGetProperty(this->mpv, "aid", MPV_FORMAT_INT64, &aid);
                auto codec = mpvPropertyText(this->mpv, "audio-codec");
                auto ao    = mpvPropertyText(this->mpv, "current-ao");
                auto device = mpvPropertyText(this->mpv, "audio-device");
                char audio_line[256];
                snprintf(audio_line, sizeof(audio_line), "mpv: audio aid=%lld codec=%s ao=%s device=%s",
                         (long long)aid, codec.c_str(), ao.c_str(), device.c_str());
                wiliwili_boot_log(audio_line);
#endif
                // event 8: 文件预加载结束，准备解码
                mpvCoreEvent.fire(MpvEventEnum::MPV_LOADED);
                // 发布一次进度更新事件，避免进度条在0秒时没有进度更新
                video_progress = 0;
                mpvCoreEvent.fire(MpvEventEnum::UPDATE_PROGRESS);
                // `loadfile replace` already discards the previous playlist. Sending
                // `playlist-clear` here races the next replacement during video switches.

                if (AUTO_PLAY) {
                    mpvCoreEvent.fire(MpvEventEnum::MPV_RESUME);
                    this->resume();
                } else {
                    mpvCoreEvent.fire(MpvEventEnum::MPV_PAUSE);
                    this->pause();
                }
                break;
            }
            case MPV_EVENT_START_FILE:
                // event 6: 开始加载文件
                brls::Logger::info("========> MPV_EVENT_START_FILE");

                // show osd for a really long time
                mpvCoreEvent.fire(MpvEventEnum::START_FILE);

                mpvCoreEvent.fire(MpvEventEnum::LOADING_START);
                break;
            case MPV_EVENT_PLAYBACK_RESTART:
                // event 21: 开始播放文件（一般是播放或调整进度结束之后触发）
                brls::Logger::info("========> MPV_EVENT_PLAYBACK_RESTART");
                wiliwili_boot_log("mpv: playback restart");
#if defined(PS5_NATIVE_APP)
                {
                    int64_t aid = -1;
                    int64_t tracks = -1;
                    int aid_rc = mpvGetProperty(this->mpv, "aid", MPV_FORMAT_INT64, &aid);
                    int tracks_rc = mpvGetProperty(this->mpv, "track-list/count", MPV_FORMAT_INT64, &tracks);
                    auto codec = mpvPropertyText(this->mpv, "audio-codec");
                    auto ao    = mpvPropertyText(this->mpv, "current-ao");
                    char audio_line[224];
                    snprintf(audio_line, sizeof(audio_line),
                             "mpv: audio active aid=%lld(rc=%d) tracks=%lld(rc=%d) codec=%s ao=%s",
                             (long long)aid, aid_rc, (long long)tracks, tracks_rc, codec.c_str(), ao.c_str());
                    wiliwili_boot_log(audio_line);
                }
#endif
                video_stopped = false;
                mpvCoreEvent.fire(MpvEventEnum::LOADING_END);
                break;
            case MPV_EVENT_END_FILE: {
                // event 7: 文件播放结束
                brls::Logger::info("========> MPV_STOP");
                /* 带上结束原因：加载失败（reason=error）与正常播完在排查时意义完全不同。 */
                {
                    auto *end = (mpv_event_end_file *)event->data;
                    char line[160];
                    snprintf(line, sizeof(line), "mpv: end file reason=%d error=%d", end ? end->reason : -1,
                             end ? end->error : 0);
                    wiliwili_boot_log(line);
                }
                mpvCoreEvent.fire(MpvEventEnum::MPV_STOP);
                video_stopped = true;
                auto node     = (mpv_event_end_file *)event->data;
                if (node->reason == MPV_END_FILE_REASON_ERROR) {
                    mpv_error_code = node->error;
                    brls::Logger::error("========> MPV ERROR: {}", safeMpvText(mpvErrorString(node->error)));
                    mpvCoreEvent.fire(MpvEventEnum::MPV_FILE_ERROR);
                }
#ifdef BOREALIS_USE_GXM
                else {
                    setFrameSize(rect);  // 清空残留画面
                }
#endif

                break;
            }
            case MPV_EVENT_PROPERTY_CHANGE: {
                auto *data = ((mpv_event_property *)event->data)->data;
                switch (event->reply_userdata) {
                    case 1:
                        if (data) {
                            bool playing = *(int *)data == 0;
                            if (playing != video_playing) {
                                video_playing = playing;
                                mpvCoreEvent.fire(MpvEventEnum::MPV_IDLE);
                            }
                            video_playing = playing;
                            disableDimming(video_playing);
                        }
                        break;
                    case 2:
                        if (data) video_eof = *(int *)data;
                        // 当视频播放自然结束时会先触发一次 EOF，如果这时 stop 或者设置了新的播放文件，会触发 STOP 然后再触发一次 EOF
                        // 避免多次触发 EOF，将第二种情况排除在外
                        if (video_eof && !video_stopped) {
                            brls::Logger::info("========> END OF FILE");
                            mpvCoreEvent.fire(MpvEventEnum::END_OF_FILE);
                        }
                        break;
                    case 3:
                        // 视频总时长更新
                        if (((mpv_event_property *)event->data)->data)
                            duration = *(int64_t *)((mpv_event_property *)event->data)->data;
                        if (duration != 0) {
                            brls::Logger::debug("========> duration: {}", duration);
                            mpvCoreEvent.fire(MpvEventEnum::UPDATE_DURATION);
                        }
                        break;
                    case 4:
                        // 播放进度更新
                        if (((mpv_event_property *)event->data)->data) {
                            playback_time = *(double *)((mpv_event_property *)event->data)->data;
                            if (video_progress != (int64_t)playback_time) {
                                video_progress = (int64_t)playback_time;
                                mpvCoreEvent.fire(MpvEventEnum::UPDATE_PROGRESS);
                                // 判断是否需要暂停播放
                                if (CLOSE_TIME > 0 && wiliwili::getUnixTime() > CLOSE_TIME) {
                                    CLOSE_TIME = 0;
                                    this->pause();
                                }
                            }
                        }

                        break;
                    case 5:
                        // 视频 cache speed
                        if (((mpv_event_property *)event->data)->data) {
                            cache_speed = *(int64_t *)((mpv_event_property *)event->data)->data;
                            mpvCoreEvent.fire(MpvEventEnum::CACHE_SPEED_CHANGE);
                        }
                        break;
                    case 6:
                        // 视频进度更新（百分比）
                        if (((mpv_event_property *)event->data)->data) {
                            percent_pos = *(double *)((mpv_event_property *)event->data)->data;
                        }
                        break;
                    case 7:
                        // 发生了缓存等待
                        if (!data) break;

                        if (*(int *)data) {
                            brls::Logger::info("========> VIDEO PAUSED FOR CACHE");
                            mpvCoreEvent.fire(MpvEventEnum::LOADING_START);
                        } else {
                            brls::Logger::info("========> VIDEO RESUME FROM CACHE");
                            mpvCoreEvent.fire(MpvEventEnum::LOADING_END);
                        }
                        break;
                    case 8:
                        // 缓存时间
                        if (((mpv_event_property *)event->data)->data) {
                            brls::Logger::verbose("demuxer-cache-time: {}",
                                                  *(double *)((mpv_event_property *)event->data)->data);
                        }
                        break;
                    case 9:
                        // 缓存信息
                        if (((mpv_event_property *)event->data)->data) {
                            auto *node = (mpv_node *)((mpv_event_property *)event->data)->data;
                            std::unordered_map<std::string, mpv_node> node_map;
                            for (int i = 0; i < node->u.list->num; i++) {
                                node_map.insert(
                                    std::make_pair(safeMpvText(node->u.list->keys[i]), node->u.list->values[i]));
                            }
                            brls::Logger::debug(
                                "total-bytes: {:.2f}MB; cache-duration: "
                                "{:.2f}; "
                                "underrun: {}; fw-bytes: {:.2f}MB; bof-cached: "
                                "{}; eof-cached: {}; file-cache-bytes: {}; "
                                "raw-input-rate: {:.2f};",
                                node_map["total-bytes"].u.int64 / 1048576.0, node_map["cache-duration"].u.double_,
                                node_map["underrun"].u.flag, node_map["fw-bytes"].u.int64 / 1048576.0,
                                node_map["bof-cached"].u.flag, node_map["eof-cached"].u.flag,
                                node_map["file-cache-bytes"].u.int64 / 1048576.0,
                                node_map["raw-input-rate"].u.int64 / 1048576.0);
                        }
                        break;
                    case 10:
                        // 倍速信息
                        if (data) {
                            video_speed = *(double *)data;
                            mpvCoreEvent.fire(VIDEO_SPEED_CHANGE);
                        }
                        break;
                    case 11:
                        // 音量信息
                        if (data) {
                            if (*(int64_t *)data > 0 && volume == 0) {
                                mpvCoreEvent.fire(VIDEO_UNMUTE);
                            } else if (*(int64_t *)data == 0 && volume > 0) {
                                mpvCoreEvent.fire(VIDEO_MUTE);
                            }
                            volume = *(int64_t *)data;
                            mpvCoreEvent.fire(VIDEO_VOLUME_CHANGE);
                        }
                        break;
                    case 12:
                        if (data) video_paused = *(int *)data;
                        if (video_paused) {
                            brls::Logger::info("========> PAUSE");
                            mpvCoreEvent.fire(MpvEventEnum::MPV_PAUSE);
                        } else if (!video_stopped) {
                            brls::Logger::info("========> RESUME");
                            mpvCoreEvent.fire(MpvEventEnum::MPV_RESUME);
                        }
                        break;
                    case 13:
                        if (data) video_stopped = *(int *)data;

                        break;
                    case 14:
                        if (data) video_seeking = *(int *)data;
                        if (video_seeking) {
                            brls::Logger::info("========> VIDEO SEEKING");
                            mpvCoreEvent.fire(MpvEventEnum::LOADING_START);
                        }
                        break;
                    case 15:
                        if (data) {
                            const char *value = *(char **)data;
                            hwCurrent         = value != nullptr ? value : "";
                            brls::Logger::info("========> HW: {}", hwCurrent);
                            GA("hwdec", {{"hwdec", hwCurrent}})
                        }
                        break;
                    case 16:
                        if (data) {
                            const char *value = *(char **)data;
                            filepath          = value != nullptr ? value : "";
                        }
                        break;
                    case 17:
                        if (data) video_brightness = *(double *)data;
                        break;
                    case 18:
                        if (data) video_contrast = *(double *)data;
                        break;
                    case 19:
                        if (data) video_saturation = *(double *)data;
                        break;
                    case 20:
                        if (data) video_gamma = *(double *)data;
                        break;
                    case 21:
                        if (data) video_hue = *(double *)data;
                        break;
                    default:
                        break;
                }
                break;
            }
            default:
                break;
        }
    }
}

void MPVCore::reset() {
    brls::Logger::debug("MPVCore::reset");
    mpvCoreEvent.fire(MpvEventEnum::RESET);
    this->percent_pos    = 0;
    this->duration       = 0;  // second
    this->cache_speed    = 0;  // Bps
    this->playback_time  = 0;
    this->video_progress = 0;
    this->mpv_error_code = 0;
    this->video_aspect   = aspectConverter(MPVCore::VIDEO_ASPECT);

    // 软硬解切换后应该手动设置一次渲染尺寸
    // 切换视频前设置渲染尺寸可以顺便将上一条视频的最后一帧画面清空
    setFrameSize(rect);
}

void MPVCore::setUrl(const std::string &url, const std::string &extra, const std::string &method) {
    brls::Logger::debug("{} Url: {}, extra: {}", method, url, extra);
    if (extra.empty()) {
        command_async("loadfile", url, method);
    } else {
        if (mpvClientApiVersion() >= MPV_MAKE_VERSION(2, 3))
            command_async("loadfile", url, method, "0", extra);
        else
            command_async("loadfile", url, method, extra);
    }
}

void MPVCore::setBackupUrl(const std::string &url, const std::string &extra) { this->setUrl(url, extra, "append"); }

void MPVCore::setVolume(int64_t value) {
    if (value < 0 || value > 100) return;
    command_async("set", "volume", value);
    MPVCore::VIDEO_VOLUME = (int)value;
}

void MPVCore::setVolume(const std::string &value) {
    command_async("set", "volume", value);
    MPVCore::VIDEO_VOLUME = std::stoi(value);
}

int64_t MPVCore::getVolume() const { return this->volume; }

void MPVCore::resume() { command_async("set", "pause", "no"); }

void MPVCore::pause() { command_async("set", "pause", "yes"); }

void MPVCore::stop() { command_async("stop"); }

void MPVCore::seek(int64_t p) { command_async("seek", p, "absolute"); }

void MPVCore::seek(const std::string &p) { command_async("seek", p, "absolute"); }

void MPVCore::seekRelative(int64_t p) { command_async("seek", p, "relative"); }

void MPVCore::seekPercent(double p) { command_async("seek", p * 100, "absolute-percent"); }

bool MPVCore::isStopped() const { return video_stopped; }

bool MPVCore::isPlaying() const { return video_playing; }

bool MPVCore::isPaused() const { return video_paused; }

double MPVCore::getSpeed() const { return video_speed; }

void MPVCore::setSpeed(double value) {
    if (video_speed != value) command_async("set", "speed", value);
}

void MPVCore::setAspect(const std::string &value) {
    MPVCore::VIDEO_ASPECT = value;
    video_aspect          = aspectConverter(MPVCore::VIDEO_ASPECT);
    if (value == "-2") {
        // 拉伸全屏
        command_async("set", "keepaspect", "no");
        command_async("set", "video-aspect-override", "-1");
        command_async("set", "panscan", "0.0");
    } else if (value == "-3") {
        // 裁剪填充
        command_async("set", "keepaspect", "yes");
        command_async("set", "video-aspect-override", "-1");
        command_async("set", "panscan", "1.0");
    } else {
        // 指定比例
        command_async("set", "keepaspect", "yes");
        command_async("set", "video-aspect-override", MPVCore::VIDEO_ASPECT);
        command_async("set", "panscan", "0.0");
    }
}

void MPVCore::setMirror(bool value) {
    MPVCore::VIDEO_MIRROR = value;
#ifndef BOREALIS_USE_GXM
    command_async("set", "vf", value ? "hflip" : "");
    setHwdecCopyMode(value);
#endif
}

void MPVCore::setHwdecCopyMode(bool value) {
    // 如果正在使用硬解，那么将硬解更新为 auto-copy，避免直接硬解因为不经过 cpu 处理导致镜像翻转、滤镜无效
    if (MPVCore::HARDWARE_DEC) {
        std::string hwdec = value ? "auto-copy" : MPVCore::PLAYER_HWDEC_METHOD;
        command_async("set", "hwdec", hwdec);
        brls::Logger::info("MPV hardware decode: {}", hwdec);
    }
}

void MPVCore::setBrightness(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_BRIGHTNESS = value;
    if (video_brightness != value) command_async("set", "brightness", value);
}

void MPVCore::setContrast(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_CONTRAST = value;
    if (video_contrast != value) command_async("set", "contrast", value);
}

void MPVCore::setSaturation(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_SATURATION = value;
    if (video_saturation != value) command_async("set", "saturation", value);
}

void MPVCore::setGamma(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_GAMMA = value;
    if (video_gamma != value) command_async("set", "gamma", value);
}

void MPVCore::setHue(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_HUE = value;
    if (video_hue != value) command_async("set", "hue", value);
}

int MPVCore::getBrightness() const { return video_brightness; }

int MPVCore::getContrast() const { return video_contrast; }

int MPVCore::getSaturation() const { return video_saturation; }

int MPVCore::getGamma() const { return video_gamma; }

int MPVCore::getHue() const { return video_hue; }

// todo: remove these sync function
std::string MPVCore::getString(const std::string &key) {
    char *value = nullptr;
    mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_STRING, &value);
    if (!value) return "";
    std::string result = std::string{value};
    mpvFree(value);
    return result;
}

double MPVCore::getDouble(const std::string &key) {
    double value = 0;
    mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_DOUBLE, &value);
    return value;
}

int64_t MPVCore::getInt(const std::string &key) {
    int64_t value = 0;
    mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_INT64, &value);
    return value;
}

std::unordered_map<std::string, mpv_node> MPVCore::getNodeMap(const std::string &key) {
    mpv_node node;
    std::unordered_map<std::string, mpv_node> nodeMap;
    if (mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_NODE, &node) < 0) return nodeMap;
    if (node.format != MPV_FORMAT_NODE_MAP) return nodeMap;
    // todo: 目前不要使用 mpv_node中有指针的部分，因为这些内容指向的内存会在这个函数结束的时候删除
    for (int i = 0; i < node.u.list->num; i++) {
        char *nodeKey = node.u.list->keys[i];
        if (nodeKey == nullptr) continue;
        nodeMap.insert(std::make_pair(std::string{nodeKey}, node.u.list->values[i]));
    }
    mpvFreeNodeContents(&node);
    return nodeMap;
}

double MPVCore::getPlaybackTime() const { return playback_time; }

std::string MPVCore::colorSwapShaderPath() {
#if defined(PS5_NATIVE_APP) && !defined(WILIWILI_SOFTWARE_RENDER)
    return ProgramConfig::instance().getConfigDir() + "/rb-swap.hook";
#else
    return std::string();
#endif
}

void MPVCore::disableDimming(bool disable) {
    brls::Logger::info("disableDimming: {}", disable);
    brls::Application::getPlatform()->disableScreenDimming(disable, "Playing video", APPVersion::getPackageName());
    static bool deactivationAvailable = ProgramConfig::instance().getSettingItem(SettingItem::DEACTIVATED_TIME, 0) > 0;
    if (deactivationAvailable) {
        brls::Application::setAutomaticDeactivation(!disable);
    }
}

void MPVCore::setShader(const std::string &profile, const std::string &shaders,
                        const std::vector<std::vector<std::string>> &settings, bool reset) {
    brls::Logger::info("Set shader [{}]: {}", profile, shaders);

    // 如果之前设置的shader包含mpv配置，就需要重置一下
    if (!currentSetting.empty() && reset) clearShader(false);

    currentShaderProfile = profile;
    currentShader        = shaders;
    currentSetting       = settings;

    // 设置着色器（内置的颜色交换必须保留在最前面，见 init() 的说明）
    std::string builtin = MPVCore::colorSwapShaderPath();
    if (!shaders.empty() || !builtin.empty())
        command_async("no-osd", "change-list", "glsl-shaders", "set",
                      builtin.empty() ? shaders : (shaders.empty() ? builtin : builtin + "," + shaders));

    // 设置mpv配置
    for (auto &setting : settings) {
        _command_async(setting);
    }

    // 显示通知
    if (reset) brls::Application::notify(profile);
}

void MPVCore::clearShader(bool showHint) {
    brls::Logger::info("Clear shader");

    // 如果当前不涉及mpv配置修改，就无需重置
    bool reset = !currentSetting.empty();

    currentShader.clear();
    currentShaderProfile.clear();
    currentSetting.clear();

    // 清空着色器（内置的颜色交换着色器保留）
    std::string builtin = MPVCore::colorSwapShaderPath();
    if (builtin.empty())
        command_async("no-osd", "change-list", "glsl-shaders", "clr", "");
    else
        command_async("no-osd", "change-list", "glsl-shaders", "set", builtin);

    // 重置mpv配置
    if (reset) MPVCore::instance().restart();

    // 显示通知
    if (showHint) brls::Application::notify("Clear profile");
}

void MPVCore::showOsdText(const std::string &value, int d) { command_async("show-text", value, d); }

void MPVCore::_command_async(const std::vector<std::string> &commands) {
    std::vector<const char *> res;
    res.reserve(commands.size() + 1);
    for (auto &i : commands) {
        res.emplace_back(i.c_str());
    }
    res.emplace_back(nullptr);
    mpvCommandAsync(mpv, 0, res.data());
}