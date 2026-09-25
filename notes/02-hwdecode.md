# 02 — sceVideodec2 硬解集成方案（含解码帧的呈现）

**范围**：在带 `TITLE_ID` 的**注册应用槽**（我们的 `PPSA99010`（`/system_ex/app/` 真实目录）/ `PPSA99012`（ffpkg + ShadowMountPlus））里用 `sceVideodec2`（VCN 硬解）解 H.264 / HEVC / VP9，并把解码出来的 NV12/P010 帧**呈现**给现有 UI（nanovg + OSMesa/llvmpipe + `ps5-payload-dev/SDL` 的 ps5 视频驱动）。

**本文只写笔记，未修改任何源码，未构建、未跑测试。**

**引用约定**：每条结论给 `项目:路径:符号/行`。外部项目全部为 **GPL-3.0**（与 wiliwili 一致，可直接借鉴代码）：
`sainsaji/EVO-PLAYER-PS5`、`blackbearreloaded/ProsperoLight`、`blackbearreloaded/ProsperoTV`、`blackbearreloaded/ps5-native-app-boilerplate`。
（GPL-2.0 的 `shadPS4` / `AnyPS5` 等本文未引用代码，仅在前批调研中作语义参考。）

---

## 0. 结论

1. **可行，且已被两个独立项目在真机验证**。`EVO-PLAYER-PS5` 在 `PPSA99039`（fake-signed、ShadowMountPlus、带 `param.json`）里拿到 `VDEC self-test: HARDWARE DECODE OK`（4K H.264 实时播放）；`ProsperoLight` 在 `PPSA99002` 上以生产级用法长跑 Moonlight 流。两个项目的结论一致：**`sceVideodec2Decode` 的 errno 5200 是"进程上下文"门，不是驱动/签名门**，我们原生标题线正是它们所需的那种进程。
2. **完整 bring-up 序列可以照抄**（§2）。三个项目的字段口径有差异（`prot`、`max_dpb_frames`、`pipeline_depth`、池大小），差异点已在 §2.4 列表说明，均可硬件验证过，按我们的场景选一套即可。
3. **呈现两条路**（§4）：
   - **(A) AGC 视频管线**：`sceVideodec2` → NV12 指针 → AGC 管线 `VIDEO_NV12` 直接采样（**零拷贝**）+ `sceAgcDcbSetFlip`。成本极低（EVO 实测 4K 合成 **982 µs/帧**），但**等于接管 VideoOut 与整条渲染链**（现有 nanovg/Mesa 栈要重写或并存），工作量在"重写渲染层"量级。
   - **(B) NV12 → 现有 llvmpipe/nanovg 栈**：两条子路 —— **B1** 上传两张纹理 + GLSL 做 YUV→RGB（改动最小，GPU 代价与今天 mpv 的视频 pass 同量级）；**B2** CPU 把 NV12 转成 ABGR8888 后**直写 SDL 窗口表面**（EVO 实测 1080p 融合转换器 **0.98–2.11 ms/帧**，4K **7.4–11 ms/帧**），**同时省掉解码与渲染**，是当前架构下唯一能真正提速的组合。
   - **推荐先做 (B)**，其中先做 B1（零正确性风险）验证链路，再评估 B2。(A) 作为后续"原生线 2.0 终极形态"，不应与首批接入混做。
4. **与 wiliwili 播放链的接法很干净**（§5）：B 站 DASH 接口本来就把 **视频轨与音频轨分成两个 URL**（`dash.video[]` / `dash.audio[]`），所以让 **mpv 只做"音频 + 时钟"**（只喂音频 URL），视频完全自建（ffmpeg demux → `h264/hevc_mp4toannexb` → videodec2 → 呈现），弹幕/OSD/进度条一行都不用改（它们只依赖 `MPVCore::playback_time`）。**必须绕开的只有 mpv 的视频解码与视频渲染两条**。
5. **收益要说实话**：我们当前播放瓶颈是 **llvmpipe 渲染**（`decoder-drops=0`，丢帧在显示端，见 `run-continuation/ps5-port-status.md`）。硬解本身**不会**让 1080p H.264 更流畅；它带来的是 (a) **4K / HEVC / 10-bit 的播放能力**（软解那本账（~450 MB flexible 上限）根本付不起），(b) **释放 CPU**（llvmpipe 与 UI 抢核），(c) 配合 B2 时**顺带砍掉 llvmpipe 的视频 pass（两遍全屏）**——这一条才是帧率收益的来源。

---

## 1. 前置条件与能力边界（直接引用，不重复验证）

| 条件 | 状态 | 出处 |
|---|---|---|
| 必须是"注册应用槽"（`TITLE_ID` + `param.json` + 沙箱），payload（elfldr/hbldr）不行 | 我们 `PPSA99010/99012` 满足 | `EVO-PLAYER-PS5:docs/evo-pro/videodec2-abi.md:§1`（"The one thing that mattered"）、`docs/evo-pro/phase-1b-app-module.md:26` |
| 系统 PRX **不能** `sceKernelLoadStartModule`，必须走**链接期 NEEDED 导入**（positional PRX 桩） | 我们的 native 线已有这套（AGC 就是这么做的） | `EVO-PLAYER-PS5:tools/native-app/stubs/prx/README.md`（"a fake-signed app module cannot `sceKernelLoadStartModule` a system PRX it did not declare NEEDED"） |
| `/system/priv/lib` 下的模块（`libSceVcodec` 等）**假签名应用不可加载**，失败是"应用直接不启动" | 与 videodec2 无关，但同一目录不要碰 | 同上 README 末节（测试日期 2026-09-21） |
| `libSceVideodec2.sprx` 位于 `/system/common/lib`，**可以**通过 NEEDED 自动加载 | `EVO` 与 `ProsperoLight` 均如此并在真机解码成功 | 同上 + `EVO-PLAYER-PS5:docs/evo-pro/status.md:400-410` |
| payload SDK 有 `libkernel.so` / `libSceSysmodule.so` 桩，但**没有** `libSceVideodec2`/`libSceAvPlayer`/`libSceAgc` 桩 | `/opt/ps5-payload-sdk/target/lib` 实测 55 个桩里无这三个 | 本机 `ls /opt/ps5-payload-sdk/target/lib` |
| 固件版本 | EVO 的硬件验证在 **12.70**；我们主机是 **12.00**，属"同族但未验证" | `docs/evo-pro/videodec2-abi.md:3`（"on a PS5 (12.70)"） |

**能力边界（EVO 实测）**：H.264 8-bit ≤4K 实时；HEVC Main 4K、HEVC Main10 1080p、VP9 Profile 0 4K 均可用（`docs/evo-pro/status.md:413-440`）；AV1 无路，10-bit 需 P010 呈现路径。

---

## 2. Bring-up 序列（照做版）

### 2.1 三条权威出处

| 出处 | 形式 | 权威度 |
|---|---|---|
| `EVO-PLAYER-PS5:docs/evo-pro/videodec2-abi.md:44-107` | **逐条编号的完整序列 + prot 表**（"hardware-verified 2026-09-01"） | 最高（转写自 ProsperoLight 并在真机跑通一个 IDR） |
| `ProsperoLight:src/moonlight_stream.cpp:2899-3030` | 生产级 C++ 代码（Moonlight 长跑） | 最高（可照抄，字段具体） |
| `EVO-PLAYER-PS5:projects/evoplayer/media/src/evo_vdec_native.c:432-540`（`slot_bringup`） | 可移植 C 后端（多 codec 常驻） | 高（但把 `prot` 全改成 0x33，且**不**每 AU `Flush`） |
| `ProsperoTV:src/iptv_native_backend.c:1030-1150`（`initialize_video`） | 第四份独立实现 + **ABI `_Static_assert`** | 高（DPB 策略最完整） |

### 2.2 序列（可直接照抄的伪代码）

> 单位与对齐：**所有尺寸在分配前都 `align16k(v) = (v + 0x3fff) & ~0x3fff`**（`ProsperoLight:src/moonlight_stream.cpp:906`、`evo_vdec_native.c:225`）。
> 每个 SCE 结构体：**先 `memset(0)`，再 `.size = sizeof(struct)`**（三份实现一致；`sce_videodec2.h` 注释："the service rejects a wrong size"）。

