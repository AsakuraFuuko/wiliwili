/*
 * SceAudioOut 音频探针（原生标题 app slot 专用）。
 *
 * 目的：P2d 的门槛——确认自管播放器能自己出声。SDL2 在 native 构建里没有音频后端
 * （实测 SDL_InitSubSystem(SDL_INIT_AUDIO) = -1、driver=(none)），所以音频必须直接接
 * 系统接口。这里用经典 API（桩里 6 个符号齐全）：Init → Open → Output(正弦) → Close。
 *
 * 触发：assets/wiliwili-options.txt 的 WILIWILI_TEST_AUDIO=1。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void wiliwili_boot_log(const char *message);

int sceSysmoduleLoadModule(unsigned short id);
int sceUserServiceGetInitialUser(int *userId);
int sceAudioOutArbitrationInitialize(void);
int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int handle, const void *ptr);
int sceAudioOutClose(int handle);
int sceAudioOutSetVolume(int handle, int flag, const int *volumes);
int sceAudioOutGetPortState(int handle, void *state);

#define AUDIO_FREQ 48000
#define AUDIO_FRAMES 1024

void wiliwili_audio_probe(void) {
    char line[192];
    wiliwili_boot_log("audio: enter");

    /* 模块 ID 表：0x01 = libSceAudioOut（0xcf = 207 = libSceVideodec2，与硬解一致）。
     * 之前从没加载过音频模块——这很可能就是经典接口一直被拒的原因。 */
    int module_rc = sceSysmoduleLoadModule(0x01);
    snprintf(line, sizeof(line), "audio: sysmodule1 rc=%d", module_rc);
    wiliwili_boot_log(line);

    int rc = sceAudioOutInit();
    snprintf(line, sizeof(line), "audio: init rc=%d", rc);
    wiliwili_boot_log(line);

    /* 音频需要真实用户上下文：先取初始用户（payload 里这一步会失败）。 */
    int user_id = -1;
    int user_rc = sceUserServiceGetInitialUser(&user_id);
    snprintf(line, sizeof(line), "audio: initial_user rc=%d id=%d", user_rc, user_id);
    wiliwili_boot_log(line);

    int arb_rc = sceAudioOutArbitrationInitialize();
    snprintf(line, sizeof(line), "audio: arbitration rc=%d", arb_rc);
    wiliwili_boot_log(line);

    /* 参数扫描：模拟器实现里 param 是"编码声道数/格式"的，且 grain 必须是 256 的整数倍。
     * 逐个组合打 rc，命中就直接出声。 */
    int handle = -1;
    const int user_ids[2]   = {user_id, 0xFF};
    const int types[2]      = {0, 1};
    const unsigned lens[4]  = {256, 512, 1024, 2048};
    const unsigned params[8] = {1, 2, 0, 3, 4, 0x0001, 0x0201, 0x1002};
    for (int u = 0; u < 2 && handle < 0; ++u) {
        for (int t = 0; t < 2 && handle < 0; ++t) {
            for (int l = 0; l < 4 && handle < 0; ++l) {
                for (int p = 0; p < 8 && handle < 0; ++p) {
                    int candidate = sceAudioOutOpen(user_ids[u], types[t], 0, lens[l], AUDIO_FREQ, params[p]);
                    /* 只认"像句柄"的小正整数：0x20000000 之类是错误码，上一轮就是被它骗停了。 */
                    if (candidate > 0 && candidate < 0x10000) {
                        handle = candidate;
                        snprintf(line, sizeof(line), "audio: HIT user=%d type=%d len=%u param=0x%x -> %d",
                                 user_ids[u], types[t], lens[l], params[p], candidate);
                    } else {
                        snprintf(line, sizeof(line), "audio: miss user=%d type=%d len=%u param=0x%x rc=%d",
                                 user_ids[u], types[t], lens[l], params[p], candidate);
                    }
                    wiliwili_boot_log(line);
                }
            }
        }
    }
    if (handle < 0) return;

    short buffer[AUDIO_FRAMES * 2];
    double phase = 0.0;
    const double step = 2.0 * 3.14159265358979 * 1200.0 / (double)AUDIO_FREQ; /* 1.2 kHz 提示音 */
    int chunks = AUDIO_FREQ / AUDIO_FRAMES;                                   /* 约 1 秒 */
    int ok_chunks = 0;
    for (int chunk = 0; chunk < chunks; ++chunk) {
        for (int i = 0; i < AUDIO_FRAMES; ++i) {
            short sample   = (short)(0.25 * 32767.0 * ((phase < 3.14159265358979) ? 1.0 : -1.0));
            buffer[i * 2]  = sample;
            buffer[i * 2 + 1] = sample;
            phase += step;
            if (phase >= 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
        }
        if (sceAudioOutOutput(handle, buffer) != 0) break;
        ++ok_chunks;
    }
    snprintf(line, sizeof(line), "audio: output chunks=%d/%d", ok_chunks, chunks);
    wiliwili_boot_log(line);

    rc = sceAudioOutClose(handle);
    snprintf(line, sizeof(line), "audio: close rc=%d", rc);
    wiliwili_boot_log(line);
    wiliwili_boot_log("audio: probe done");
}
