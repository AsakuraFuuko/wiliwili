# 08 — 方案 C：sceVideodec2 硬解 / NV12-P010 呈现决策研究

> 日期：2026-10-05
>
> 结论先行：C **值得做，但只在目标包含 4K、HEVC、10-bit 或释放 CPU 时值得做**。A 已经解决了当前 1080p SW 面 60 FPS；C 不应作为 A 的替代优化，而应作为一条按能力探测启用、失败回退到 A 的视频后端。
>
> 当前固件是 12.00。P0 已在本机跑过 H.264 4K、HEVC Main 4K、HEVC Main10 1080p：三种 decoder/输出格式能力均通过；但 `sceVideodec2` 的 `OutputInfo` 没有输出 PTS 字段，PTS/reorder 子项仍未闭环，不能据此进入 P1。外部 EVO/Prospero 项目有 12.70 真机数据，可用于估算上限，不能替代本机验收。

## 1. 决策摘要

|问题|结论|
|---|---|
|C 能否带来 A 没有的能力？|能。硬解路径有机会覆盖 4K、HEVC Main、P010/Main10，并显著释放 CPU；A 只能优化 SW YUV→RGBA/缩放。|
|当前 12.00 能否直接承诺 4K/HEVC/10-bit/60？|decoder 能力 P0 已通过三组样本，但不等于可播放：4K Main10 未测，PTS/reorder 未验证，NV12/P010 呈现也未接入。严格 P0 未完全通过，当前不进入 C 的 P1。|
|NV12/P010 能否接现有 AGC？|接口形状已经具备：`evo_agc_blit_yuv()` 接受 NV12/planar/P010 参数，writer 有 R8/RG8/R16/RG16 T# 描述符。但当前 native 工作树的 video pipe 资源由 `EVO_AGC_HAVE_VIDEO_PIPES` 门控，`evo_agc_pipes.h` 当前只包含 UI pipe；还不是可直接接入的生产链路。|
|VideoOut 是否能直接注册 NV12/P010 扫描面？|没有证据，按不可行处理。公开/仓库内 API 只显示单一 RGB/BGRA buffer + 两个 scanout buffer，没有 YUV plane/CSC 注册接口。NV12 必须先由 AGC/GL shader 转成最终 BGRA/RGB scanout。|
|能否保留 mpv？|能保留 mpv 的音频、时钟、pause/speed/property/UI 状态；视频压缩流不能继续让 mpv 读取，否则会重复下载。视频需要独立 FFmpeg demux/BSF → `sceVideodec2` → present。|
|推荐顺序|保留当前 A；若继续 C，先补齐独立 FFmpeg PTS + VDEC 输出顺序实验，再做 P1 AGC NV12 8-bit。P0 关键 gate 未闭环就不做生产重构。|

## 2. 现状盘点

### 2.1 `videodec2_probe.c` 的事实边界

|能力/字段|状态|证据与含义|
|---|---|---|
|加载 `libSceVideodec2`、compute queue、decoder、reset|**已验证（当前 12.00）**|三次 P0 均为 `sysmodule=0`, `query_compute=0`, `compute_queue=0`, `query_decoder=0`, `create=0`, `reset=0`；探针主流程 `videodec2_probe.c:352-467`。|
|H.264/AVC 4K 8-bit|**已验证（当前 12.00）**|`PPSA99260`：codec=1/profile=100/level=52，60/60 AU，输出 3840×2160 NV12，见 P0 实测节。|
|HEVC Main 4K 8-bit|**已验证（当前 12.00）**|`PPSA99261`：codec=974921/profile=1/level=153，60/60 AU，输出 3840×2160 NV12，见 P0 实测节。|
|HEVC Main10 1080p/P010|**已验证（当前 12.00）**|`PPSA99262`：codec=974921/profile=2/level=123，60/60 AU，输出 1920×1088、`pitch_bytes=pitch*2`，P010 判定 60/60。|
|连续 AU、`Decode` 后 `valid=0` 再 `Flush`|**已验证（当前 P0）**|三组均 `decoded=60/60`, `buffered=60`, `accepted=60`, `errors=0`；路径在 `videodec2_probe.c:530-546`。当前 `pipeline_depth=1`，每 AU 同步 Flush。|
|输出格式/内存布局|**已验证（当前 P0）**|H.264/HEVC Main 为 `pitch=3840,pitch_bytes=3840`；Main10 为 `pitch=1920,pitch_bytes=3840`。`out.buffer` 三组均在 frame pool 内；探针校验见 `:554-592`。|
|frame pool/direct memory|**已验证为当前配置；不是生产 zero-copy 证明**|三组 direct limit 均 `0x300000000`，type 12 + `0x32/0x33` 映射和 flexible `0x03` 均成功；原始/对齐大小见 P0 实测表。GPU 仍未直接采样这些槽。|
|槽位与 pipeline|**仅探针**|`PIPELINE_SLOTS=3`、`pipeline_depth=1`，见 `videodec2_probe.c:51-54,414-415,506-598`。没有 GPU fence/VideoOut retire 保护，不能当作生产 zero-copy。|
|PTS/reorder|**未闭环（严格 P0 gate 未通过）**|输入只送 synthetic 90 kHz PTS、`step=3000`、DTS=`UINT64_MAX`，见 `:516-522`；ABI `OutputInfo` 没有输出 PTS 字段，三次均记录 `output_pts=not_in_abi reorder=not_observable`。不能宣称 B 帧已按显示 PTS 正确重排。|
|seek/flush 后 SPS/PPS|**当前 P0 未验证；现有笔记已有生产坑**|`notes/02-hwdecode.md:172-183,1151-1166`：DASH fMP4 必须过 Annex-B BSF，seek 后要重建 BSF；单纯 `av_bsf_flush()` 不会重新注入 SPS/PPS。|
|4K/4K60|**4K 连续解码已验证；端到端 4K60 未验证**|H.264/HEVC Main 均解出 60/60 个 3840×2160 AU；该探针无呈现、无真实媒体时钟，不能把 decoder P95 当 displayed 4K60。|
|VP9/AV1|**当前 wiliwili 未知/无 C 路线**|`sceVideodec2` ABI 参考声明 VP9 tag，但 wiliwili 探针没有实现；AV1 没有仓库内 `sceVideodec2` 路线。|