```c
/* 0) 尽早执行：main 早期、任何 credential 切换之前。
 *    ProsperoLight 把它放在窗口/连接建立之前；EVO 强调必须在
 *    evo_jailbreak_self() 之前（自解越权后 sceSysmoduleLoadModule(207) 返回
 *    ESDKVERSION，模块再也装不完）—— EVO-PLAYER-PS5:projects/evoplayer/media/src/evo_vdec_native.h:24-31。
 *    我们 native 线不做 self-unjail，风险低，但仍建议启动期一次性完成。 */
sceSysmoduleLoadModule(207);                     /* SCE_SYSMODULE_VIDEODEC2 */

int64_t dm_limit = sceKernelGetDirectMemorySize();

/* 1) compute 路径 */
SceVideodec2ComputeMemoryInfo cm = { .size = sizeof cm };
sceVideodec2QueryComputeMemoryInfo(&cm);
size_t cm_bytes = align16k(cm.cpu_gpu_size);
alloc_direct(cm_bytes, /*prot*/ 0x33, dm_limit, &cm_start, &cm.cpu_gpu);
cm.cpu_gpu_size = cm_bytes;                      /* 必须回写对齐后的值 */

SceVideodec2ComputeConfigInfo cc = { .size = sizeof cc };  /* pipe_id=queue_id=0 */
sceVideodec2AllocateComputeQueue(&cc, &cm, &compute_queue);

/* 2) 解码器配置 */
SceVideodec2DecoderConfigInfo cfg = {
  .size = sizeof cfg,
  .resource_type = 1,                 /* compute 路径 */
  .codec_type    = <1 / 974921 / 2382845>,   /* AVC / HEVC / VP9，见 §6.2 */
  .profile       = <见 §6.2>,
  .max_level     = <见 §6.2>,
  .max_width     = roundup16(w),      /* 1920 / 2560 / 3840 */
  .max_height    = roundup16(h),      /* 1088 / 1440 / 2176 */
  .max_dpb_frames= <4 | -1 | 见 §2.4>,
  .pipeline_depth= 1,
  .compute_queue = (uint64_t)compute_queue,
  .cpu_affinity  = 0x3f,              /* core 0-5 */
  .cpu_priority  = 700,
  .optimize_progressive = 1,          /* H.264 广播流用 0，见 §2.4 */
};
SceVideodec2DecoderMemoryInfo mem = { .size = sizeof mem };
sceVideodec2QueryDecoderMemoryInfo(&cfg, &mem);   /* 出 cpu/gpu/cpu_gpu/max_frame_size */

/* 3) CPU 工作区走 flexible memory（只可 CPU 读写，不可 GPU 采样） */
size_t cpu_sz = align16k(mem.cpu_size);
sceKernelAvailableFlexibleMemorySize(&avail);     /* 建议：不足就早退，不要硬上 */
sceKernelMapNamedFlexibleMemory(&mem.cpu, cpu_sz, 0x03, 0, "WiliVdecCpu");

/* 4) 三块 direct memory + 两个池 */
size_t gpu_sz     = align16k(mem.gpu_size);
size_t cpugpu_sz  = align16k(mem.cpu_gpu_size);
size_t frame_sz   = align16k(mem.max_frame_size);
size_t au_slot    = 0x800000;                     /* 8 MiB（可缩，见 §6.4） */
size_t au_pool    = au_slot    * AU_SLOTS;        /* 3 ~ 4 */
size_t frame_pool = frame_sz   * FRAME_SLOTS;     /* 3 ~ 8 */

alloc_direct(gpu_sz,    0x32, ...);  mem.gpu = ...;      mem.gpu_size = gpu_sz;
if (cpugpu_sz) { alloc_direct(cpugpu_sz, 0x33, ...); mem.cpu_gpu = ...; mem.cpu_gpu_size = cpugpu_sz; }
alloc_direct(au_pool,   0x32, ...);                       /* AU 池 */
alloc_direct(frame_pool,0x32, ...);                       /* 帧池（呈现方式决定 prot，见 §2.3） */

/* 5) 建、复位 */
sceVideodec2CreateDecoder(&cfg, &mem, &decoder);
sceVideodec2Reset(decoder);
```

```c
/* alloc_direct：三份实现完全一致（type=12，对齐 0x4000） */
static int alloc_direct(size_t size, int prot, int64_t limit, int64_t *start, void **addr) {
    int rc = sceKernelAllocateDirectMemory(0, limit, size, 0x4000, 12, start);
    if (rc == 0) rc = sceKernelMapDirectMemory(addr, size, prot, 0, *start, 0x4000);
    return rc;
}
```
出处：`ProsperoLight:src/moonlight_stream.cpp:964-971`；`EVO-PLAYER-PS5:projects/evoplayer/media/src/evo_vdec_native.c:351-357`；`ProsperoTV:src/iptv_native_backend.c`（同名函数）。

**拆除顺序**（照抄 `videodec2-abi.md:§4`，所有 rc 忽略）：
`DeleteDecoder` → 释放 frame_pool / au_pool / mem.cpu_gpu / mem.gpu（`munmap` + `sceKernelReleaseDirectMemory`）→ `sceKernelReleaseFlexibleMemory(mem.cpu)` + `munmap` → `ReleaseComputeQueue` → 释放 compute cpu_gpu → `sceSysmoduleUnloadModule(207)`。

### 2.3 prot 语义与"该用哪个"（**最容易踩错的地方**）

| prot | 含义 | 出处 |
|---|---|---|
| `0x33` | CPU 读+写 **且** GPU 全部访问 | `videodec2-abi.md:103` |
| `0x32` | CPU 只写 + GPU 全部访问 | `videodec2-abi.md:104` |
| `0x03`（flexible，另一套 API） | 仅 CPU 读写，**不可被 GPU 采样** | `videodec2-abi.md:105`；EVO 的成因描述见 `docs/evo-pro/status.md:188-200`（"NV12 staging…flexible memory（PROT_RW only — not GPU-samplable）"） |

三个项目实际用法**不一致**，且都硬件验证过：

| 用途 | EVO（`evo_vdec_native.c:432-540`） | ProsperoLight（`moonlight_stream.cpp:2918-3005`） | ProsperoTV（`iptv_native_backend.c:1110-1135`） |
|---|---|---|---|
| compute `cpu_gpu` | 0x33 | 0x33 | 0x33 |
| decoder `mem.gpu` | **0x33** | 0x32 | 0x32 |
| decoder `cpu_gpu` | 0x33 | 0x33 | 0x33 |
| AU 池 | **0x33** | 0x32 | 0x32 |
| 帧池 | **0x33** | 0x32 | 0x32 |
| decoder `cpu` | flexible 0x03 | flexible 0x03 | flexible 0x03 |

**决策规则（我们照此选）**：
- 呈现走 **(A) 纯 GPU 采样** → 帧池 `0x32` 就够（ProsperoLight/ProsperoTV 的做法，CPU 不需要读）。
- 呈现走 **(B1/B2) CPU 要读像素**（上传纹理 / CPU 转换）→ 帧池**必须 `0x33`**（EVO 就是因为自己有 CPU 转换回退才全用 0x33；它的 `ro_harvest` 直接 `memcpy` 读 `out->buffer`，见 `evo_vdec_native.c:824-870`）。
- AU 池永远只有 CPU 写，`0x32` 语义上最贴切，用 `0x33` 也无害（EVO 实测）。

> 另有一条硬约束：**decoder 的 `mem.cpu`（flexible）里的帧不能被 GPU 采样**。若曾想把解码输出放 flexible 省 direct memory，必须放弃（`status.md:188-200`）。

### 2.4 字段口径分歧（照抄前先选一套）

| 字段 | EVO | ProsperoLight | ProsperoTV | 我们的选择 |
|---|---|---|---|---|
| `max_dpb_frames` | `-1`（`SCE_VIDEODEC2_AUTO_FRAMES`，解码器自定尺寸） | `4` | AVC：≤1080p→`16`、≤1440p→`8`、4K→`6`；HEVC/VP9→`4`；HEVC 4K→`6` | **`4`**（最保守、两份生产实现都用 4）；若遇到 `0x811D0302 OVERSIZE_DECODE` 再按 ProsperoTV 的策略抬高，DPB 越大越吃内存 |
| `pipeline_depth` | `4`（每 AU 不 Flush） | `1`（默认；`Makefile:23 DECODER_PIPELINE_DEPTH ?= 1`，`2` 为实验值） | `1` | **`1`**（与 ABI 文档和两份生产实现一致，且内存最省） |
| `optimize_progressive` | `1`（无条件下） | `1` | H.264→`0`，其余→`1` | `1`；若播 B 站直播/隔行 H.264 出现解码异常，改 `0`（ProsperoTV 注释：广播 H.264 常为隔行） |
| `cpu_affinity` / `cpu_priority` | `0x3f` / `700` | `0x3f` / `700`（`Makefile:23-24`，且 `docs/PERFORMANCE_ROUND_3.md:18` 说明只在 700–767、6 核子集内做实验） | `0x3f` / `700` | `0x3f` / `700` |
| AU 池槽数 | `4` × 8 MiB = 32 MiB | `PIPELINE_BUFFER_COUNT = depth+2 = 3` × 8 MiB = 24 MiB | `3` × 8 MiB = 24 MiB | `3` × **2 MiB** = 6 MiB（1080p 的 AU 远小于 2 MiB；4K 用 4 MiB，见 §6.4） |
| 帧池槽数 | `8` | `3`（与 AU 同环，靠 `native_agc_wait_source_idle` 保证 GPU 用完） | `3` | `3`（配合"呈现完成才复用"）；若呈现是异步的，抬高到 6–8 |
| struct 尾部 8 字节 | `u32 check_memory_type; u32 reserved;` | 同 EVO | `u8 optimize_progressive, check_memory_type, reserved0, reserved1; void *extra_config;` | 两者都 `sizeof == 72`，**都置 0** 即可（`ProsperoTV:src/iptv_native_backend.c:123-127` 的 `_Static_assert` 可作编译期护栏） |

### 2.5 易错点汇总（都会以"进程直接死"或"静默错帧"的形式出现）

