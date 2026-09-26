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

    /* 照抄 EVO-PLAYER-PS5 的真机可用实现（SoundEffectEngine.cpp）：
     *   sceAudioOutInit();
     *   handle = sceAudioOutOpen(0xFF, 0, 0, 256, 48000, 1);
     *   参数含义：userId=0xFF、type=0、index=0、grain=256 帧、48000 Hz、param=1 表示 S16 立体声。
     *   句柄判定是 >= 1（0x20000000 这类是有效句柄，不是错误码）；
     *   sceAudioOutOutput(handle, block) 阻塞到该块播完。 */
    int rc = sceAudioOutInit();
    snprintf(line, sizeof(line), "audio: init rc=%d", rc);
    wiliwili_boot_log(line);

    int handle = sceAudioOutOpen(0xFF, 0, 0, 256, 48000, 1);
    snprintf(line, sizeof(line), "audio: open handle=%d", handle);
    wiliwili_boot_log(line);
    if (handle < 1) return;

    /* 256 帧立体声 S16 = 1024 字节/块；每块约 5.33 ms，180 块 ≈ 1 秒。 */
    short buffer[256 * 2];
    double phase = 0.0;
    const double step = 2.0 * 3.14159265358979 * 1200.0 / 48000.0;
    int ok_chunks = 0;
    int first_rc = 0;
    for (int chunk = 0; chunk < 180; ++chunk) {
        for (int i = 0; i < 256; ++i) {
            short sample      = (short)(0.25 * 32767.0 * ((phase < 3.14159265358979) ? 1.0 : -1.0));
            buffer[i * 2]     = sample;
            buffer[i * 2 + 1] = sample;
            phase += step;
            if (phase >= 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
        }
        int out_rc = sceAudioOutOutput(handle, buffer);
        if (chunk == 0) first_rc = out_rc;
        /* EVO 的可用实现只把"负数"当失败（正数是正常返回，例如本次的 256）。 */
        if (out_rc < 0) {
            snprintf(line, sizeof(line), "audio: output failed at chunk %d rc=%d", chunk, out_rc);
            wiliwili_boot_log(line);
            break;
        }
        ++ok_chunks;
    }
    snprintf(line, sizeof(line), "audio: chunks=%d first_rc=%d", ok_chunks, first_rc);
    wiliwili_boot_log(line);

    rc = sceAudioOutClose(handle);
    snprintf(line, sizeof(line), "audio: close rc=%d", rc);
    wiliwili_boot_log(line);
    wiliwili_boot_log("audio: probe done");
}