### 2.2 `notes/02-hwdecode.md` 中的坑重新分类

已经有价值、应直接复用的结论：

- DASH 是视频/音频分轨；现有 `VideoUrlResult` 与 `VideoView::genExtraUrlParam()` 已经能把音频 URL 作为 `audio-file` 传给 mpv，见 `notes/02-hwdecode.md:319-363`、`wiliwili/source/view/video_view.cpp:802-840`。
- fMP4/avcC/hvcC 必须经过 Annex-B BSF；AU 边界不能按 `av_read_frame()` 输出包简单猜测，见 `notes/02-hwdecode.md:172-183,1141-1166`。
- 解码器 Create 时固定最大尺寸；切清晰度或中途换分辨率需要重建 decoder，见 `notes/02-hwdecode.md:178-180`。
- PTS 需要自己做 reorder；`sceVideodec2` 的输入 PTS 不是完整的显示时序系统，见 `notes/02-hwdecode.md:204-214`。
- frame pool 槽在下一次 Decode/Flush 复用前有效；零拷贝必须等 GPU 使用完，见 `notes/02-hwdecode.md:252-257`。
- `max_dpb_frames` 不能从一个简单测试流硬推常量。历史笔记记录过真实 B 站流 `max_num_ref_frames=7` 拒绝 DPB=4，后来改 AUTO 才能解出，见 `notes/02-hwdecode.md:1222-1241`。生产实现应解析码流并优先使用 decoder AUTO 策略。
- 生产播放必须把网络读取、音频输出和视频解码移出 render thread，见 `notes/02-hwdecode.md:1093-1132`。

不能当作当前 wiliwili 12.00 已验证的内容：

- EVO/ProsperoLight 在其他版本/固件上的 HEVC、VP9、P010、4K 能力；这些是移植参考和上限估计，不是本机验收。
- “direct memory CPU VA == GPU VA”在其他项目中的零拷贝假设；当前探针只证明 CPU 可读，不证明 AGC shader 直接采样成功。
- `notes/02-hwdecode.md` 中引用的 EVO 4K GPU present 数值；它不是当前 wiliwili 视频链路的实测。

## 3. 编解码/格式/分辨率能力矩阵

### 3.1 当前主机 vs 参考项目