1. **Sony 代码可能"炸"而不是"返错"**：`codec_type` / `max_level` 写错会直接带走进程，失败上报代码根本执行不到。EVO 因此在**每个调用前**打印一行 breadcrumb（`evo_vdec_native.c:416-427` 的 `STAGE(name)` 宏）。我们照做（写进启动日志/UDP）。
2. **尺寸必须 16K 对齐后再分配**，且 `mem.gpu_size`/`cpu_gpu_size`/`max_frame_size` 都要把**对齐后的值回写**给 `CreateDecoder`（`videodec2-abi.md:§2` 编号 6/7）。
3. **`.size` 必须等于 `sizeof`**，且结构体先清零（未清零的 reserved 字段行为未知）。
4. **`cm.cpu_gpu_size` 的对齐值要回写**后才能 `AllocateComputeQueue`（`videodec2-abi.md:58`）。
5. **`max_width/max_height` 是"解码器尺寸"，在 Create 时固定**：影片中途换分辨率必须重建解码器（`videodec2-abi.md:§6` 第 4 条）。
6. **`Query*MemoryInfo` 的输出是"必需量"，不是建议量**——小于它的分配不要试。
7. **`sceVideodec2Reset` 在开新流时调用**（EVO `open()` 里 `sceVideodec2Reset(n->dec)`），seek 时也要调（EVO `flush()`）。
8. **AU 必须是完整 Annex-B 访问单元**（SPS+PPS+slice…）；fMP4/DASH 的 avcC/hvcC 包装必须过 `h264_mp4toannexb` / `hevc_mp4toannexb`。**seek 后必须重建 bsf**（`av_bsf_flush()` 不会重新注入 SPS/PPS，之后每个 AU 都会以 **`0x811d0303`** 失败直到放弃：`evo_vdec_native.c:1217-1250` 的 #57 注释）。
9. **不要在渲染线程上做 bring-up/decode**（`videodec2-abi.md:§2` 开头："off the render thread"；`hardware-decode-review.md:§7`）。

---

## 3. 每帧 decode

### 3.1 环与提交（depth=1 版本，推荐）

```c
/* AU 池 3 槽，帧池 3 槽，同索引环转（ProsperoLight 的做法） */
unsigned slot = au_index % 3;
memcpy(au_pool + slot*AU_SLOT, annexb_au, au_len);

SceVideodec2InputData in = { .size=sizeof in,
    .au = au_pool + slot*AU_SLOT, .au_size = au_len,
    .pts = pts_us, .dts = UINT64_MAX, .attached = 0 };
SceVideodec2FrameBuffer fb = { .size=sizeof fb,
    .buffer = frame_pool + slot*frame_sz, .buffer_size = frame_sz };
SceVideodec2OutputInfo out = { .size=sizeof out };

int rc = sceVideodec2Decode(dec, &in, &fb, &out);

/* depth == 1：!valid 时**必须** Flush 把这一帧挤出来
 * （videodec2-abi.md:§3；ProsperoLight:src/moonlight_stream.cpp:1272-1289） */
if (rc == 0 && !out.valid) { memset(&out,0,sizeof out); out.size = sizeof out;
                             rc = sceVideodec2Flush(dec, &fb, &out); }
/* 成功判据：rc==0 && out.valid && !out.error && fb.accepted */
if (out.valid) frame = out.buffer;   /* NV12 / P010 */
```

**`pipeline_depth > 1` 的变体**（EVO，吞吐优先）：每次 Decode 后**不要** Flush（会串行化），`!valid` 时直接返回"还要更多输入"，靠一个 4 深重排窗口攒帧，流尾/seek 时用 `sceVideodec2Flush` 循环抽干（`evo_vdec_native.c:947-975` 的 `drain_decoder`、`evo_vdec_native.c:1126-1145` 的 `send(NULL,0)` 分支）。**两套都硬件验证过**；我们先用 depth=1。

**重排/丢帧**：`sceVideodec2Decode` **按解码顺序**输出并原样回传 `pts`（`videodec2-abi.md:§6` 第 1 条），B 帧重排要自己做（ProsperoLight 上游是 Moonlight 的 decoder 单元，EVO 自建 `EVO_VDEC_REORDER_DEPTH=4` 的按 PTS 出队窗口，`evo_vdec_native.c:751-786`）。

### 3.2 输出布局（NV12 / P010）

| 量 | 含义 | 值例（1080p） | 出处 |
|---|---|---|---|
| `out.width` / `out.height` | **编码**亮度尺寸（MB 对齐，高可能 1088） | 1920 × 1088 | self-test 输出行 `out valid=1 error=0 pics=1 1920x1088 pitch=2048 codec=1`（`videodec2-abi.md:33`） |
| `out.pitch` | 亮度行距，单位**采样**（8-bit 时 == 字节数） | 2048（256 对齐） | 同上；ProsperoTV 明确校验 `pitch ≤ ((max_width+255)&~255)` 且为偶数：`iptv_native_backend.c:1380-1392` |
| `out.pitch_bytes` | 字节行距；**P010 时 == `pitch*2`** | 2048 / 4096 | `videodec2-abi.md:§3`；ProsperoLight 校验 `output.pitch_bytes == output.pitch*sizeof(uint16_t)`（`moonlight_stream.cpp:1315`） |
| NV12 布局 | 亮度平面后紧跟**交织 UV 平面**，行距同亮度，高为亮度一半 | `uv = buf + pitch*coded_height` | `evo_vdec_native.c:830-845` |
| 色度起始偏移 | 用 **coded（MB 对齐）高度**，不是显示高度 | 1088 → `uv = buf + 2048*1088` | `videodec2-abi.md:§3`；`pp_frame.coded_height` 就是为此新增（`docs/evo-pro/native-decode-plan.md:384-388`） |
| 单帧字节数 | `pitch * (height + (height+1)/2) * (10bit?2:1)` | 2048×(1088+544)=3,342,336 B | `ProsperoTV:src/iptv_native_backend.c:1355-1363` |
| 一帧有效边界 | `out.buffer` 指向**帧池槽内** | — | 用 `frame_is_in_pool()` 校验：`ProsperoLight:src/moonlight_stream.cpp:956-962` |

### 3.3 "输出有效性"判据（照抄 ProsperoTV，最完整）

```c
/* ProsperoTV:src/iptv_native_backend.c:1366-1410 —— 逐条 reject_flags，任一命中即判失败 */
if (!out.valid)                       reject;   /* 未出帧 */
if (out.error)                        reject;
if (!frame.accepted)                  reject;   /* 解码器没接收这个槽 */
if (out.picture_count != 1)           reject;   /* 正常帧就是 1 */
if (out.codec != cfg.codec_type)      reject;
if (!out.width || !out.height)        reject;
if (out.width  > cfg.max_width)       reject;
if (out.height > cfg.max_height)      reject;
if (out.width  < visible_width)       reject;   /* 比请求还小 = 异常 */
if (out.height < visible_height)      reject;
if (out.pitch < out.width)            reject;
if (out.pitch > ((max_width+255)&~255))reject;  /* 行距上限 = 宽度 256 对齐 */
if (out.pitch & 1)                    reject;   /* 必须偶数 */
if (out.pitch_bytes != out.pitch * (bit_depth==10?2:1)) reject;
if (!out.buffer || out.buffer_size < required_bytes) reject;
if (out.buffer_size > frame_slot_size)reject;
if (!frame_is_in_pool(out.buffer))    reject;   /* 关键：防越界/防伪造指针 */
```
ProsperoLight 另有两条工程性校验（`moonlight_stream.cpp:1295-1315`）：`output.codec == mode->codec_type`、`output.height ∈ {coded, visible}`。

### 3.4 输出缓冲的生命周期（**决定要不要拷贝**）

- **槽复用规则**：`out.buffer` 在"下一次 `Decode`/`Flush` 复用到同一个槽"之前有效（`videodec2-abi.md:§3`）。
- **零拷贝做法**（ProsperoLight）：提交时 `slot = au_index % 3`，呈现端用完才允许复用的槽，用 `native_agc_wait_source_idle(frame_slot)` **等 GPU 采样完**再提交下一个同槽 AU（`moonlight_stream.cpp:1166-1174`）。
- **拷贝做法**（EVO）：拷进自己的重排窗口（`ro_harvest`），窗口 ≤ 5 帧时槽不会被复用（`evo_vdec_native.c:816-825` 注释给出这层推理）。EVO 的偷懒版是**直接借用**帧池指针（`s->borrowed = 1`），前提是"重排窗口 ≤ 5 帧远短于 8 槽循环"。
- **我们的取舍**：帧池 3 槽 + "呈现完成信号" 才能零拷贝；不确定就用**拷贝到自有缓冲**（1080p 约 3.2 MiB/帧 memcpy，~0.3 ms）换正确性。

---

## 4. 解码帧怎么"用"：两条路

### (A) 照 EVO：NV12 → AGC 视频管线做 YUV→RGB（零拷贝）

**机制**（全部真机验证）：
1. 解码器帧池是 direct memory，**CPU 虚拟地址 == GPU 地址**（起点 256 对齐、pitch 256 对齐时直接当 GPU 地址用）：
   `EVO-PLAYER-PS5:projects/evoplayer/media/src/evo_agc_runtime.c:2335-2339`
   ```c
   if (is_direct && ((uintptr_t)src & 255u)==0 && ((uint32_t)src_pitch & 255u)==0) {
       *out_pitch = src_pitch; *out_gpu = (uint64_t)(uintptr_t)src; return 0; }  /* 零拷贝 */
   ```
   1080p 的 `pitch=2048`（256 对齐）、槽 16K 对齐 → 条件天然满足。
