/*
 * PS5 原生音频探针（sceAudioOut2*，原生标题 app slot 专用）。
 *
 * 背景：经典 sceAudioOutOpen 在本机被拒（0x80310711，见 notes/02），SDL2 又没有音频后端，
 * 所以走 PS5 原生的 AudioOut2（环形 grain 推送模型，桩里符号齐全）。
 *
 * 流程仿照硬解那套（先 Query 再分配再 Create），逐调用打点、按 rc 收敛。
 * 结构体/签名参考：SharpProspero Interop/Audio/AudioOut2.cs、AnyPS5 libSceAudioOut/src/AudioOut2*.cpp。
 * 触发：assets/wiliwili-options.txt 的 WILIWILI_TEST_AUDIO2=1。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

extern void wiliwili_boot_log(const char *message);

int sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd, size_t length, size_t alignment,
                                  int32_t memoryType, int64_t *physicalAddr);
int32_t sceKernelMapDirectMemory(void **addr, size_t length, int32_t prot, int32_t flags, int64_t physicalAddr,
                                 size_t alignment);
int64_t sceKernelGetDirectMemorySize(void);

int sceSysmoduleLoadModule(unsigned short id);
/* 带参数的仲裁：无参版本会崩，说明它有入参（属性表 + 计数，与 SetAttributes 同形）。 */
int sceAudioOut2ArbitrationInitialize(const void *attributes, unsigned int numAttributes);
int sceAudioOut2ContextResetParam(void *params);
int sceAudioOut2ContextQueryMemory(const void *params, size_t *memorySize);
int sceAudioOut2ContextCreate(const void *params, void *buffer, size_t bufferSize, uint64_t *context);
int sceAudioOut2ContextDestroy(uint64_t context);
int sceAudioOut2PortCreate(uint64_t context, const void *params, uint64_t *port);
int sceAudioOut2PortDestroy(uint64_t port);
int sceAudioOut2ContextBedWrite(uint64_t context, const void *data, size_t size);
int sceAudioOut2ContextPush(uint64_t context, uint32_t blocking);
int sceAudioOut2ContextAdvance(uint64_t context);
int sceAudioOut2ContextGetQueueLevel(uint64_t context, uint32_t *queued, uint32_t *available);

/* 结构体：注意 AudioOut2 的结构体没有 size 首字段（与 videodec2 的约定不同）。 */
typedef struct {
    uint32_t MaxPorts;
    uint32_t MaxObjectPorts;
    uint32_t GuaranteeObjectPorts;
    uint32_t QueueDepth;
    uint32_t NumGrains;
    uint32_t Flags;
    uint32_t Reserved[10];
} ContextParam;

/* 布局来自 PS5PCEM 的 HLE（src/hle/libs/audio.zig 的 AudioOut2PortParam）：
 * 开头是 port_type(u16) + padding(u16)，然后才是 format/freq/flags/user_handle。 */
typedef struct {
    uint16_t PortType;
    uint16_t Padding;
    uint32_t DataFormat;
    uint32_t SamplingFrequency;
    uint32_t Flags;
    uintptr_t UserHandle;
    uint32_t Reserved[10];
} PortParam;

static void log1(const char *fmt, long a) {
    char line[160];
    snprintf(line, sizeof(line), fmt, a);
    wiliwili_boot_log(line);
}

static void log2(const char *fmt, long a, long b) {
    char line[192];
    snprintf(line, sizeof(line), fmt, a, b);
    wiliwili_boot_log(line);
}