|格式|当前 wiliwili / 固件 12.00|参考项目数据（PS5 12.70；不可直接移植为验收）|决策含义|
|---|---|---|---|
|H.264 AVC 8-bit 4:2:0|**4K 连续解码通过**：3840×2160，60/60 AU，NV12，平均 16.504 ms、P95 20.195 ms；未测端到端 4K60 present|EVO `README.md:51-58` 与 validation 对 4K60 的口径冲突，仍只作参考|decoder 能力成立；4K60 播放仍需独立 present/时钟验收|
|HEVC Main 8-bit|**4K 连续解码通过**：3840×2160，60/60 AU，NV12，平均 6.509 ms、P95 8.372 ms；未接呈现|EVO `README.md:53-56`、`docs/codec-support.md:13-20` 的 4K 结论与本机结果方向一致，但不能替代本机 present 验收|可作为后续 8-bit C 生产首目标；当前仍被 PTS/reorder gate 卡住|
|HEVC Main10 / P010|**1080p 连续解码通过**：1920×1088 coded、60/60 AU，`pitch=1920,pitch_bytes=3840`，P010 60/60，平均 2.477 ms、P95 3.245 ms|参考 README 只可靠列到 1080p；后续文档对 4K Main10 仍有未完整测量说明|10-bit decoder 能力成立；4K Main10 与 HDR/present 仍未知|
|HEVC Main10 4K|未测|参考项目文档本身也存在 4K Main10 吞吐/呈现未收口的记录|不进入当前 P0；若继续 C，另立能力测试|
|VP9 Profile 0/2|未测|参考文档版本间口径不一致|不放进第一原型|
|AV1|无 `sceVideodec2` 路线|参考项目走软件 dav1d|C 不覆盖 AV1|

### 3.2 解码输出和显示上限

ABI 参考给出的 Create 尺寸档位是 `1920×1088`、`2560×1440`、`3840×2176`，HEVC level 在 2160p 使用 153，见 `references/EVO-PLAYER-PS5/docs/evo-pro/videodec2-abi.md:158-170`。这只是配置/ABI 允许的尺寸，不是当前固件的帧率承诺。

当前能用于决策的帧率数据只有参考项目的量级：

- 其 validation 表 `docs/build/validation.md:324-330` 记录 native H.264 4K60 decode 平均约 0.57 ms、P95 1.20 ms，native HEVC 4K60 平均约 0.56 ms、P95 0.59 ms；这是参考项目完整播放器中的 decode 统计，不是本仓库的 12.00 结果。
- 参考项目 README 同时记录 H.264 4K60 约 14 FPS，说明“硬件 decode 很快”不等于“最终 present 60 FPS”；VideoOut、GPU CSC、UI/OSD、时钟和版本路径都会改变结果。
- 本项目的验收必须分别测：decoder P95、NV12/P010 upload/stage、CSC/scale GPU 时间、UI/OSD 时间、flip/vblank 等待、实际 displayed frame PTS。只看 `agc health` 或 UI FPS 不够。

## 4. 呈现侧：两条路线

### 4.1 路线 A：AGC/NanoVG 侧 NV12/P010 shader

**可行性：架构可行；当前 native 树还缺生产接缝和完整 video pipe 资源。**

现有证据：

1. AGC runtime API 已经定义了 `EVO_AGC_PIPE_VIDEO_NV12`、`VIDEO_HDR`、`VIDEO_HLG`、`VIDEO_PLANAR`，并暴露 `evo_agc_blit_yuv(y, y_pitch, uv, uv_pitch, u, u_pitch, v, v_pitch, coded_w, coded_h, disp_w, disp_h, view_mode, ten_bit, color_trc, is_direct, pts_us)`，见 `library/borealis/library/include/borealis/extern/nanovg/agc/evo_agc_runtime.h:14-20,305-314`。
2. `evo_agc_runtime.c:3402-3650` 已有选 pipe、计算 crop/scale、构造 Y/UV 或 planar descriptor、记录 quad、标记 video PTS 的实现；`stage_plane()` 在 `:2532-2609` 对 256B 对齐且 direct 的源可直接把 CPU VA 当 GPU 地址，否则复制到 transient ring。
3. AGC writer 有 R8/RG8/R16/RG16 T# builder，见 `library/borealis/library/lib/extern/nanovg/agc/evo_agc_writer.c:109-195`。这正好覆盖 NV12 的 Y/R8 + UV/RG8 和 P010 的 16-bit 平面。
4. 这不是运行时 GLSL。`evo_agc_runtime.c:536-663` 通过预编译 shader metadata/ISA 调 `sceAgcCreateShader()` + `sceAgcLinkShaders()`；`evo_agc_pipes.h:1-6` 当前只包含 UI pipe 头。video pipe 编译由 `EVO_AGC_HAVE_VIDEO_PIPES` 门控，见 `evo_agc_runtime.c:1170-1200`；当前 native 对象 flags 没有该宏，当前工作树也没有 `video_yuv_*_pipe.h` 生成文件。故“API 有”不等于“当前 PPSA99233 已能调用 NV12 AGC pipe”。
5. `library/borealis/library/lib/extern/nanovg/nanovg_agc.cpp:232-344` 的 NanoVG backend 只把 UI geometry 绑定到 `EVO_AGC_PIPE_UI` 并写 T#/S# descriptor；它没有 GLSL 编译或通用 shader 注入入口。视频应走 runtime 的独立 `evo_agc_blit_yuv()`，不是给 NanoVG image handle 换一个像素格式。