2. 为 Y 建 **T# R8**、为 UV 建 **T# RG8**（`evo_agc_build_tsharp_r8/rg8`，各 48 字节描述符 + S# 采样器），指向帧池地址：`evo_agc_runtime.c:2700-2760`。
3. 选 `EVO_AGC_PIPE_VIDEO_NV12` 管线（10-bit 走 `VIDEO_HDR`/`VIDEO_HLG`），全屏 viewport + scissor，V# 常量传 crop/scale：`evo_agc_runtime.c:2581-2700`。
4. 画 6 顶点 quad，`sceAgcDcbSetFlip` 由 DCB 自己发（**不用 `sceVideoOutSubmitFlip`**）：`docs/evo-pro/status.md:188-200`。
5. 代价：EVO 实测 4K 合成 **982 µs/帧**（见 `run-continuation/ps5-port-status.md` 的 GPU 渲染调研节）。

**可行性要点**：
- **不需要 amdllpc**：视频管线已作为**预编译 ISA 数组 + 全部寄存器值**随源码发布（`projects/evoplayer/shaders/agc/video_yuv_nv12_pipe.h`，7.3 KB，含 `..._GS_ISA_BYTES 256`、`..._RSRC1/2`、`..._DRAW_MODIFIER`、user-SGPR 布局），由 `tools/build_agc_pipes.py` + 打过 gfx1013 补丁的 amdllpc 生成（`docs/hardware/shader-compilation.md:91-123`）。**复用这些 blob 合法（GPL-3.0）且省掉整条着色器工具链**。
- **但必须接管 VideoOut 与合成**：AGC 自己发 flip ⇒ 与我们 SDL ps5 驱动（`sceVideoOutOpen` + `RegisterBuffers2` + `SubmitFlip`）**不能同时存在**（`sceVideoOut` 单 owner，第二次 open 会 panic —— `run-continuation/ps5-port-status.md` GPU 调研节）。UI（nanovg/llvmpipe）要么也搬到 AGC（EVO 的 UI 渲染器就是自写的），要么让 AGC 只画视频而 UI 用 CPU 写进同一块扫描缓冲（需要 cache flush + 手写合成，EVO 的 present 里就有 `cache_flush` 计时项，`ProsperoLight:src/native_agc_present.hpp:14-18`）。
- **`libSceAgc`/`libSceAgcDriver` 必须在 self-unjail 之前初始化**（`docs/hardware/gpu-notes.md`，见状态文档同节）；我们的 native 线**已经有这两个 PRX 桩与 `-lSceAgc -lSceAgcDriver` 链接**（`ps5-native/ps5-opengl/tools/build-native-test-app.sh:209-220` 生成桩；我们的 native 构建已链入并出画面）。

**结论**：(A) 的性能与"视频平面零拷贝"最优，但**它是一整套渲染架构**（AGC runtime 2771 行 + writer 362 行 + transient ring 137 行，再加 flip/呈现所有权），不是"解个码"。**不建议与首批硬解接入同时做。**

### (B) NV12 → 现有 llvmpipe / nanovg 栈

先明确今天的视频是怎么上屏的（真机事实，`run-continuation/ps5-port-status.md`）：
- payload/native 线都是 `ps5-payload-dev/SDL` 的 **ps5 视频驱动**：窗口表面 `SDL_PIXELFORMAT_ABGR8888`（`ps5-native/cache/SDL/src/video/ps5/SDL_ps5video.c:93,162`）；**OSMesa 直接把 llvmpipe 的渲染目标指向窗口表面像素**（`SDL_ps5osmesa.c:142-160` 的 `OSMesaMakeCurrent(ctx, surface->pixels, ...)`）；`SDL_GL_SwapWindow` = `OSMesaFlush()` + `SDL_UpdateWindowSurface()`（`SDL_ps5osmesa.c:199-230`）；驱动再用 tile 线程拷贝 + `sceVideoOutSubmitFlip`（`SDL_ps5video.c:115-146`）。
- 视频则是 mpv 渲染进 **FBO**（`mpv_core.cpp:635` `mpv_fbo.fbo = media_framebuffer`）再被 wiliwili 自己用着色器画成四边形（`mpv_core.cpp` GL 分支），**两遍全屏**。

#### B1 — 上传两张纹理 + GLSL 做 YUV→RGB（改动最小）
在 `MPVCore::draw()` 的接缝处换成"我们自己的源"：
- `glTexSubImage2D(GL_TEXTURE_2D, ..., GL_R8,  w, coded_h, NV12_Y )`、`GL_RG8, w/2, coded_h/2, NV12_UV`（OSMesa 22 是 GL 3.3 core，R8/RG8 可用）；
- 一个 2 采样器片元着色器做 BT.709 limited→RGB（与 mpv 用的矩阵一致），画满 video rect；
- **代价**：llvmpipe 仍要对视频区域做一次全屏采样+转换 pass（≈今天 mpv 那一遍的量级，1080p 十几 ms 量级）；**收益**：CPU 解码全部释放，且省掉 mpv 的 FBO 那一遍（-> 由两遍变一遍）。
- **风险**：几乎为零（不动 UI 合成/清屏/顺序）。**建议作为第一步。**

#### B2 — CPU 转换 NV12→ABGR8888，**直写窗口表面**（推荐目标形态）
- 转换器：`libyuv` / ffmpeg `sws_scale` / 自写 AVX2 均可；EVO 的实测标尺是 **1080p 融合转换（转换+swizzle）0.98–2.11 ms/帧（4–6 worker）、4K 7.43–11.35 ms/帧**（`docs/research/converter-perf.md:221-227`），1080p 整条渲染线程 2.20 ms/帧（同文件 264-266）。
- 写到哪：`SDL_GetWindowSurface(window)->pixels` 的 video rect（线性 ABGR8888），随后驱动的 tile 拷贝 + flip 照旧。
- **合成顺序问题（唯一难点）**：
  - 简单版：**视频先写、UI 后画**。要求 UI 在该矩形内不重绘——今天 borealis 每帧整屏清屏，所以需要用"清屏补集"（`VideoContext::clearExcept()` 曾实现过，见状态文档"主页局部渲染（静态层）"节，那次因 UI 失效判定复杂而回退；**对视频矩形而言失效条件简单**：视频每帧都重画）。danmaku 在同一 nanovg pass 里画在 VideoView 之后（`wiliwili/source/view/video_view.cpp:626-657`），顺序天然正确。
  - 稳妥版：UI 照常整帧画（video rect 画成透明/黑），在 `OSMesaFlush()` 之后、`SDL_UpdateWindowSurface()` 之前，按 UI 的 alpha 通道对 video rect 做一次 source-over 合成（表面是 ABGR8888，有 alpha）。多一次矩形合成（~2M 像素，多线程 1–2 ms）。
- **收益**：视频彻底离开 llvmpipe（省掉全部视频 pass），是真帧率收益的来源。**代价**：需要动呈现顺序（B2 才有真正的风险，且与那次被回退的实验同源，务必先量后改）。

### 对比表

| | (A) AGC 视频管线 | (B1) 纹理 + GLSL | (B2) CPU 转换 + 直写表面 |
|---|---|---|---|
| 视频每帧成本 | **≈1 ms 量级**（4K 982 µs，真机） | 一次 llvmpipe 全屏采样 pass（≈今天 mpv 那遍） | **≈1–2 ms/1080p**（EVO 实测融合转换） |
| 解码帧拷贝 | 零拷贝（GPU 直采帧池） | CPU→GPU 上传 1.5×帧字节（OSMesa 下其实是 memcpy） | CPU 读 + 写 2×帧字节 |
| 改动面 | 接管 VideoOut/flip + AGC runtime + 合成/UI 方案 | `MPVCore::draw` 接缝 + 一个 shader | 同上 + 呈现顺序/清屏补集 |
| 与现有栈冲突 | **与 SDL ps5 驱动抢 VideoOut** | 无 | 与 borealis 整屏清屏需协调 |
| 主要风险 | 与 UI/OSD 合成、flip 所有权、GPU hang | llvmpipe 视频 pass 没变快 | 残影/闪烁类视觉缺陷（有过前科） |
| 许可证 | EVO/ProsperoLight 均 GPL-3.0，可借鉴 | 自写 | 自写 |

---

## 5. 与 wiliwili 现有播放链的接入点

### 5.1 现状（每一步的文件/符号）

| 环节 | 现状 | 位置 |
|---|---|---|
| 取播放地址 | B 站 `playurl` → `VideoUrlResult{dash.video[], dash.audio[], durl[]}`（DASH 下**视频与音频是两个 URL**，`codecid` 7=AVC/12=HEVC/13=AV1） | `wiliwili/include/api/bilibili/result/video_detail_result.h:455-529`；`wiliwili/source/utils/config_helper.cpp:279`（codecid 映射） |
| 选轨 + 拼 mpv 参数 | 选 `codecid == BILI::VIDEO_CODEC` 的视频轨、选音轨（杜比/FLAC/标准回退），`setUrl(v.base_url, start, end, audios)` | `wiliwili/source/activity/player_base_activity.cpp:518-700` |
| mpv 参数 | `vo=libmpv`、`hwdec`（当前 PS5 上必然是"软"）、`video-timing-offset=0`、`keep-open=yes`、`hr-seek=yes`、观察 `playback-time` 等 | `wiliwili/source/view/mpv_core.cpp:299-412` |
| 传给 mpv 的额外参数 | `referrer="https://www.bilibili.com"`、`audio-file="<音频 URL>"`（**这行就是"音视频可分离"的证据**） | `wiliwili/source/view/video_view.cpp:824-838` |
| 视频渲染 | mpv 渲染进 FBO（`mpv_fbo.fbo = media_framebuffer`），再由 wiliwili 用自己 shader 画四边形到默认帧缓冲 | `wiliwili/source/view/mpv_core.cpp:635`、同文件 `MPVCore::draw()` GL 分支 |
| 叠加层 | `VideoView::draw()`：先 `mpvCore->draw()`，再弹幕（`DanmakuCore`）/OSD | `wiliwili/source/view/video_view.cpp:626-657` |
| 弹幕时间基准 | `MPVCore::instance().playback_time`（mpv 的时钟属性） | `wiliwili/source/view/danmaku_core.cpp:401,434,552` |
| 呈现 | OSMesa 直写窗口表面 → `SDL_UpdateWindowSurface` → ps5 驱动 tile 拷贝 + `SubmitFlip`（只保留一帧在飞） | `ps5-native/cache/SDL/src/video/ps5/SDL_ps5osmesa.c:199-230`、`SDL_ps5video.c:115-146` |