void wiliwili_audio2_probe(void) {
    wiliwili_boot_log("a2: enter");

    /* 两个会让进程直接退出的调用都跳过：无参 sceAudioOut2ArbitrationInitialize()（要参数）
     * 与 sceAudioOut2ContextResetParam()（结构体约定不符）。改为手工填参数。 */
    {
        char line[128];
        int module_rc = sceSysmoduleLoadModule(0x01); /* libSceAudioOut */
        snprintf(line, sizeof(line), "a2: sysmodule1 rc=%d", module_rc);
        wiliwili_boot_log(line);
    }
    {
        char line[128];
        int arb_rc = sceAudioOut2ArbitrationInitialize(NULL, 0);
        snprintf(line, sizeof(line), "a2: arbitration(NULL,0) rc=%d", arb_rc);
        wiliwili_boot_log(line);
    }
    wiliwili_boot_log("a2: manual params");
    int rc = 0;

    ContextParam params;
    memset(&params, 0, sizeof(params));
    params.MaxPorts           = 2;
    params.MaxObjectPorts     = 0;
    params.GuaranteeObjectPorts = 0;
    params.QueueDepth         = 2;   /* 允许在飞两个 grain */
    params.NumGrains          = 2;
    params.Flags              = 0;
    log2("a2: params ports=%d grains=%d", (long)params.MaxPorts, (long)params.NumGrains);

    size_t memory_size = 0;
    rc                 = sceAudioOut2ContextQueryMemory(&params, &memory_size);
    log2("a2: query_memory rc=%d size=0x%lx", rc, (long)memory_size);
    if (rc != 0 || memory_size == 0) return;

    uint64_t limit   = (uint64_t)sceKernelGetDirectMemorySize();
    size_t aligned   = (size_t)((memory_size + 0x3FFF) & ~0x3FFFuLL);
    int64_t physical = 0;
    if (sceKernelAllocateDirectMemory(0, (int64_t)limit, aligned, 0x4000, 12, &physical) != 0) {
        wiliwili_boot_log("a2: allocate_direct failed");
        return;
    }
    void *buffer = NULL;
    if (sceKernelMapDirectMemory(&buffer, aligned, 0x33, 0, physical, 0x4000) != 0) {
        wiliwili_boot_log("a2: map_direct failed");
        return;
    }
    log1("a2: context memory ok %d", buffer != NULL);

    uint64_t context = 0;
    rc               = sceAudioOut2ContextCreate(&params, buffer, aligned, &context);
    log2("a2: context_create rc=%d ctx=%d", rc, context != 0);
    if (rc != 0) return;

    /* 端口：DataFormat 依次试 0/1/2（枚举里前几个是浮点通道数），采样率固定 48000（主输出硬约束）。 */
    uint64_t port = 0;
    PortParam port_params;
    memset(&port_params, 0, sizeof(port_params));
    port_params.PortType          = 0;
    port_params.DataFormat        = 1; /* 立体声（枚举里 1 = 两声道） */
    port_params.SamplingFrequency = 48000;
    port_params.Flags             = 0;
    int port_rc              = sceAudioOut2PortCreate(context, &port_params, &port);
    log2("a2: port_create rc=%d port=%d", (long)port_rc, (long)(port != 0));

    /* 输出一段正弦：float 立体声（DataFormat 若是 float 就按 float 填），48000 Hz。 */
    {
        const int frames = 1024; /* grain 需为 256 的整数倍且在 256..2048 内 */
        float tone[1024 * 2];
        double phase = 0.0;
        const double step = 2.0 * 3.14159265358979 * 1200.0 / 48000.0;
        for (int i = 0; i < frames; ++i) {
            tone[i * 2]     = (float)(0.2 * ((phase < 3.14159265358979) ? 1.0 : -1.0));
            tone[i * 2 + 1] = tone[i * 2];
            phase += step;
            if (phase >= 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
        }

        int pushed = 0;
        for (int block = 0; block < 40; ++block) {
            int write_rc = sceAudioOut2ContextBedWrite(context, tone, sizeof(tone));
            if (write_rc != 0) {
                log1("a2: bed_write rc=%d", write_rc);
                break;
            }
            int push_rc = sceAudioOut2ContextPush(context, 1 /* blocking */);
            if (push_rc != 0) {
                log1("a2: push rc=%d", push_rc);
                break;
            }
            int advance_rc = sceAudioOut2ContextAdvance(context);
            if (advance_rc != 0) {
                log1("a2: advance rc=%d", advance_rc);
                break;
            }
            ++pushed;
        }
        log1("a2: pushed blocks=%d", pushed);

        uint32_t queued = 0, available = 0;
        if (sceAudioOut2ContextGetQueueLevel(context, &queued, &available) == 0)
            log2("a2: queue queued=%d available=%d", (long)queued, (long)available);
    }

    if (port) sceAudioOut2PortDestroy(port);
    sceAudioOut2ContextDestroy(context);
    wiliwili_boot_log("a2: probe done");
}