需要改的文件/模块：

- 把 NV12/P010 预编译 pipe metadata/ISA 作为 native build 输入；更新 `evo_agc_pipes.h`，定义并验证 `EVO_AGC_HAVE_VIDEO_PIPES`。不能在标题内临时编译 GLSL。
- 新增 `NativeVideo`/`Ps5NativeVideo` 之类的线程安全 frame seam；由 `VideoView::draw()` 或同等视频绘制点调用 `evo_agc_blit_yuv()`。
- `VideoView::draw()` 目前先画 mpv 视频，再画弹幕/OSD，见 `wiliwili/source/view/video_view.cpp:627-658`。AGC video quad 必须在 OSD 前进入同一 DCB；不能在 `nvgEndFrame()` 后再画，否则会盖住弹幕/OSD。Borealis 帧循环顺序见 `library/borealis/library/lib/core/application.cpp:761-852`。
- 处理 AGC frame slot、VideoOut flip、GPU fence、P010 tone-map/HDR metadata 和视频 PTS。`evo_agc_runtime.h:114-168` 的 frame/slot/cache API 可复用，但当前 mpv SW image 路径没有使用它。

代价与收益：

- **收益**：NV12/P010 可由 GPU 做 CSC + scale；direct frame pool 条件满足时省掉 CPU→RGBA 和一次中间拷贝。高质量 scaler 可以实现为 AGC pipe 或后续 EASU/RCAS pass，而不是依赖 mpv swscale。
- **代价**：需要预编译 shader 资源、AGC pipeline 注册、direct/staging 生命周期、UI/OSD 的 DCB 顺序和 VideoOut fence。不是“给 NanoVG 加一个 NV12 texture”这么小的改动。
- **高质量边界**：现有 NV12 pipe 是否使用 bilinear、是否启用高质量 upscale，必须在 GPU shader 资源实际编译后用渐变/细线图测量；不能把 R8/RG8 descriptor 的存在当作画质保证。

### 4.2 路线 B：VideoOut/硬件 YUV plane 或 CSC

**可行性判定：公开 ABI 下不应作为方案。**

证据：

- payload SDK 的 PS5 SDL presenter 只创建 `SDL_PIXELFORMAT_ABGR8888` surface，见 `/opt/ps5-payload-sdk` 对应 SDL 源 `ps5-native/cache/SDL/src/video/ps5/SDL_ps5video.c:89-112`；注册时传单一 64-bit RGB/BGRA format `0x8000000022000000`、两个 buffer，见 `:197-202,215-265`。
- 该 backend 的本地声明只有不透明 `PS5_VideoAttr[80]` 和 `PS5_VideoBuf`，没有 plane descriptor 或 NV12/P010 scanout 字段，见 `ps5-native/cache/SDL/src/video/ps5/SDL_ps5video.h:36-83`。
- 参考 `evo_ps5.h` 只列 RGB/BGRA、float/HDR format 与 `sceVideoOutSetBufferAttribute2/RegisterBuffers2`，见 `references/EVO-PLAYER-PS5/projects/common/include/evo_ps5.h:124-218`；仓库内没有任何项目把 NV12/P010 注册为 VideoOut buffer，也没有第二平面注册 API。
- `/opt/ps5-payload-sdk` 的 `SDL_pixels.h` 有通用 `SDL_PIXELFORMAT_NV12`，`SDL_render.h` 有 `SDL_UpdateNVTexture` 声明；这只是 SDL 抽象/头文件能力，不证明 PS5 VideoOut 扫描面支持 NV12。当前 PS5 presenter 实现仍把结果变成单一 tiled BGRA。

因此：

- 不要投入“VideoOut 直接扫描 NV12/P010”路线；目前没有可调用的公开证据。
- “直写”可行的含义是 **AGC shader 把 NV12/P010 写入当前 BGRA scanout**，不是 VideoOut 接管 YUV plane。
- 如果 AGC pipe 不能在当前 native toolchain 编译/加载，唯一低风险 fallback 是两张 R8/RG8（或 R16/RG16）纹理 + 一个 GL/软件 shader pass；它能验证像素链路，但不提供真正的 VideoOut YUV zero-copy。

### 4.3 B1 GL 双平面作为原型 fallback

现有 `videodec2_probe.c:440-635` 已演示 R8 Y + RG8 UV、片元 shader CSC、crop/flip/U-V swap。它能证明“两个平面可以被当前 GL bridge 采样”，但有三个限制：