### 5.2 留 / 绕 清单

**保留（不动）**
- mpv 的**音频**：解码、输出、`audio-file` 回退链、杜比/FLAC 选择。
- mpv 的**时钟与状态机**：`playback-time` / `duration` / `pause` / `speed` / `core-idle` / `paused-for-cache` / 缓存速率（`mpv_core.cpp:399-412` 已全部 observe）。
- 弹幕、字幕、进度条、片头片尾、清晰度切换、备份 URL 逻辑、UI/输入/网络（全部与视频像素无关）。
- 视频页的 OSD 与评论区。

**必须绕开**
- mpv 的**视频解码**（lavc 软解）：`--vid=no` 或干脆不给 mpv 视频轨。
- mpv 的**视频渲染**（`vo=libmpv` 的 render context / FBO 那一遍）：改成我们自己画。
- **不能让 mpv 同时读视频 URL**：否则视频数据被下载两遍（mpv 即使 `--vid=no` 也会把整条流读下来）。

### 5.3 最小改造方案（推荐）

```
[mpv]   只喂音频 URL（dash.audio[i].base_url + backup 作为 audio-file）
        ⇒ 音频解码/输出 + 时钟 + seek/pause/speed 全部照旧
            │  playback_time（已有属性）
            ▼
[新模块 ps5_native_video]
  demux  : 独立 ffmpeg avformat 打开 dash.video[j].base_url
           （必须带 Referer: https://www.bilibili.com；CA 与 mpv 同源策略）
  bsf    : h264_mp4toannexb / hevc_mp4toannexb（DASH 是 fMP4/avcC）
  解码   : videodec2（§2/§3），产出 NV12 + pts
  节奏   : 以 mpv 的 playback_time 为基准：晚到就丢，早到就等（EVO pp_clock 同构）
  呈现   : (B1) 上传纹理 + shader  或  (B2) CPU 转换直写视频矩形
            │
            ▼
[VideoView::draw]  mpvCore->draw(...) 改为 nativeVideo->present(rect, alpha)
                  之后的弹幕/OSD 一行不改
```

**落地顺序（每一步都可独立验证）**
1. **能力探针**（不改播放链）：启动时 `sceSysmoduleLoadModule(207)` + 建一个 1080p AVC 常驻解码器 + 解一个内置样本 AU，日志打印 `rc/valid/pitch`；失败则整条 native 视频路径关闭。产出物：固件 12.00 上的第一手数据 + 内存实测值。
2. **B1 呈现**：接自建 demux + 解码，固定清晰度、无 seek，验证画面/颜色/时间轴对齐。
3. **行为对齐**：seek（`av_seek_frame` + `sceVideodec2Reset` + **重建 bsf**）、pause、speed、切清晰度（换 URL 需重建解码器）、备份 URL 回退、播放结束。
4. **B2 呈现**（若视频 pass 仍是瓶颈）。
5. **回退开关**：native 路径连续 N 帧错误 → 回到 mpv 软解（EVO 的 `fatal-streak → 干净回退`，`EVO-PLAYER-PS5:docs/evo-pro/native-decode-plan.md:404-406`；同样的机制也写在 `docs/evo-pro/status.md:676`）。

**边界情形**
- `durl`（FLV 单 URL，音视频复用）：无法分离 → `--vid=no` + 视频另开一条连接会把视频下载两遍；建议这类流**直接回退 mpv 软解**（B 站主流是 DASH）。
- **AV1（codecid 13）**：videodec2 无路（EVO 不支持），回退 mpv 或引导用户改设置回 AVC/HEVC。
- **直播**：`live_player_activity` 走另一套接口，首批不碰。

---

## 6. 导入桩、符号清单、codec 表、内存预算

### 6.1 需要新增的导入桩（**我们只缺 1 个**）

| 模块 | 是否已有 | 说明 |
|---|---|---|
| `libkernel.so` | **已有**（payload SDK） | 实测 `sceKernelAllocateDirectMemory` / `MapDirectMemory` / `MapNamedFlexibleMemory` / `AvailableFlexibleMemorySize` / `ConfiguredFlexibleMemorySize` / `ReleaseFlexibleMemory` / `GetDirectMemorySize` / `VirtualQuery` / `ReleaseDirectMemory` / `Munmap` / `DebugOutText` 全部在桩里 |
| `libSceSysmodule.so` | **已有** | `sceSysmoduleLoadModule` |
| `libSceAgc.so` / `libSceAgcDriver.so` | 已有（ps5-opengl 生成并已链入） | 仅 (A) 需要 |
| **`libSceVideodec2.so`** | **没有，需自建**（10 个符号） | 生成方式见下 |

**10 个符号**（照抄 `EVO-PLAYER-PS5:tools/native-app/stubs/prx/libSceVideodec2.syms`）：
```
sceVideodec2QueryComputeMemoryInfo   sceVideodec2AllocateComputeQueue  sceVideodec2ReleaseComputeQueue
sceVideodec2QueryDecoderMemoryInfo   sceVideodec2CreateDecoder         sceVideodec2DeleteDecoder
sceVideodec2MapDirectMemory          sceVideodec2Reset
sceVideodec2Decode                   sceVideodec2Flush
```
（`MapDirectMemory` 三份实现都没调用，但它是真实导出，留着无害。）

**桩的生成（与现有 AGC 桩完全同构）**：写一个 `videodec2_link_stub.c`（空函数体，返回 -1，**永不被打包**——`EVO-PLAYER-PS5:tools/native-app/stubs/videodec2_link_stub.c:1-8` 顶部注释明确"These bodies are never packaged or executed; the native module writer emits system imports"），然后：
```bash
prospero-clang -c videodec2_link_stub.c -o videodec2_link_stub.o
prospero-lld --shared -soname libSceVideodec2.prx -o <stub_dir>/libSceVideodec2.so videodec2_link_stub.o
```
参照 `ps5-native/ps5-opengl/tools/build-native-test-app.sh:209-220`（AGC 桩就是这么生成的）。
链接侧：在 native 链接命令里加 `-lSceVideodec2`；`ps5-native/ps5-native-app-boilerplate/tooling/native/native_app_builder.cpp:248` 会把 `--stub-dir` 下所有 `*.so` 当模块导出表，SONAME（`libSceVideodec2.prx`）决定 `NEEDED` 名 ⇒ **loader 启动时自动把 `libSceVideodec2.sprx` 装进来**，代码里直接 `extern` 调用即可（无 `LoadStartModule`、无 NID 计算）。我们 native 线的 stub 目录已由 `scripts/ps5/native/native_build.py:355-380` 通过 `PS5_NATIVE_STUB_DIR` 可配（默认就是带 AGC 桩的那份 SDK 副本），把新 `libSceVideodec2.so` 放进同一目录即可。

**ABI 结构体**：直接用 `EVO-PLAYER-PS5:projects/evoplayer/media/include/sce/sce_videodec2.h`（GPL-3.0，可直接放进我们的源码树），并**加上 ProsperoTV 的编译期护栏**（`ProsperoTV:src/iptv_native_backend.c:123-133`）：
`sizeof(config)==72`、`sizeof(memory)==72`、`sizeof(compute_config)==16`、`sizeof(compute_memory)==24`、`sizeof(input)==48`、`sizeof(frame)==32`、`sizeof(output)==56`，以及 `offsetof(config, optimize_progressive)==60`。

### 6.2 codec / profile / level 表（照抄，来源 `videodec2-abi.md:§5` + `evo_vdec_native.c:270-281`）

| codec | `codec_type` | `profile` | `max_level` 1080 / 1440 / 2160 |
|---|---|---|---|
| H.264 | `1` | `66` Baseline / `77` Main / `100` High | `51` / `52` / `52` |
| HEVC | `974921` (`0xEE049`) | `1` Main / `2` Main10 | `123` / `150` / `153` |
| VP9 | `2382845` (`0x245BFD`) | `0` P0 / `2` P2 | `41` / `51`（VP9 的 x10 标度是估计值，未实测） |

映射（来自解复用器的 `AVCodecParameters`）：`AV_CODEC_ID_H264 → 1`、`AV_CODEC_ID_HEVC → 974921`、`AV_CODEC_ID_VP9 → 2382845`；`w/h` 各自 `roundup16`（`evo_vdec_native.c:1040-1075`）。B 站 `codecid` → `AVCodecID`：7→H264、12→HEVC、13→AV1（AV1 无硬解路，回退）。

补充：`max_level` 只在 `<=1080p` 时用 `level_1080`，`>1080p` 用 `level_4k`（`evo_vdec_native.c:467`）；AVC 的 `max_level` 标度是 level×10，HEVC 是 `general_level_idc`（level×30）（同文件 265-269 注释）。

### 6.3 内存预算（公式 + 估算 + 必须实测的两项）

**先记住两个硬上限**
- **flexible memory 给 fake-signed 游戏模块的上限 ≈ 450 MB**（真机证据：`EVO-PLAYER-PS5:docs/evo-pro/status.md:669-671`，`heap live=62M peak=142M flex_maps=25958 ... flex_avail=281M` + "the hard ~450 MB flexible-memory budget"）。**解码器的主要内存不从这里出**（走 direct memory），但我们的 UI/封面/解析器都在这个池里。
- **direct memory** 的可用量 = `sceKernelGetDirectMemorySize()`（三个项目都在 bring-up 第一步取它）。我们目前**没有这个数字**（native 线的启动日志里没有），**这是第一个要量的值**。

**每解码器组成**

| 项 | 来源 | 1080p 估算 | 4K 估算 |
|---|---|---|---|
| compute `cm.cpu_gpu` | `QueryComputeMemoryInfo` 出 | 未知（要实测） | 未知 |
| `mem.cpu`（flexible 0x03） | `QueryDecoderMemoryInfo` 出 | **≈14 MB**（EVO 注释：depth 1 下 ~14 MB，`evo_vdec_native.c:616-618`） | 同量级（随 depth 变） |
| `mem.gpu`（direct） | 同上（DPB + 内部） | 未知，估 20–40 MB | 估 50–80 MB |
| `mem.cpu_gpu` | 同上，常为 0 | 0 | 0 |
| `mem.max_frame_size` | 同上 | 2048×(1088+544)=**3.19 MiB** | 3840×(2176+1088)=**11.95 MiB** |
| AU 池 | 自定（槽数 × 槽大小） | 3 × 2 MiB = **6 MB** | 3 × 4 MiB = **12 MB** |
| 帧池 | 自定（槽数 × max_frame_size） | 3 × 3.19 = **9.6 MB**（8 槽 = 25.5 MB） | 3 × 11.95 = **35.9 MB**（8 槽 = 95.6 MB） |
| **每解码器合计** | | **≈50–70 MB** | **≈110–170 MB** |

参照：EVO 常驻 5 个解码器（AVC 4K + HEVC 1080p + VP9 1080p + HEVC10 1080p + …，`evo_vdec_native.c:578-620`），其中 AVC 4K 失败会自动重试 1080p（`probe_slot`，`evo_vdec_native.c:539-577`）。
**建议我们先只要两个常驻**：AVC 1080p（覆盖 B 站绝大多数）+ HEVC 1080p（可选）。

**要实测并在日志里留档的三行**（照抄 EVO 的日志格式最容易）：
```
flex pool configured=%zuMB available=%zuMB before bring-up          /* sceKernelConfigured/AvailableFlexibleMemorySize */
RESIDENT %s decoder up %ux%u frame=%zuKB total=%zuKB (compute=%zuKB gpu=%zuKB cpu_gpu=%zuKB input=%zuKB frame_pool=%zuKB) flex=%zuKB
flex after bring-up = %zuMB (resident decoders took %zuMB)
```
出处：`EVO-PLAYER-PS5:projects/evoplayer/media/src/evo_vdec_native.c:578-640`、`:539-576`。

---

## 7. 已知风险

| 风险 | 依据 | 缓解 |
|---|---|---|
| **固件 12.00 未验证**（EVO 的硬件验证在 12.70） | `videodec2-abi.md:3` | 第 1 步就做能力探针，先拿 rc 序列 |
| **错误的 `codec_type`/`max_level` 会直接杀进程**（不是返错） | `evo_vdec_native.c:416-427` 注释 | 每步 breadcrumb 落盘（我们的 UDP 日志 + `/download0` 启动日志）；探针放在启动早期 |
| **native 调用 hang 会卡住整个标题**（"a payload that hangs holding VideoOut costs an hour to recover"） | `docs/hardware/hardware-decode-review.md:§7` | 解码线程看门狗 + 拒绝在渲染线程调用；先做离线单帧探针 |
| **seek 后 0x811d0303**（bsf 未重建，SPS/PPS 丢失） | `evo_vdec_native.c:1217-1250` | seek 路径强制重建 bsf；错误帧连续 N 次 → 回退软解 |
| **`0x811D0302 OVERSIZE_DECODE`**（DPB 太小，广播 H.264 隔行/多参考） | `ProsperoTV:src/iptv_native_backend.c:1069-1086` | 按 ProsperoTV 策略抬 `max_dpb_frames`（1080p AVC→16），代价是内存 |
| **输出缓冲生命周期搞错 → 花屏/撕裂** | `videodec2-abi.md:§3`（槽复用规则） | 先"拷贝出帧池"，确认稳定后再评估零拷贝 |
| **帧池 prot 选错**：用 0x32（CPU 只写）却想 CPU 读 → 读不到/异常 | `videodec2-abi.md:103-105` | 走 (B) 就用 0x33 |
| **flexible memory 上限 ≈450 MB** 影响整个应用（不只解码器） | `docs/evo-pro/status.md:669-671` | 解码器内存走 direct memory；UI 侧已用 llvmpipe + 资源内嵌，注意不要再加 flex 大户 |
| **PS5 上 HEVC/VP9 的 B 站片源占比低**，收益主要集中在 4K/高码率 H.264 | 常识 + `player_base_activity.cpp:627` 的 codecid 选择 | 首批只做 AVC，HEVC 次之 |
| **(A) 与 SDL ps5 驱动抢 VideoOut** | `sceVideoOut` 单 owner（状态文档 GPU 调研节） | (A) 需要整体接管呈现，不做半套 |
| **双 demuxer 的 A/V 漂移**（mpv 音频时钟 vs 我们视频 PTS） | — | 以 `playback_time` 为唯一基准，晚到即丢；seek 后按时间对齐丢弃 |
| 自建 demuxer 必须复现 mpv 的 `referrer` 与 TLS/CA 配置 | `video_view.cpp:824-838`；payload 线 CA 教训见状态文档 | 先用 mpv 相同的 URL+header 做一次纯 demux 冒烟 |

---

## 8. 工作量估计

以"我们已经具备 native 标题线的构建/部署/日志基础设施"为前提（这部分**不重复投入**）。

| 阶段 | 内容 | 估计 |
|---|---|---|
| P0 探针 | 生成 `libSceVideodec2` 桩；启动期 207 + compute + decoder（1080p AVC）bring-up；解一帧内置 AU；落盘 rc 序列与内存实测值 | **1.5–2 人天** |
| P1 接入（B1） | 自建 ffmpeg demux（视频 URL + Referrer）→ bsf → videodec2 → 纹理上传 + YUV→RGB shader → 接进 `MPVCore::draw` 接缝；mpv 改音频专用；固定清晰度、无 seek | **3–4 人天** |
| P2 行为对齐 | seek/pause/speed/切清晰度/备份 URL/结束/错误回退；重排窗口与节奏控制 | **2–3 人天** |
| P3 codec 与内存 | HEVC（+可选 Main10/P010 判断）、常驻双解码器、内存预算实测与调参、DPB 策略 | **1.5–2 人天** |
| P4（可选）B2 直写表面 | 融合 NV12→ABGR8888 转换器 + 呈现顺序/清屏补集；需要处理残留/闪烁类缺陷 | **3–5 人天**（有一定概率撞上"视觉缺陷不可接受"而回退） |
| P5（可选，另立项）(A) AGC 视频管线 | 接管 VideoOut/flip + AGC runtime/writer/transient ring + 与 UI 合成方案 | **15–30 人天**（等价于"给 native 线换一套渲染器"） |

**合计**：**P0–P3 ≈ 8–11 人天** 可得到"1080p AVC 硬解播放、可 seek、可回退"的原生线播放路径；加 P4 约 **11–16 人天**。P5 不应与上面混做。

---

## 9. 参考与许可证

| 项目 | 许可证 | 本文引用位置 |
|---|---|---|
| `sainsaji/EVO-PLAYER-PS5` | GPL-3.0 | `docs/evo-pro/videodec2-abi.md`（§2/§3/§4/§5/§6）、`projects/evoplayer/media/include/sce/sce_videodec2.h`、`projects/evoplayer/media/src/evo_vdec_native.c`、`evo_agc_runtime.c`、`projects/evoplayer/pp/src/pp_playback.c`、`projects/evoplayer/shaders/agc/video_yuv_nv12_pipe.h`、`tools/native-app/stubs/{prx/libSceVideodec2.syms,videodec2_link_stub.c,prx/README.md}`、`docs/evo-pro/status.md`、`docs/hardware/hardware-decode-review.md`、`docs/research/converter-perf.md`、`docs/hardware/shader-compilation.md` |
| `blackbearreloaded/ProsperoLight` | GPL-3.0 | `src/moonlight_stream.cpp`、`src/native_agc_present.hpp`、`Makefile`、`docs/PERFORMANCE_ROUND_3.md` |
| `blackbearreloaded/ProsperoTV` | GPL-3.0 | `src/iptv_native_backend.c`、`src/iptv_native_agc_present.h`、`vendor/ps5/sdk/stubs/videodec2_link_stub.c` |
| `blackbearreloaded/ps5-native-app-boilerplate` | （骨架，本机已有） | `tooling/native/sce_module_writer.cpp`、`tooling/native/native_app_builder.cpp:248` |
| `ps5-payload-dev/SDL`（`37acbca`） | zlib | `src/video/ps5/SDL_ps5osmesa.c`、`SDL_ps5video.c` |
| wiliwili 自身 | GPL-3.0 | `wiliwili/source/**` 各引用点 |