1. 它复制到稳定 CPU buffer，不是 decoder frame pool zero-copy。
2. 它是诊断探针，不接入 `MPVCore::draw()` 或 VideoView 生命周期。
3. 当前 `BOREALIS_USE_AGC` 下 `wiliwili_draw_gl_texture()` 是 no-op，见 `wiliwili/source/utils/ffmpeg_video_test.cpp:249-269`；不能把旧 GL helper 直接当作正式 AGC present。

B1 适合做 P1 正确性/格式验证；最终 4K/P010 高质量路线仍应回到 AGC pipe。

## 5. 管线改造与 mpv 边界

### 5.1 现有链路源码证据

|环节|当前代码|
|---|---|
|DASH 选视频轨/音频轨|`player_base_activity.cpp:599-701`：从 `result.dash.video` 选视频，从 `result.dash.audio` 选音频，再 `setUrl(v.base_url, ..., audios)`|
|音频 URL 传给 mpv|`video_view.cpp:802-820` 生成 `audio-file="..."`，`setUrl()` 在 `:831-840` 交给 `MPVCore`|
|mpv 输出/解码|`mpv_core.cpp:465-572` 设置 `vo=libmpv`、`hwdec`；原生标题 `:681-691` 选择 `MPV_RENDER_API_TYPE_SW`|
|SW 视频像素|`mpv_core.cpp:1127-1157`：mpv `pixels` 变化后 `nvgUpdateImage()`，再用 NanoVG image pattern 绘制|
|时钟/交互|观察 `playback-time/speed/pause/seeking`，见 `mpv_core.cpp:658-673`；VideoView seek 调 `mpvCore->seekRelative()`，见 `video_view.cpp:561-617`|
|UI/弹幕/OSD|`VideoView::draw()` 先视频，后进度、弹幕、OSD，见 `video_view.cpp:627-683`|

### 5.2 推荐架构：mpv 音频/时钟 + 独立视频线程

```text
B站 DASH video URL ── FFmpeg AVFormatContext
                         → H264/HEVC mp4toannexb BSF
                         → AU 聚合 / PTS 转 us
                         → sceVideodec2 Decode/Flush
                         → NV12/P010 frame slots
                         → AGC pipe（首选）或 B1 fallback

B站 DASH audio URL ── mpv audio-only
                         → ao=sdl
                         → playback-time / pause / speed / duration

VideoView::draw ── nativeVideo.present(recent frame, playback_time)
                 └─ 后续弹幕/OSD 仍按 MPVCore::playback_time 绘制
```

保留 mpv 是可行的，但不能只在调用方给 mpv 设一个未知 `hwdec` 名称：

- 当前 `MPVCore::init()` 只是把设置值写入 `hwdec`，见 `mpv_core.cpp:566-572`；仓库没有 `sceVideodec2` 的 mpv decoder/hwdec backend。
- native link 直接链接预构建 `-lmpv`，见 `scripts/ps5/native/native_build.py:299-316`。没有 caller-side packet callback 可把 mpv 的压缩视频包交给外部 VDEC；若要把 VDEC 做成 mpv 内部 backend，需要重编 libmpv，修改 mpv 的 `video/decode`/`hwdec`/`video/out` 内部，工作量和回退风险都更大。
- mpv 不能继续把视频 URL 当主文件并同时 `vid=no`，否则仍可能读取/缓存视频，造成重复网络和缓存压力。应增加“audio-only load”接口，主输入使用音频 URL，或单独维护 mpv audio handle。

### 5.3 交互和同步规则

- **播放**：worker 以 mpv `playback-time` 为音频主时钟。视频 PTS 早于时钟超过阈值就丢，晚于时钟就短等待；不能用渲染帧数当时钟。
- **暂停**：mpv pause 状态下停止送新视频帧，保留最近一帧；AGC 双 scanout buffer 仍需按 stale/PTS 规则重画，避免换 buffer 后黑屏/旧帧闪烁。
- **倍速**：mpv speed 改变只改变视频 pacing；decoder 不必改变，队列根据新 playback-time 追赶或丢帧。
- **seek**：增加 seek generation；停止旧 worker 输出，调用 `sceVideodec2Reset()`，重建对应 BSF，跳到目标附近 IDR，丢弃旧 generation 帧，直到新 PTS 稳定。不能只调用 `av_bsf_flush()`。
- **切清晰度/备份 URL**：停止并 join 旧视频 worker，按新轨道的 codec/profile/size 重建 decoder；失败立即退回当前 A 的 mpv SW 全链路。
- **退出**：先停止网络读取和 worker，再等待 GPU 使用完 frame slot，最后释放 decoder/compute queue/VideoOut 资源。否则会出现 decoder 槽被 AGC 采样时复用。