> 提醒：`shadPS4` / `sharpemu` / `AnyPS5` / `prosperity` 为 GPL-2.0，**只能读、不可复制代码**；本文未引用它们的代码。


---

## 实测更新（2026-09-25 真机，原生标题 PPSA99013）

在原生标题里加了 `WILIWILI_TEST_FFMPEG=<url>` 探针（`wiliwili/source/main.cpp`），真机结果：

```
fmpeg: version=7.0.1
fmpeg: open rc=0                 ← HTTP 拉流成功（网络可用）
fmpeg: streams=1 find_rc=0
fmpeg: codec=h264 640x360
fmpeg: decoder_open rc=0
fmpeg: frame 640x360 pix_fmt=0
fmpeg: sws_scale lines=360 px=7a02f8
fmpeg: probe done frame=1
```

⇒ **ffmpeg（demux + H.264 解码 + swscale）在 app slot 里完全可用**，P0 目标达成；配合"mpv 在 app slot 里连初始化都过不去"（见 `05-n0-progress.md`），P1 的实际形态要调整为 **完全绕开 mpv**（视频与音频都自管）。

### 音频后端：SDL2 不可用（新增约束）

同一探针里 `SDL_InitSubSystem(SDL_INIT_AUDIO)` 返回 **-1**、`SDL_GetCurrentAudioDriver()` 返回 **(none)** ⇒ ps5-opengl 提供的 native SDL2 构建**没有音频后端**。自管播放器的音频需要二选一：

1. **直接接 `SceAudioOut`**（`libSceAudioOut`，与 VideoOut 同族的系统接口）——推荐，依赖少；
2. 重建 native SDL2 并启用音频后端（需要对应的导入桩与驱动实现）。

### 其它实测

- 标题 → 开发机的 **TCP 连接失败**（`connect()` 不成功），而同方向的 **UDP 正常**（`wiliwili_boot_log` 的 9999 一直可用）⇒ 之后回传大块数据用 UDP 分片，或用控制台侧落盘再取。


### P1 里程碑 M1 实测：软件解码可以，但"每帧上传纹理"不可行（2026-09-26 真机）

在原生标题里实现并跑通了自管视频（`wiliwili/source/utils/ffmpeg_video_test.cpp`，开关 `WILIWILI_TEST_FFMPEG`）：
ffmpeg 解封装/解码 → swscale 成 RGBA → 纹理 → nanovg 全屏绘制。真机数据：

| 路径 | 解码 | 上传调用 | **应用帧率** |
|---|---|---|---|
| `nvgUpdateImage`（nanovg 流式纹理） | 0.83 ms/帧 | 7.8-8.7 ms/帧 | — |
| `nvgUpdateImage` + `STREAMING\|COPY_SWAP` | 0.83 ms | 8.3 ms（无改善） | — |
| **原生 GL 纹理 + `glTexSubImage2D` + `nvglCreateImageFromHandleGL3`** | 0.83 ms | **0.01 ms** | **4.3 fps** |

⇒ **解码极便宜（0.8 ms），上传调用也能做到 0.01 ms，但绘制含动态纹理的视频层会把整机帧率压到 4.3 fps**（每帧约 230 ms 落在驱动的纹理同步/上传路径上，而不是我们自己的调用里）。640×360 的纹理放大到 1920×1080 本该是几个 ms 的量级，所以这是**驱动侧动态纹理的代价**，不是像素量。

**结论（重要，改变 P1 优先级）**：在 ps5-opengl/AGC 这条 GL 路上，**"软件解码 + 每帧上传纹理"不可行**；能走通的是**零拷贝**——解码器把帧直接写进 GPU 可见的 direct memory，渲染时直接采样那块内存（EVO-PLAYER-PS5 的 `sceVideodec2` 方案正是如此：NV12 作为 R8+RG8 纹理，从解码帧池零拷贝）。因此：

- **P1（ffmpeg 自管 + 自绘）到此为止**：它的价值是验证"ffmpeg 可用 + 解码便宜 + 链路可跑通"，已完成；
- **下一步应直接做 P2（`sceVideodec2` 硬解零拷贝）**：app slot 里硬解可用（视频验证见前文），配合 AGC 管线做 YUV→RGB，即可同时解决"解码"与"上传"两个环节；
- 若仍想保留软件解码，则必须找到驱动支持的**稳定纹理/一次性上传**路径（例如把帧放进 direct memory 再让纹理指向它），否则 4.3 fps 是不可接受的。

**顺带确认的约束**：SDL2 音频不可用（`rc=-1`、driver=(none)）⇒ 音频要走 `SceAudioOut`。


### P2a 门槛测试（2026-09-26 真机）：第一步即被挡

已接好完整链路：链接桩 `scripts/ps5/native/videodec2_link_stub.c`（EVO 的 10 个符号，GPL-3.0）+ 探针 `videodec2_probe.c`（照 EVO 的硬件验证序列）+ 构建接入（`native_build.py` 的 extra_sources）+ 单帧 Annex-B 片源（`assets/vdec-au.h264`，H.264 High，7201 B）+ 触发开关 `WILIWILI_TEST_VDEC=1`。

真机结果（PPSA99013）：

```
vdec: enter
vdec: sysmodule207 rc=0            ← 系统模块加载成功
vdec: direct_limit=0x300000000     ← 12 GiB direct memory 可用
vdec: query_compute rc=-1 size=0x0 ← 卡在这里（EVO 在 12.70 上是 0）
```

**已核对但排除的差异**：我们的 `param.json` 与 EVO 的对照表基本一致——`applicationCategoryType: 0`（game ✓）、`contentBadgeType: 1` ✓、`gameIntent.permittedIntents` ✓、`downloadDataSize: 256` ✓；唯一差异是 `attribute`（我们 0，EVO 用 `0x62000000`，那是 HDR VideoOut 用的，与 videodec 无关）。

**候选原因（待验证，按可能性）**：

1. **固件差异**：我们 12.00，EVO 的硬件验证在 12.70；服务可能有额外前置条件或对 fake-signed 标题的放行策略不同。
2. `sceSysmoduleLoadModule` 返回 0 但服务并未真正可用 ⇒ 加 `sceSysmoduleIsLoaded(207)` + `errno`/`sceKernelGetLastError` 打印即可判定。
3. NID 推导差异（模块写入器按 SHA1 从名字推 NID）：若解析错，通常表现为崩溃而不是 rc=-1，但仍应打印各 API 的解析地址核对。
4. 服务对标题的额外声明要求（param.json 其它字段、或需要先调别的初始化接口）。

**下一步实验（都便宜）**：(a) 打印 `sceSysmoduleIsLoaded` 与 errno；(b) 打印 `sceVideodec2*` 的运行时地址（确认解析到真函数而非桩）；(c) 试不同 `size` 取值/先调 `sceVideodec2MapDirectMemory`；(d) 查 EVO/ProsperoLight 是否记录了固件版本要求。

**沉淀**：接硬解的"管道"已经修好（链接桩机制 + `scripts/ps5/native/regen-cdb.sh` 生成 native 用 compile_commands.json + 探针开关 + 片源随包），后续无论走哪条硬解路线都不必再搭。


### P2b 实测（2026-09-26 真机）：硬解 + 纹理呈现 = 60 fps

连续解 30 帧 Annex-B 流（含 P 帧、含 Flush 路径），并把解码出的 Y 平面当纹理每帧全屏绘制：

```
vdec: aus=30
vdec: stream decoded=30 buffered=30
vdec: avg_decode_x100=146     ← 1.46 ms/AU（含 Flush 全流程）
vdec: last 640x368 pitch=768
vdec: fps~60 upload_avg_x100=0
```

⇒ **硬解 1.46 ms/帧 + 呈现跑满 60 fps**。与软件路径的对比很关键：ffmpeg 软解 + **nanovg 的 `nvgUpdateImage`** 上传使整机掉到 4.3 fps（见上节），而**硬解 + 原生 GL 纹理上传 + 绘制**是 60 fps。也就是说：

- **瓶颈从来不是解码**，而是"每帧把 RGBA 交给 nanovg 更新纹理"这条路径；
- 正确形态是 **解码器（硬件）→ 帧在 direct memory → GL 纹理（raw GL 上传）→ 自定义着色器做 YUV→RGB → 绘制**。

### P2c 计划（下一步）

1. 上传 **NV12 两个平面**（Y=R8、UV=RG8，共 1.5 B/px，640×368 约 353 KB）；
2. 用一个小的 GLSL 着色器做 YUV→RGB（raw GL 画四边形；nanovg 不支持 YUV 采样）；
3. 测量帧率（目标仍 60），并核对颜色正确性；
4. 之后 P2d 接音频（`SceAudioOut`，桩已在桩目录里）、P2e 接进播放器视图与时钟同步。

**内存注意**：跑硬解探针时进程 471 MB（应用槽 flexible 上限约 450 MB 的记录来自 EVO，实测我们的槽能到 475 MB，暂未见分配失败，但要在正式接入时控制缓冲数量）。


### P2c 通过 + P2d 现状（2026-09-26）