## 6. 最小验证原型与生产收口

### 6.1 P0：能力/内存/输出格式探针

**目标：不接 UI，不改 A 路径，只回答当前 12.00 的 decoder 能力。**

测试矩阵：

1. H.264 3840×2160 8-bit，60 AU 连续样本（本轮实测；不是 30–60 秒长测）。
2. HEVC Main 3840×2160 8-bit，60 AU 连续样本（本轮实测）。
3. HEVC Main10 1920×1080 P010，60 AU 连续样本（本轮实测）。
4. HEVC Main10 3840×2160 未测；因为 PTS/reorder gate 未闭环，按止损规则不进入该项。

每个样本必须记录：

- `QueryDecoderMemoryInfo` 的 cpu/gpu/cpu_gpu/max_frame_size；
- Create/Reset/Decode/Flush rc；
- `out.valid/error/picture_count/codec/width/height/pitch/pitch_bytes/frame_format/buffer_size`；
- frame pool VA 对齐、direct memory 类型、CPU 读测试结果；
- Decode 平均/P95、真实 PTS、输出帧数、丢帧/重排；
- 不做 GPU present 时，不能把 P0 结果写成“可播放”。

**P0 通过条件**：当前固件至少 H.264 4K + HEVC Main 4K 连续解码稳定；如果目标含 10-bit，则还必须得到合法 P010。P0 只通过一帧、连续流或 PTS/reorder 任一项失败，都不能进入生产实现。

### 6.2 P0 实测（2026-10-05）

主机载荷恢复后，8080/2120/2323/3232/8084 均 open。临时标题分别为 `PPSA99260`（AVC）、`PPSA99261`（HEVC Main）、`PPSA99262`（HEVC Main10）；没有改正式 `PPSA99233`。每个流由 FFmpeg 生成 60 个 30fps Annex-B AU，含 `bframes=2`；这是连续解码样本，不是 30–60 秒长测，也没有接 GPU present。

测试流主机侧属性：H.264 High 3840×2160 level 5.2；HEVC Main 3840×2160 level 150；HEVC Main10 1920×1080、`yuv420p10le` level 120。三组均 `aus=60`。

#### 6.2.1 结果矩阵

|样本|Create 配置|输出|连续性/格式|耗时|
|---|---|---|---|---:|
|H.264 4K|codec=1, profile=100, level=52, max=3840×2176, `dpb=-1`, depth=1|60 帧均 `valid=1,error=0,pics=1,codec=1,3840×2160,pitch=3840,pitch_bytes=3840,fmt=0,in_pool=1`; NV12 判定 0|`decoded=60/60, buffered=60, accepted=60, errors=0, format_mismatch=0`|平均 **16,504 us**，P95 **20,195 us**|
|HEVC Main 4K|codec=974921, profile=1, level=153, max=3840×2176, `dpb=-1`, depth=1|60 帧均 `valid=1,error=0,pics=1,codec=974921,3840×2160,pitch=3840,pitch_bytes=3840,fmt=0,in_pool=1`; NV12 判定 0|`decoded=60/60, buffered=60, accepted=60, errors=0, format_mismatch=0`|平均 **6,509 us**，P95 **8,372 us**|
|HEVC Main10 1080p|codec=974921, profile=2, level=123, max=1920×1088, `dpb=-1`, depth=1|60 帧均 `valid=1,error=0,pics=1,codec=974921,1920×1088,pitch=1920,pitch_bytes=3840,fmt=0,in_pool=1`; P010 判定 60/60|`decoded=60/60, buffered=60, accepted=60, errors=0, format_mismatch=0`|平均 **2,477 us**，P95 **3,245 us**|

#### 6.2.2 原始关键日志与内存

```text
PPSA99260 AVC:
sysmodule=0; query_compute=0 size=0x495300; compute_queue=0
query_decoder=0
mem raw cpu=0x3a43800 gpu=0x9505000 cpu_gpu=0x29f1300 frame=0xbf5000 align=256
mem aligned cpu=0x3a44000 gpu=0x9508000 cpu_gpu=0x29f4000 map_rc=0 alloc=1/1/1
pools au=0x1800000 frame=0x23e8000 frame_size=0xbf8000 ok=1/1
create_decoder=0; reset=0
out idx=0 ... 3840x2160 pitch=3840 pitch_bytes=3840 fmt=0 buf=12550144 accepted=1 p010=0 in_pool=1 required=12441600 us=36778
stats decoded=60/60 buffered=60 accepted=60 errors=0 p010=0/60 format_mismatch=0 avg_us=16504 p95_us=20195
pts input=synthetic90k_step3000 output_pts=not_in_abi reorder=not_observable
result pass=1 complete=1 expected_p010=0 decoded=60
```