**P2c（NV12 + 着色器呈现）已通过**：把 Y（R8）与 UV（RG8）两个平面每帧上传，用 raw GL 的
全屏三角形 + BT.601 limited YUV→RGB 片元着色器绘制：

```
vdec: yuv pipeline ready
vdec: fps~60 upload_avg_x100=0
```

⇒ 硬解 + 双平面 + 着色器仍是 **60 fps**，解码 1.47 ms/帧。颜色正确性待人工确认（测试图案的
红蓝/量化范围）。

**P2d（音频）现状**：SDL2 无音频后端（实测 rc=-1、driver=(none)），故走系统接口。
`sceAudioOutInit` **rc=0** ✓，但 `sceAudioOutOpen(0, 0, 0, 1024, 48000, 1)` 返回错误码
（重试 `userId=0xFF` 得到无效句柄），`Output` 一帧未送出。PS5 的函数签名与 PS4 相同
（对照 `AnyPS5` 的 `libSceAudio/Export.cpp`：`(userId, type, index, len, freq, param)`），
所以问题在**取值/前置条件**：可能需要在标题侧先做仲裁（桩里有 `sceAudioOutArbitrationInitialize`），
或改用 PS5 原生的 `sceAudioOut2*`（桩里整套都有）。桩与探针都已就位，剩下是参数迭代。

**探针开关汇总**（都通过 `assets/wiliwili-options.txt` 触发，不影响其它平台）：
`WILIWILI_TEST_VDEC=1`（硬解 bring-up + 30 帧连续解 + NV12 上屏）、`WILIWILI_TEST_AUDIO=1`（音频）、
`WILIWILI_TEST_FFMPEG=<url>`（ffmpeg 自管视频，历史）、`WILIWILI_VIDEO_UPLOAD=1`（上传路径对照）。


### P2d 第二轮（2026-09-26）：经典音频 API 打不通，剩两条路

```
audio: init rc=0
audio: initial_user rc=0 id=451557725      ← 真实用户上下文可用（payload 里这一步会返回 0x80940004）
audio: arbitration rc=0
audio: open user=451557725 param=1 -> -2144993263   ← 0x80310711（错误码）
audio: open user=451557725 param=0 -> -2144993263
audio: open user=0         param=1 -> -2144993263
audio: open user=0         param=0 -> -2144993263
audio: open user=255       param=1 -> 536870912     ← 0x20000000（无效句柄）
```

⇒ 用户上下文与仲裁都成功，但 `sceAudioOutOpen` 各种 userId/param 组合都被拒。剩下两条路：

1. **改用 PS5 原生音频 API `sceAudioOut2*`**（桩里整套都在：`ContextCreate`/`ContextPush`/`ContextQueryMemory`/
   `ArbitrationInitialize`/…）——这是环形推送模型，按 `02` 里硬解同款的"先 Query 再分配再 Create"套路接，预计 1-2 小时；
2. 继续猜经典 API 的取值（type/index 组合、`param` 位域含义、是否需要先 `sceAudioOutDeviceIdOpen`）。

推荐 1（`sceAudioOut2*` 是 PS5 原生路径，且符号齐全、模式与我们已经打通的硬解一致）。


### P2d 交接材料：PS5 原生音频 `sceAudioOut2*`（2026-09-26 调研）

经典 `sceAudioOut*` 打不通（见上），PS5 的原生路径是 `sceAudioOut2*`，桩里符号齐全。已从公共参考拿到**确切签名与结构体**（`SvenGDK/SharpProspero` 的
`Interop/Audio/AudioOut2.cs` 与 `docs/audio.md`；`boykopovar/AnyPS5` 的 `libSceAudioOut/src/AudioOut2*.cpp`）：

```c
int sceAudioOut2ArbitrationInitialize(...);
int sceAudioOut2ContextResetParam(SceAudioOut2ContextParam *params);            /* 用默认值填结构体 */
int sceAudioOut2ContextQueryMemory(const SceAudioOut2ContextParam *params, size_t *memorySize);
int sceAudioOut2ContextCreate(const SceAudioOut2ContextParam *params, void *buffer, size_t bufferSize, uint64_t *context);
int sceAudioOut2ContextSetAttributes(uint64_t context, const SceAudioOut2Attribute *attrs, uint32_t num);
int sceAudioOut2PortCreate(uint64_t context, const SceAudioOut2PortParam *params, uint64_t *port);
int sceAudioOut2PortSetAttributes(uint64_t port, const SceAudioOut2Attribute *attrs, uint32_t num);
int sceAudioOut2ContextPush(uint64_t context, uint32_t blocking);               /* 把当前块交给输出 */
int sceAudioOut2ContextAdvance(uint64_t context);                                /* 推进到下一块 */
int sceAudioOut2ContextGetQueueLevel(uint64_t context, uint32_t *queued, uint32_t *free);
int sceAudioOut2ContextDestroy(uint64_t context);
```

结构体（注意：**没有 `size` 首字段**，与 videodec2 的约定不同）：

```c
typedef struct { uint32_t MaxPorts, MaxObjectPorts, GuaranteeObjectPorts, QueueDepth, NumGrains, Flags; uint32_t Reserved[10]; } SceAudioOut2ContextParam;
typedef struct { uint32_t DataFormat, SamplingFreq, Flags; uintptr_t UserHandle; uint32_t Reserved[10]; } SceAudioOut2PortParam;
typedef struct { uint32_t AttributeId, Pad; uintptr_t ValueSize; } SceAudioOut2Attribute;
```

来自 `docs/audio.md` 的**硬约束**：grain（每块每声道帧数）必须是 **256 的整数倍且 256..2048**；主输出**只接受 48000 或 192000 Hz**。

**剩余的一个未知**：向当前 grain 写 PCM 的调用（桩里有 `sceAudioOut2ContextBedWrite`，SharpProspero 的 C# 侧把它包在 `AudioOutDevice.Output()` 里；需要确认它的确切签名——这是接线的最后一步）。

**接线方式**（照硬解那套已跑通的模式）：桩 `.so` 加进链接目录（`-lSceAudioOut` 已在用）+ 探针里逐调用打点（`ResetParam` → `QueryMemory` → 分配 → `ContextCreate` → `PortCreate` → 写正弦 → `Push`/`Advance`），每步 rc 落日志；出错就按 rc 收敛，和 P2a 两轮定位的方式一样。


### P2d 第三轮（2026-09-26）：经典 API 参数扫描无解；AudioOut2 需按高层封装还原调用序列

**经典 `sceAudioOutOpen` 参数扫描**（2×2×4×8 = 64 个组合：userId ∈ {真实用户, 0xFF}、type ∈ {0,1}、
len ∈ {256,512,1024,2048}、param ∈ {1,2,0,3,4,0x0001,0x0201,0x1002}，全部 rc 落日志）：
**没有任何组合成功**；唯一"非负"返回值是 `0x20000000`，但它在 `Output` 上失败，实际是错误码而非句柄。
⇒ 经典音频路径在本机（固件 12.00）对 fake-signed 标题不可用。

**`sceAudioOut2*` 盲试两次都崩**：
- 无参调用 `sceAudioOut2ArbitrationInitialize()` ⇒ 进程直接退出（签名需要参数）；
- 跳过仲裁后 `sceAudioOut2ContextResetParam(&params)` ⇒ 同样退出（结构体/约定与猜测不符）。

**下一步（明确）**：不要再盲试签名，改为**读高层封装的实现**来还原确切调用序列——
`SvenGDK/SharpProspero` 的 `AudioOutDevice`/`AudioQueueDevice`（`Interop/Audio/` 下的封装类）内部就是
按正确顺序调用 `sceAudioOut2Context*`/`Port*` 并填正确结构体字段的；把它们逐个翻译成 C 即可，
这与 P2a 用 EVO 的 ABI 文档一次打通硬解是同一个套路。

**音频这一项的现状总结**：SDL2 无后端 ✗、经典 `sceAudioOut*` 不可用 ✗、用户上下文 ✓、仲裁（经典）✓、
`sceAudioOut2*` 符号齐全 ✓ 且 ABI 有待按高层封装还原。**探针与桩都已就位**（`WILIWILI_TEST_AUDIO` /
`WILIWILI_TEST_AUDIO2`），还原出调用序列后一轮就能验证出声。


### P2d 第四轮（2026-09-26）：经典音频 API 全组合扫完，确认不可用

修正上一轮的判定漏洞（`0x20000000` 被误认为句柄而提前停止），只认"小正整数句柄"后把
**全部 128 个组合**跑完：`userId ∈ {真实用户, 0xFF} × type ∈ {0,1} × len ∈ {256,512,1024,2048} × param ∈ {1,2,0,3,4,0x1,0x201,0x1002}`（freq 固定 48000）
⇒ **128 miss、0 hit**。两种错误码：`0x80310711`（多数）与 `0x80310707`（非法 param 值，说明参数确实被解析）。

⇒ **经典 `sceAudioOut*` 对本机（12.00）的 fake-signed 标题不可用**（payload 上下文里 SDL 能用它是另一回事）。
音频只剩 **`sceAudioOut2*`** 一条路，其 ABI 细节需按 SharpProspero 的高层封装（`AudioOutDevice`）逐行还原。

**顺带澄清**：用户反馈"刚才好像有声音了"——我们的探针没有真正出声（`Output` 全部失败、`audio2` 在写数据前即崩），
听到的应是**标题退出/崩溃时的系统提示音**。