```text
PPSA99261 HEVC Main:
config codec=974921 profile=1 level=153 max=3840x2176 dpb=-1 depth=1
mem raw cpu=0x271de80 gpu=0xa401500 cpu_gpu=0x4c5cb00 frame=0xbf5000 align=256
mem aligned cpu=0x2720000 gpu=0xa404000 cpu_gpu=0x4c60000 map_rc=0 alloc=1/1/1
pools au=0x1800000 frame=0x23e8000 frame_size=0xbf8000 ok=1/1
create_decoder=0; reset=0
out idx=0 ... 3840x2160 pitch=3840 pitch_bytes=3840 fmt=0 buf=12550144 accepted=1 p010=0 in_pool=1 required=12441600 us=9870
stats decoded=60/60 buffered=60 accepted=60 errors=0 p010=0/60 format_mismatch=0 avg_us=6509 p95_us=8372
pts input=synthetic90k_step3000 output_pts=not_in_abi reorder=not_observable
result pass=1 complete=1 expected_p010=0 decoded=60
```

```text
PPSA99262 HEVC Main10:
config codec=974921 profile=2 level=123 max=1920x1088 dpb=-1 depth=1
mem raw cpu=0x15c1000 gpu=0x51ed500 cpu_gpu=0x1c8d800 frame=0x5fb000 align=256
mem aligned cpu=0x15c4000 gpu=0x51f0000 cpu_gpu=0x1c90000 map_rc=0 alloc=1/1/1
pools au=0x1800000 frame=0x11f4000 frame_size=0x5fc000 ok=1/1
create_decoder=0; reset=0
out idx=0 ... 1920x1088 pitch=1920 pitch_bytes=3840 fmt=0 buf=6275072 accepted=1 p010=1 in_pool=1 required=6266880 us=5321
stats decoded=60/60 buffered=60 accepted=60 errors=0 p010=60/60 format_mismatch=0 avg_us=2477 p95_us=3245
pts input=synthetic90k_step3000 output_pts=not_in_abi reorder=not_observable
result pass=1 complete=1 expected_p010=1 decoded=60
```

#### 6.2.3 判定

- **decoder/内存/输出格式子项：3/3 通过**。当前 12.00 已证明 H.264 4K、HEVC Main 4K、HEVC Main10 1080p/P010 可以连续解码；三组输出 buffer 都落在 frame pool 内，所有 Create/Reset/Decode/Flush rc 和内存映射 rc 均为 0。
- **PTS/reorder 子项：未通过验收，不是“默认通过”**。输入 PTS 是探针合成的 `0,3000,...`，而 ABI 的 `OutputInfo` 没有输出 PTS/显示序号。即使流含 B 帧，当前证据也只能证明 60 AU 被解出，不能证明输出顺序与 DASH 显示 PTS 正确对应。
- **严格 P0 总判定：未完全通过；按止损规则停止 C 的 P1/生产化。** C 的 decoder 能力值得保留，但在另行完成 FFmpeg 时间戳到输出帧顺序的实验前，不得进入 NV12/AGC 管线重构，也不得宣称 4K/10-bit 播放完成。

### 6.3 P1：固定 URL、8-bit NV12、无复杂交互


建议只做一个固定 DASH 视频 URL + 已验证音频 URL：

- FFmpeg demux/BSF → VDEC → 3 槽 NV12；
- mpv audio-only + SDL audio clock；
- 首先接 AGC NV12 pipe，必要时用 B1 两纹理 shader；
- 暂不做清晰度切换、seek、Main10、VP9；
- 保留 A 作为启动/codec/present 任一步失败时的回退。

P1 验收：连续 60 秒，视频 PTS 与 `playback-time` 漂移 <100 ms；无 `Decode/Flush` 错误、无槽复用错误；1080p/4K 画面方向、BT.709 limited range、色彩和 crop 正确；AGC `dcb_full/ring_fail/tex_fail/timeouts` 全 0；UI、弹幕、OSD 不被视频覆盖。

### 6.4 P2：可发布生产

必须补齐：

- H.264/HEVC Main、DASH BSF/AU、真实 PTS/reorder；
- pause/speed/seek/切清晰度/备份 URL/EOF/网络失败；
- HEVC Main10 P010（若 P0 通过）、BT.2020/PQ/HLG tone-map 或明确 SDR tone-map；
- direct frame slot + GPU fence；不确定时复制到自有 ring；
- OSD/弹幕/字幕和 VideoOut 所有权；
- 4K/1080p 长测、内存压力、退出/重启、native fallback 到 A；
- 显示的是视频帧率和 PTS，不只报告 UI loop FPS。

生产验收建议：1080p60 和 4K30/4K60 各至少 10 分钟；HEVC Main10 至少 10 分钟；随机 seek/暂停/倍速/清晰度切换；无崩溃、无黑帧/撕裂、A/V 漂移 <100 ms；默认 profile 的 GPU/AGC health 三项 0；native 不支持的样本自动回退 A。

## 7. 性能、工作量与风险

### 7.1 成本量级

|阶段|工作量（1 人）|能换到什么|
|---|---:|---|
|P0 能力探针|2–4 人日（本轮含三标题、流生成、构建、部署、日志解析）|当前 12.00 的 codec/尺寸/P010/内存/decoder P95 结论；本轮 decoder 子项通过，但 PTS/reorder gate 未闭环|
|P1 8-bit NV12 原型|5–8 人日；连 P0 合计约 7–12 人日|固定视频链路能跑，CPU 解码释放，初步 4K/HEVC 能力；还不能发布|
|P2 生产 8-bit|10–16 人日|A/V、seek、fallback、清晰度、长测完整；可覆盖 H.264/HEVC Main|
|P2 + P010/高质量 AGC|额外 5–10 人日；若需新 AGC shader/toolchain，再加 5–10 人日|10-bit/HDR/高质量 GPU scale；最大不确定性在 pipe 资源和 P010 present|

综合判断：

- **仅要 1080p60**：不值得。A 已交付，C 的网络/同步/内存/VideoOut 风险远大于收益。
- **要 4K/HEVC/CPU 余量**：decoder 能力已经证明值得保留，但严格 P0 尚未通过；先补 PTS/reorder 子项，再决定是否投入 P1。
- **要 4K HEVC Main10 + 高质量 60fps**：仍不能承诺。当前只证明 1080p P010 解码，不覆盖 4K Main10、HDR、GPU CSC、呈现和时钟。

### 7.2 主要风险与放弃条件

1. **当前 decoder 能力没有触发固件止损**：H.264 4K、HEVC Main 4K、HEVC Main10 1080p 均连续通过；但 PTS/reorder 未闭环，严格规则下停止 P1。
2. **P010 只解出但无法显示**：若 P010 `out.valid=1`，但 R16/RG16/AGC pipe 无法正确采样或 tone-map，先交付 8-bit HEVC，10-bit 标为 unsupported；不要把 P010 转回 8-bit CPU 后宣称完成目标。
3. **VideoOut plane 假设错误**：没有 YUV scanout ABI；若 AGC video pipe 无法加载，停止 zero-copy/直写路线，最多做 B1 诊断，不继续挖 undocumented plane。
4. **内存预算**：4K P010 单帧约 24.9 MiB，仅是输出；decoder GPU/CPU workspace、3 槽、AGC transient、UI/图片缓存还要叠加。若 native 标题在 Query/Create 或长测出现 direct/flexible 分配压力，停止 4K Main10，并让 A 处理可回退样本。
5. **时序和槽复用**：出现一次 GPU 采样已开始而 decoder 重用槽、黑帧、撕裂或 seek 后旧帧回屏，P1 不通过；必须回到复制 ring 或显式 fence。
6. **网络/BSF/AU**：seek 后连续 `0x811D0303`、PTS 无法稳定、或 DASH 备份切换不能无崩溃恢复，停止生产化；不允许用“暂停视频/丢帧”掩盖同步错误。

## 8. 明确推荐

1. **现在不改正式 A 路径，不把 C 合入默认播放。**
2. **先不进入 P1。** 需要单独补一个 FFmpeg 时间戳 + B 帧输出顺序实验；该实验通过后才重新评估 C。
3. 未来若 gate 通过，首个原型才做“mpv audio-only + 独立视频线程 + AGC NV12 pipe”，不做 VideoOut YUV plane；AGC pipe 资源缺失时最多用 B1 双平面 shader 验证格式。
4. 默认策略保持 A：native decoder 按 codec/profile/size 探测，失败或不满足门槛就回到 `sws-fast=yes` 的 mpv SW 链路。这样 C 的失败不会破坏当前 1080p60 交付。

## 9. 临时实验清理

本轮保留了 `videodec2_probe.c` 的 P0 统计模式，但仅由 `WILIWILI_TEST_VDEC=1` + `WILIWILI_VDEC_P0=1` 门控；正式标题不触发。三组测试流、options、`PPSA99260/61/62` 本地 dist、`/tmp/*.ffpkg` 与日志均已删除；主机 `/data/homebrew` 已核对只剩正式 `PPSA99233.ffpkg`（另有原有目录），未修改 A 路径，未 push。
