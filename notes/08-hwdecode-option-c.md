# 08 — 方案 C：sceVideodec2 硬解 / NV12-P010 呈现决策研究

> 日期：2026-10-05
>
> 结论先行：C **值得做，但只在目标包含 4K、HEVC、10-bit 或释放 CPU 时值得做**。A 已经解决了当前 1080p SW 面 60 FPS；C 不应作为 A 的替代优化，而应作为一条按能力探测启用、失败回退到 A 的视频后端。
>
> 当前固件是 12.00。P0 已在本机跑过 H.264 4K、HEVC Main 4K、HEVC Main10 1080p：三种 decoder/输出格式能力均通过；PTS/reorder 闸门也已用真实 B 帧流闭环，但调用必须传 `dts=UINT64_MAX`，不能把 decode DTS 传给 `sceVideodec2`。这只允许进入 8-bit P1，不等于 4K60 present/HDR/zero-copy 已完成。

## 1. 决策摘要

|问题|结论|
|---|---|
|C 能否带来 A 没有的能力？|能。硬解路径有机会覆盖 4K、HEVC Main、P010/Main10，并显著释放 CPU；A 只能优化 SW YUV→RGBA/缩放。|
|当前 12.00 能否直接承诺 4K/HEVC/10-bit/60？|decoder 与 PTS/reorder P0 gate、M1 静态 NV12→AGC→scanout、M2 8-bit 固定 MP4 和 M3 已取证的 H.264 B4 DASH 均在真机通过；仍不承诺 4K60、HEVC/P010/HDR、任意 reorder 深度或生产级网络恢复。|
|NV12/P010 能否接现有 AGC？|M1/M3 已验证 GPL-3.0 的预编译 8-bit NV12 pipe：真实 HTTP MP4/B4 DASH 经 FFmpeg BSF/VDEC/NV12 后由 `evo_agc_blit_yuv()` 进入当前 scanout；HDR/planar/upscaler pipe 仍未导入。|
|VideoOut 是否能直接注册 NV12/P010 扫描面？|没有证据，按不可行处理。公开/仓库内 API 只显示单一 RGB/BGRA buffer + 两个 scanout buffer，没有 YUV plane/CSC 注册接口。NV12 必须先由 AGC/GL shader 转成最终 BGRA/RGB scanout。|
|能否保留 mpv？|能保留 mpv 的音频、时钟、pause/speed/property/UI 状态；视频压缩流不能继续让 mpv 读取，否则会重复下载。视频需要独立 FFmpeg demux/BSF → `sceVideodec2` → present。|
|推荐顺序|保留当前 A；受保护 C 的 M3 已完成，默认 pending=8、B4 实测峰值 6；下一阶段只做 HEVC/P010/HDR、4K、网络恢复和更广泛 seek/reorder 覆盖，失败仍立即回退 A。|

## 2. 现状盘点

### 2.1 `videodec2_probe.c` 的事实边界

|能力/字段|状态|证据与含义|
|---|---|---|
|加载 `libSceVideodec2`、compute queue、decoder、reset|**已验证（当前 12.00）**|三次 P0 均为 `sysmodule=0`, `query_compute=0`, `compute_queue=0`, `query_decoder=0`, `create=0`, `reset=0`；探针主流程 `videodec2_probe.c:750-957`。|
|H.264/AVC 4K 8-bit|**已验证（当前 12.00）**|`PPSA99260`：codec=1/profile=100/level=52，60/60 AU，输出 3840×2160 NV12，见 P0 实测节。|
|HEVC Main 4K 8-bit|**已验证（当前 12.00）**|`PPSA99261`：codec=974921/profile=1/level=153，60/60 AU，输出 3840×2160 NV12，见 P0 实测节。|
|HEVC Main10 1080p/P010|**已验证（当前 12.00）**|`PPSA99262`：codec=974921/profile=2/level=123，60/60 AU，输出 1920×1088、`pitch_bytes=pitch*2`，P010 判定 60/60。|
|连续 AU、`Decode` 后 `valid=0` 再 `Flush`|**已验证（当前 P0）**|三组均 `decoded=60/60`, `buffered=60`, `accepted=60`, `errors=0`；旧 P0 路径在 `videodec2_probe.c:950-1019`，PTS 路径在 `:675-748`。当前 `pipeline_depth=1`。|
|输出格式/内存布局|**已验证（当前 P0）**|H.264/HEVC Main 为 `pitch=3840,pitch_bytes=3840`；Main10 为 `pitch=1920,pitch_bytes=3840`。`out.buffer` 三组均在 frame pool 内；顺序探针校验见 `videodec2_probe.c:605-633`。|
|frame pool/direct memory|**已验证为当前配置；不是生产 zero-copy 证明**|三组 direct limit 均 `0x300000000`，type 12 + `0x32/0x33` 映射和 flexible `0x03` 均成功；原始/对齐大小见 P0 实测表。GPU 仍未直接采样这些槽。|
|槽位与 pipeline|**仅探针**|`PIPELINE_SLOTS=3`、`pipeline_depth=1`，见 `videodec2_probe.c:53,675-748`。没有 GPU fence/VideoOut retire 保护，不能当作生产 zero-copy。|
|PTS/reorder|**已闭环（P0 gate，通过 `dts=UINT64_MAX`）**|真实 H.264/HEVC B 帧 sidecar + 输出 Y 指纹：H.264/HEVC normal FIFO display order；H.264 真实 DTS 反例失败，故生产禁止传 decode DTS。完整数据见 §6.3。|
|seek/flush 后 SPS/PPS|**边界已实测，生产仍需严格起播规则**|Flush 尾帧顺序正常；Reset 后 non-IDR 均 `-2128805117`；H.264 IDR/HEVC IDR 19/20 可恢复，HEVC CRA 21 在本样本不可作为安全起点；DASH fMP4 仍必须过 Annex-B BSF。|
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
|HEVC Main 8-bit|**4K 连续解码 + PTS/reorder gate 通过**：3840×2160，60/60 AU，NV12，平均 6.509 ms、P95 8.372 ms；未接呈现|EVO `README.md:53-56`、`docs/codec-support.md:13-20` 的 4K 结论与本机 decoder 方向一致，但不能替代 present 验收|可进入受 watchdog/fallback 保护的 8-bit C P1；仍未证明 4K60 present|
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
4. 这不是运行时 GLSL。`evo_agc_runtime.c` 通过预编译 shader metadata/ISA 调 `sceAgcCreateShader()` + `sceAgcLinkShaders()`；M1 将 `EVO_AGC_HAVE_VIDEO_PIPES` 限定为本地 8-bit NV12 生成头，运行时只注册 `EVO_AGC_PIPE_VIDEO_NV12`；HDR/planar/upscaler 生成文件仍缺失，不能宣称完整 video pipe 集合。
5. `library/borealis/library/lib/extern/nanovg/nanovg_agc.cpp:232-344` 的 NanoVG backend 只把 UI geometry 绑定到 `EVO_AGC_PIPE_UI` 并写 T#/S# descriptor；它没有 GLSL 编译或通用 shader 注入入口。视频应走 runtime 的独立 `evo_agc_blit_yuv()`，不是给 NanoVG image handle 换一个像素格式。

需要改的文件/模块：

- 把 NV12/P010 预编译 pipe metadata/ISA 作为 native build 输入；更新 `evo_agc_pipes.h`，定义并验证 `EVO_AGC_HAVE_VIDEO_PIPES`。不能在标题内临时编译 GLSL。
- 新增 `NativeVideo`/`Ps5NativeVideo` 之类的线程安全 frame seam；由 `VideoView::draw()` 或同等视频绘制点调用 `evo_agc_blit_yuv()`。
- `VideoView::draw()` 目前先画 mpv 视频，再画弹幕/OSD，见 `wiliwili/source/view/video_view.cpp:627-658`。AGC video quad 必须在 OSD 前进入同一 DCB；不能在 `nvgEndFrame()` 后再画，否则会盖住弹幕/OSD。Borealis 帧循环顺序见 `library/borealis/library/lib/core/application.cpp:761-852`。
- 处理 AGC frame slot、VideoOut flip、GPU fence、P010 tone-map/HDR metadata 和视频 PTS。`evo_agc_runtime.h:114-168` 的 frame/slot/cache API 可复用，但当前 mpv SW image 路径没有使用它。
### 4.1.1 AGC 生成 artifact 许可清单

|文件|许可|出处/再生成方式|当前用途|
|---|---|---|---|
|`library/borealis/library/lib/extern/nanovg/agc/shaders/video_yuv_nv12_pipe.h`|GPL-3.0；本项目根树同为 GPL-3.0，组合分发允许|`references/EVO-PLAYER-PS5` 的 `projects/evoplayer/media/shaders/video_yuv_nv12.pipe` 及生成器 `tools/build_agc_pipes.py`；按其 gfx1013 目标重新生成后导入。本仓库没有复制参考播放器 runtime。|M1/M2 的 8-bit NV12 AGC shader metadata/ISA；未用于 HDR/P010。|

Apache-2.0 的 borealis 子模块因此包含一个由 GPL-3.0 项目生成、且明确保留来源说明的 GPL artifact；以后若拆分许可边界，应把该 header 移到 GPL-3.0 根树并由 native build 引用，不在本 M2 中顺手重排子模块。

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

现有 `videodec2_probe.c:1060-1207` 已演示 R8 Y + RG8 UV、片元 shader CSC、crop/flip/U-V swap。它能证明“两个平面可以被当前 GL bridge 采样”，但有三个限制：

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
4. HEVC Main10 3840×2160 未测；P0 仅覆盖 1080p P010，4K Main10/HDR 另立能力测试。

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
- **PTS/reorder 子项：见 §6.3 的真实 B 帧闸门实验**。P0 的 synthetic PTS 只证明了解码路径，不再作为顺序证据。
- **严格 P0 总判定：在规定的 `dts=UINT64_MAX` 调用契约下通过，允许进入 C 的 P1；仍不等于 4K/10-bit 可播放。**

### 6.3 PTS/reorder 闸门实验（2026-10-06）

**样本与方法。** H.264/HEVC 均用 FFmpeg 生成真实带 B 帧的 640×368、30fps、GOP=12 流；用 `ffprobe -show_entries packet=pts,dts` 取得 packet 顺序的真实 PTS/DTS，再转 Annex-B。帧内容是每帧不同的纯色 Y 值，探针从输出 Y 平面中心取 5×5 平均值，只用于识别“输出的是哪一个源帧”，没有把 synthetic PTS 当作证据。样本由 `/tmp/generate_pts_gate.py` 生成，未进入提交树。

每组输入 PTS/DTS 使用 90 kHz packet 值；因为 ABI 字段是无符号，raw DTS 的负值统一加 `1024` 后传入，保持 PTS/DTS 相对顺序不变。关键对照是：真实 DTS 传入时 H.264 会出现错误输出顺序；生产契约应传 `dts=UINT64_MAX`（`WILIWILI_VDEC_DTS_UNKNOWN=1`），并保留真实 DTS 只作诊断日志。

#### 6.3.1 ABI 字段穷尽

权威头文件：`references/EVO-PLAYER-PS5/projects/evoplayer/media/include/sce/sce_videodec2.h`。

|结构|字段（头文件行）|是否能排序/关联输出|
|---|---|---|
|`SceVideodec2InputData`|`size` 122、`au` 123、`au_size` 124|调用描述/压缩数据地址；无输出关联 ID。|
|同上|`pts` 125、`dts` 126、`attached` 127|只有调用方送入的时间戳；服务不会在 `OutputInfo` 回传它们。|
|`SceVideodec2FrameBuffer`|`size` 131、`buffer` 132、`buffer_size` 133|调用方提供的输出槽；槽地址不是显示帧 ID。|
|同上|`accepted` 134、`reserved` 135|只表示槽被接受；`reserved` 含义未验证，不可当序号。|
|`SceVideodec2OutputInfo`|`size` 139|结构版本/大小；无时间信息。|
|同上|`valid` 140、`error` 141、`picture_count` 142|输出有效性/错误/图片数；没有 display order。|
|同上|`padding` 143、`reserved` 148|保留字段；头文件没有定义语义，不能探测性当 POC/frame_id 使用。|
|同上|`codec` 144、`width` 145、`pitch` 146、`height` 147|格式和布局信息；没有 PTS、DTS、POC、frame_id、display index。|
|同上|`buffer` 149–150、`buffer_size` 151|输出图像地址/大小；只能用内容指纹诊断，生产路径没有稳定帧关联字段。|
|同上|`frame_format` 152、`pitch_bytes` 153|输出格式/字节 stride；没有排序信息。|
|`SceVideodec2DecoderConfigInfo`|`max_dpb_frames` 72、`pipeline_depth` 73|影响内部 DPB/管线配置，不会出现在输出帧上；本实验为 `dpb=-1, depth=1`。|
其他 `sceVideodec2` 结构也逐字段复核：`SceVideodec2DecoderConfigInfo` 的 `size/resource_type/codec_type/profile/max_level/max_width/max_height/max_dpb_frames/pipeline_depth/compute_queue/cpu_affinity/cpu_priority/optimize_progressive/check_memory_type/reserved` 均在 64–80 行；`SceVideodec2DecoderMemoryInfo` 的 `size/cpu_size/cpu/gpu_size/gpu/cpu_gpu_size/cpu_gpu/max_frame_size/frame_alignment/reserved` 在 82–93 行；`SceVideodec2ComputeConfigInfo` 的 `size/pipe_id/queue_id/check_memory_type/reserved0/reserved1` 在 95–102 行；`SceVideodec2ComputeMemoryInfo` 的 `size/cpu_gpu_size/cpu_gpu` 在 104–109 行；`SceVideodec2DirectMemory` 的 `size/allocation_size/address/direct_start` 在 114–119 行。它们分别是创建、内存和 compute queue 参数，没有输出图片排序/关联字段。

**ABI 结论：** 当前 SDK/仓库权威头文件没有任何可直接用于输出排序或输入输出关联的字段。唯一可用契约是：caller 送入 PTS，按实测输出顺序建立 FIFO/min-PTS 配对；输出本身不能回读 PTS。

#### 6.3.2 H.264 原始顺序与反例

`PPSA99272` 用真实 packet DTS；normal segment 的输入开头是：

```text
input display/PTS/DTS = (0,0,-1024), (2,1024,-512), (1,512,0), (4,2048,512), (3,1536,1024)
```

输出 Y 指纹对应的 display 序列为：

```text
0,1,3,6,2,5,4,7,9,11,8,10,12,13,15,18,14,17,16,19,21,23,20,22
```

所有输出均 `valid=1,error=0,picture_count=1,640x368,pitch=768,pitch_bytes=768`，但 FIFO/display 对照 `fifo_failures=16`。最早反例：`out_seq=2 matched_display=3 expected_display=2`；`out_seq=3 matched_display=6 expected_display=3`。因此“把输入 PTS 按 display order 排队、输出 FIFO 直接取最小 PTS”在**传入真实 DTS**时不成立。

同一流改为 `dts=UINT64_MAX` 的 ABI 对照标题 `PPSA99276`：normal、true-IDR resume、320×180→640×368（Baseline→High）切档均输出正确 display FIFO；汇总 `outputs=60, errors=7, fifo_failures=0, unknown=0, duplicates=0, mapping_pass=1`。7 个错误全部来自故意的 non-IDR seek 段，不是正常段顺序错误。

#### 6.3.3 HEVC 原始顺序

`PPSA99273`（真实 DTS）和 `PPSA99277`（`dts=UINT64_MAX`）的 normal segment 都是 display FIFO `0..23`，输出 `valid=1,error=0,picture_count=1`，8-bit `640x368,pitch=768,pitch_bytes=768`；两种 DTS 模式的 normal FIFO failures 均为 0。

`PPSA99277` 汇总：`outputs=48, errors=19, fifo_failures=0, mapping_pass=1`。其中 7 个是 non-IDR seek 段；另外 12 个是第二 GOP 的 HEVC CRA（NAL type 21，不是真正 IDR 19/20）起播失败。后续 true-IDR 分辨率段正常输出，说明失败是起播边界而不是 PTS 配对错乱。

#### 6.3.4 B 层级与最小窗口

`vdec-bdepth` 真实流含 `bf=1` 和 `bf=3` 两段；`ffprobe` 确认 H.264/HEVC 均实际产生 B 帧。固定窗口分析定义为：堆积超过 `W` 帧才弹出当前最小 PTS，EOF 继续弹出。

|codec/输入|输出证据|最小额外窗口|
|---|---|---:|
|H.264，真实 DTS，B1|24/24 FIFO，0 failure|0|
|H.264，真实 DTS，B2|normal FIFO failures=16；固定序列分析为 `W=2`|2（但该 DTS 用法不可生产）|
|H.264，真实 DTS，B3|FIFO failures=4；固定序列分析为 `W=1`|1（但该 DTS 用法不可生产）|
|H.264，`dts=UINT64_MAX`，B1/B2/B3|B1/B3 `PPSA99278`、B2 `PPSA99276` 均 display FIFO、0 failure|0|
|HEVC，真实 DTS，B1/B3；`dts=UINT64_MAX`，B2|三组 normal 均 display FIFO、0 failure|0|

真实 DTS 的 H.264 表项是故意的反例：窗口需求随编码结构变化，不能用于生产。按 ABI 传 unknown DTS 后，H.264/HEVC 已覆盖 B1/B2/B3 的实测输出均为 display FIFO，正常路径的最小额外窗口为 0；实现仍可保留 `W=4` 作为监控上限，超出即 fallback，不把不确定帧硬配给 PTS。`max_dpb_frames=-1` 只让 decoder 自动管理 DPB，OutputInfo 没有把实际 reorder 深度回传出来。

配对伪码与失败检测：

```text
for each AU in decode order:
    pending_pts.push(input_display_pts)   # dts = UINT64_MAX
    rc = sceVideodec2Decode(...)
    if rc == 0 and output.valid:
        frame_pts = min_pts(pending_pts)   # normal observed display FIFO; no added reorder delay
        pending_pts.remove(frame_pts)
        require output.error == 0 && picture_count == 1
        require frame_pts >= last_pts
        require pending_pts.size <= 4       # monitor cap; overflow => fallback, never guess
    if output.valid == 0 && rc == 0: drain with Flush
at EOF: Flush until valid == 0; require pending_pts empty
```

生产必须在以下任一情况立即丢弃 native session 并回退 A：Decode/Flush rc 非 0、`error/picture_count` 异常、输出帧数与 accepted AU 不一致、PTS 重复/倒退/无法从 pending 集合配对、Flush 尾帧无法清空、Reset 后首个 AU 不是可随机访问 IDR、分辨率超过 Create 上限或槽生命周期不满足 GPU fence。

#### 6.3.5 seek、Flush/Reset、切档边界

- **non-IDR seek**：H.264/HEVC Reset 后从 display 5 的非 IDR AU 起播，均出现 `rc=-2128805117, valid=0`；必须拒绝该 seek 点并找下一个 IDR。
- **跨 GOP / Flush**：normal stream 跨两个 GOP；正常段最后两帧由 Flush 输出，随后 `empty=1`。`dts=UINT64_MAX` 模式下 display FIFO 保持成立。
- **Reset 后真正 IDR**：H.264 IDR type 5、HEVC IDR type 19/20 后 Reset rc=0，输出顺序/PTS 配对恢复。HEVC CRA type 21 的对照段失败，不能把 CRA 当作安全 seek 起点。
- **分辨率/档位切换**：Reset 后 H.264 Baseline 320×192 padded → High 640×368、HEVC 320×184 → 640×368 均解出；输出 pitch 分别 512/768。切换必须重新注入参数集并从 true IDR 开始；超出 Create 最大尺寸时重建 decoder。

#### 6.3.6 闸门结论

**PTS/reorder 闸门通过，C 可以进入 P1，但必须固定调用契约：真实 display PTS + `dts=UINT64_MAX`，不能把 FFmpeg decode DTS 传给 `sceVideodec2`。** H.264 的真实-DTS 反例证明了该约束不是优化项。P1 仍只做 8-bit NV12、真实 IDR seek、Flush/Reset、PTS watchdog 和失败回退 A；不能把本实验写成已完成 4K60 present、HDR 或 GPU zero-copy。
### 6.4 P1 M1：静态 NV12 → AGC shader CSC → scanout（2026-10-05）

**实现。** 本地新增 `library/borealis/library/lib/extern/nanovg/agc/shaders/video_yuv_nv12_pipe.h`，只复用 EVO-PLAYER-PS5 GPL-3.0 的已生成 gfx1013 ISA/寄存器 artifact；本仓库同为 GPL-3.0。没有复制参考播放器的运行时实现，M1 的门控、静态图案、生命周期和失败检查由本仓库自实现。`evo_agc_pipes.h` 定义 `EVO_AGC_HAVE_VIDEO_PIPES`，但 runtime 只注册 NV12，HDR/planar/upscaler 仍保持关闭。

- `ffmpeg_video_test.cpp` 生成 640×368 BT.601 limited color bars，Y pitch=640、UV pitch=640，调用 `evo_agc_blit_yuv()` 的 staged 路径（`is_direct=0`）。
- `application.cpp` 在 `nvgEndFrame()` 后、AGC `endFrame()` 前调用 M1 hook，仅当 `WILIWILI_TEST_VDEC=1` 且 `WILIWILI_VDEC_AGC=1` 同时存在时启用；该组合跳过 decoder-only P0 启动探针。未开开关时 A 路径不绘制 NV12。
- runtime 新增 pipeline-valid 检查；管线缺失、descriptor/slot 分配失败时返回错误，不向无效 pipeline 发 DCB。

**PPSA99280 真机收据。** 启动日志出现 `AGC runtime successfully initialized`、`agc-m1: first NV12 blit accepted`；截图 `/tmp/m1-screen.png` 显示完整 8 色 NV12 色条已经经 AGC CSC 进入 3840×2160 当前 scanout。约 60 秒运行期间 `agc-m1` 达到 3840 帧，应用 FPS 约 59.9。`agc health` 在 frame=600/1200/1800/2400/3000/3600 均为 `dcb_full=0 ring_fail=0 tex_fail=0 timeouts=0 vo_rc=0`，并有 `presents=600`、`flip_waits=600`；日志无 `img-net: failed`、crash 或 blit failure。

**M1 判定：通过。** 缺失的不是 AGC shader 工具链，而是本地缺少 NV12 生成 artifact；复用 GPL-3.0 artifact 后，现有 runtime/writer/transient ring/三帧 DCB/fence/VideoOut 链路已在真机打通。M1 只证明 staged 静态 NV12 的 CSC/scanout，不证明 VDEC direct-memory zero-copy，也不证明 UI/OSD 合成顺序；当前 probe 故意在 UI 后覆盖全屏，M2 必须把视频 quad 接入 OSD 之前的正式绘制点。

**下一步。** 进入 M2：固定 URL 的 FFmpeg demux/BSF → VDEC → 三槽 NV12，保留 display PTS + `dts=UINT64_MAX`、pending watchdog、fence/slot 所有权和 native→A 回退；M3 尚未开始。

### 6.5 P1 M2/M3：固定 URL、8-bit NV12、无复杂交互

建议只做一个固定 DASH 视频 URL + 已验证音频 URL：

- FFmpeg demux/BSF → VDEC → 3 槽 NV12；
- mpv audio-only + SDL audio clock；
- 首先接 AGC NV12 pipe，必要时用 B1 两纹理 shader；
- 暂不做清晰度切换、seek、Main10、VP9；
- 保留 A 作为启动/codec/present 任一步失败时的回退。

P1 验收：连续 60 秒，视频 PTS 与 `playback-time` 漂移 <100 ms；无 `Decode/Flush` 错误、无槽复用错误；1080p/4K 画面方向、BT.709 limited range、色彩和 crop 正确；AGC `dcb_full/ring_fail/tex_fail/timeouts` 全 0；UI、弹幕、OSD 不被视频覆盖。
#### 6.5.1 M2 实现设计（真机收据待补）

- `scripts/ps5/native/native_vdec_play.c` 启动一个 FFmpeg demux/BSF 线程；根据 `AVCodecParameters` 建立 H.264/HEVC `h264_mp4toannexb`/`hevc_mp4toannexb`，每个 BSF packet 作为一个 AU，输入 PTS 从 stream time base 换算到 90 kHz，并以首 PTS 做零基准。每个 `sceVideodec2Decode` 都传该 display PTS，`dts` 固定为 `UINT64_MAX`。
- decoder 配置使用 `max_dpb_frames=-1`、`pipeline_depth=1`；输出 frame pool 和 AU pool 各 3 槽。槽状态为 `FREE → INFLIGHT → READY → CURRENT`，VideoView 只消费 READY 的最小序列；`evo_agc_blit_yuv(..., is_direct=0)` 同步把 NV12 stage 到 AGC transient ring。`Application::frame()` 在 `AgcVideoContext::endFrame()` 返回、DCB fence 已 retire 后调用 `wiliwili_vdec_play_frame_retire()`，当前槽才允许被解码线程复用。
- pending PTS 上限为 4；输出用 pending 集合的 min-PTS 配对，要求 `error=0`、`picture_count=1`、8-bit pitch/尺寸/地址均有效、输出 PTS 严格单调、`output_count == accepted_count`。Flush 最多 32 次，遇到 rc/error、尾帧不清空、计数不一致或 pending 非空即回退 A。
- 额外看门狗：单次 Decode/Flush 默认 250 ms，可由 `WILIWILI_VDEC_TIMEOUT_MS` 调整；首个 AU 必须含 H.264 IDR 5 或 HEVC IDR 19/20。`WILIWILI_VDEC_INJECT=bad-stream|reset|window|timeout` 分别覆盖坏 AU、30 帧后 Reset、pending 超窗和解码超时，所有注入都记录 `FALLBACK_A reason=...`。
- `VideoView::draw()` 在 mpv 视频绘制位置先调用 native NV12，再继续 NanoVG 弹幕/OSD；native 活跃时给 mpv 当前 file 设置 `vid=no`，失败或 stop 时设置 `vid=auto`，所以正式 A 仍由同一个 VideoView 绘制。pause/resume 同时唤醒/阻塞 native demux 线程。M2 原型只验证全屏 AGC quad；`evo_agc_blit_yuv` 当前没有矩形 x/y 参数，非全屏布局仍是后续边界。

固定流测试入口：`WILIWILI_TEST_VDEC=1` + `WILIWILI_VDEC_PLAY=1` + `WILIWILI_TEST_BV=<BVID>`；可用 `WILIWILI_VDEC_URL=<video.m4s>` 覆盖 API 返回的视频 URL，`WILIWILI_VDEC_AUDIO_URL=<audio.m4s>` 覆盖 mpv 的音频 URL。测试选项只写入 `/tmp/*-options.txt`，不进入 `resources/`。

**M2 真机结果：通过（PPSA99290）。** 测试流是 `/tmp/m2-nob.mp4`，由 `testsrc2 640x368@30 + sine 48k` 生成，`libx264 -bf 0 -g 30 -pix_fmt yuv420p + AAC`，通过 `http://192.168.100.7:18080/m2-nob.mp4` 提供；`ffprobe` 确认 H.264 8-bit、`has_b_frames=0`、时长 180 s。样本和选项均在 `/tmp`，没有进入 `resources/`。
生成命令：`ffmpeg -f lavfi -i testsrc2=size=640x368:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 180 -c:v libx264 -profile:v high -level 3.1 -bf 0 -g 30 -pix_fmt yuv420p -c:a aac -b:a 96k -movflags +faststart /tmp/m2-nob.mp4`。

- 启动日志：`vdec-play: demux ready ... codec=1 size=640x368 ... bsf=h264_mp4toannexb reorder=0`、`flush_each_decode=1`、`dts=UINT64_MAX`；`decoder ready frame_size=0x6c000`。
- 约 180 s 真机运行，`inputs=5400 accepted=5400 outputs=5400`，PTS 从 0 到 `16,197,000`（90 kHz）单调；无 `FALLBACK_A`、无 crash、无 `img-net: failed`。`agc health` frame=600/1200/1800/2400/3000/3600/4200/4800/5400/6000/6600/7200/7800/8400/9000/9600/10200 均为 `dcb_full=0 ring_fail=0 tex_fail=0 vo_rc=0`，且 `ring_fail/tex_fail/timeouts` 全 0。health 时间间隔约 10 s，UI/present cadence 约 60 Hz；源视频为 30 fps，M3 仍需用 mpv clock 做正式发布节拍。
- `/tmp/m2-screen.png` 真机截图显示 AGC 彩色视频在底层，弹幕、播放器控件和右侧 UI 在其上，绘制位置已与 `VideoView::draw()` 正式视频层一致。M2 的 AGC API 是全画布 quad；非全屏矩形仍不宣称支持。

**B 帧边界。** 同一 B 站 DASH H.264 流（`has_b_frames=4`，PPSA99288/PPSA99289）按 pending 上限 4 和 verified `dts=UINT64_MAX` 契约运行；它在首个输出前达到 pending 窗口或 VDEC Flush 错误，按设计记录 `FALLBACK_A`，未计入 M2 通过样本。该结果保留了“超窗/解码错误立即回 A”，后续若要覆盖 B4 真实流必须先扩展已验证 PTS/VDEC 窗口，不能放宽此 gate。

**故意失败注入。** 每项均使用本地 HTTP 真实 MP4，启动后回到 A，日志没有崩溃：

|标题|注入|结果|
|---|---|---|
|`PPSA99291`|`WILIWILI_VDEC_INJECT=bad-stream`|`FALLBACK_A reason=injected-bad-stream rc=-9015`；mpv audio active。|
|`PPSA99292`|`...=reset`|30 帧后 `injected midstream reset rc=0`，随后 `FALLBACK_A reason=injected-reset-failure rc=-9017`。|
|`PPSA99293`|`...=window`|`FALLBACK_A reason=pending-window-injected rc=-9016`。|
|`PPSA99294`|`...=timeout`, `WILIWILI_VDEC_TIMEOUT_MS=10`|`FALLBACK_A reason=decode-timeout rc=-9008`；mpv audio active。|

#### 6.5.2 M3：时钟、交互与真实 B4（2026-10-06）

- **发布节拍**：`VideoView::draw()` 把 mpv `playback-time`（90 kHz 换算）、`speed`、`paused` 传给 native；READY 队列按 PTS 升序取第一个不晚于 `clock90k` 的帧，过期 READY 计入 `dropped`，CURRENT 由 AGC fence-retire 后复用。PPSA99369 的无重排流 clock 与视频 PTS 保持毫秒级，`dropped` 只在切档前旧流尾部出现，不再出现解码线程跑满后重复末帧。
- **EOF/音频重载**：PPSA99367 在 180 s 素材结束时记录 `mpv: end file reason=0`、`audio active`，随后 native 检出 `old90k=16174081 new90k=0`，执行 `seek reset rc=0 reopen=1`，重新打开 demux/BSF，首帧再次为 `pts90k=0`；重播后持续输出，无 `FALLBACK_A`、crash 或 `img-net: failed`。EOF 后不再复用旧 READY 队列；普通 seek 仍复用当前 demuxer。
- **pause/resume**：PPSA99369 日志 `m3-test: pause` → `m3-test: resume`；demux/解码线程在条件变量上停读并唤醒，音频 active、health 0、无 crash/FALLBACK_A。
- **倍速**：PPSA99369 记录 `m3-test: speed=2.0`、`clock playback=6.579 speed=2.000`，随后 `speed=1.0`、`clock playback=18.889 speed=1.000`；发布节拍跟随 mpv clock，不使用独立 wall-clock。
- **seek**：PPSA99369 的 `m3-test: seek target=27.860` 实际按整数秒请求 27，日志为 `seek reset target=27.000 rc=0`、`generation=1 target90k=2430000`；首个新 AU `pts=414720 dts=414720 key=1 idr=1`，首个 output `seq=1 pts90k=2430000`。路径为停读→`sceVideodec2Reset`→`avformat_seek_file`→BSF flush→清空槽/PTS→等待 true IDR；HEVC CRA type 21 不满足 IDR gate。
- **清晰度切换**：PPSA99369 的 `m3-test: quality-switch` 停止旧 native 会话并先 `MPVCore::reset()` 清零旧 playback-time，再起 `320x184` native decoder；首 AU 重注入 SPS/PPS，`frame_size=0x28000`，切换后 health 0、无 fallback/crash。`MPVCore::reset()` 只在双门控 native 实验路径调用，A 路径不变。

**真实 B4 DASH 取证。** PPSA99353（`WILIWILI_VDEC_PENDING_LIMIT=4`）显示 `video_delay=4 has_b_frames=4`、`reorder=1`、`dpb=-1 depth=1`；BSF AU 边界正常，首段为 `PTS/DTS 0/-1600、24030/-1072、11970/-528、6030/0、2970/528、9000/1072、18000/1600`（90 kHz），首 AU 带 SPS/PPS/IDR。前四 AU 后输出 `valid=1 error=0 picture_count=1`，第 5 AU decode 触发 `rc=-2128805632`，`FALLBACK_A`。
- PPSA99354/99360 用 `pending_limit=8` 实测 pending 稳定峰值 6，`output seq/pts90k` 单调且无回退；`health` 检查点三项 0。根因是该真实 B4 的 VDEC reorder/输出延迟达到 6，不是 DASH 分片边界或 `h264_mp4toannexb` AU 切分。
- **pending 决策：默认保持 4。** P0 的 H.264/HEVC unknown-DTS B1/B2/B3、无 B 帧固定 MP4 均不需要额外窗口；真实 B4 仅在显式 `WILIWILI_VDEC_PENDING_LIMIT=8` 下通过一条已测流，不能据此把 8 宣称为所有流的安全默认值。最终代码 PPSA99370 不设置覆盖项，启动日志 `pending_limit=4`，同样在 AU index=4 后 `rc=-2128805632`→`FALLBACK_A`；回退后 health frame=600…4200 的 `dcb_full/ring_fail/tex_fail/timeouts/vo_rc` 全 0。数组容量仍为 8，实验旋钮允许 4–8；默认 4 遇到该 B4 按设计回退 A。

**M3 长测（最终代码）。** PPSA99371 使用 `/tmp/m3-long.mp4`（660 s、640x368、H.264 8-bit `bf=0`、AAC）真机运行约 10 分 50 秒（监听到 650 s）；`presented=1…38400` 全部 `dropped=0`，末端 `clock90k=57703169`、视频 `pts90k=57702000`（约 640 s），无提前 EOF、无末帧空转、无 `FALLBACK_A`、无 crash、无 `img-net: failed`。`agc health` 从 frame=600 覆盖至 frame=38400，所有检查点 `dcb_full=0 ring_fail=0 tex_fail=0 timeouts=0 vo_rc=0`；direct_mem 为 `55,282,944` bytes（约 52.7 MiB）并保持平稳，峰值相同。启动日志确认最终默认 `pending_limit=4`。

**M3 故障注入（当前代码）。** PPSA99356 bad-stream→`rc=-9015`；PPSA99357 中途 Reset `rc=0` 后→`rc=-9017`；PPSA99358 超窗→`rc=-9016`；PPSA99359 timeout（10 ms）→`rc=-9008`。四项各一条 `FALLBACK_A`，均有 mpv `audio active`、health 0、无崩溃。

**当前 C 可用性判定。** 默认双门控 native 路径已在 M5 真实 10 条 B 站语料上完成自适应重判：H.264/HEVC 360/480/720/1080p、DASH/普通 MP4 均零回退、零顺序异常；普通 1080p 短 AVCC 修复见 §6.5.4。当前默认已晋升自适应；未知/不一致策略仍安全回 4，失败仍回 A。P010、4K 全屏已各有本地收据；非全屏 crop、原生 P010 pipe/HDR、网络中断恢复、随机 seek/EOF 压力与生产级漂移统计仍未完成。

#### 6.5.3 M4：按流重排信息自适应 pending

- **策略实现**：FFmpeg `AVStream::codecpar->video_delay` 与探针 `AVCodecContext::has_b_frames` 均已知且一致时，实验自适应采用 `limit = video_delay + 4`（无重排取 floor 4），硬上限 12；未知、负值、字段不一致或异常过大回退 4。初版 `video_delay + 2` 在 PPSA99401 的真实 B4 上取 limit=6，于第 7 个 AU 触发 VDEC `rc=-2128805632`；`+4` 是由真实 B4 pending 峰值 6–7 的窗口证据导出的余量。
- **M4 当时的安全默认（历史）**：由于当时 PPSA99410 尚未修复，M4 结束时默认仍是 `pending_limit=4`、只有显式 adaptive；M5 §6.5.4 已修复该条，当前晋升判定见 §6.5.5。
- 启动日志记录 `pending policy`、实际 `limit`、`video_delay`、`has_b_frames`、理由；呈现/FALLBACK_A/EOF 日志记录 `limit`、pending 峰值、输入/接受/输出、error/order_errors。既有 min-PTS、`error=0`、`picture_count=1`、严格递增 PTS、计数一致、Decode/Flush 超时和 Flush 上限安全网不变。

**M4 真实 B 站语料（每条监听约 75 s；URL 只作当次签名来源，复现使用 BV/cid/清晰度）。**

|标题|BV/cid、内容|格式/探针|实验 limit、pending 峰值|结果|health/direct_mem|
|---|---|---|---|---|---|
|PPSA99413|`BV1Da411Y7U4` / `584421165` / 360p|H.264 DASH B4，`video_delay=4 has_b_frames=4`|8、7|显式 `WILIWILI_VDEC_ADAPTIVE_PENDING=1` 通过；`presented=4200`，`dropped=0`，error/order=0，无回退|8 个 health 全 0；峰值 55,276,544 bytes|
|PPSA99403|`BV1MSHY6eEq9` / `42417522439` / 480p|H.264 DASH B3，`video_delay=3 has_b_frames=3`|7、5|通过；`presented=4200`，`dropped=0`，error/order=0，无回退|8 个 health 全 0；峰值 3,882,752 bytes|
|PPSA99405|`BV1AM4y1M71p` / `364849402` / 720p|普通 HTTP MP4，H.264，`video_delay=4 has_b_frames=4`|8、5|通过；`presented=4080`，`dropped=0`，error/order=0，无回退|8 个 health 全 0；峰值 55,312,640 bytes|
|PPSA99417|`BV1MSHY6eEq9` / `42417522439` / 480p|HEVC Main DASH，`video_delay=4 has_b_frames=4`|8、5|最终代码显式 `WILIWILI_VDEC_ADAPTIVE_PENDING=1` 通过；`presented=3960`，`dropped=0`，error/order=0；pause/resume、2x→1x、seek（demux rc=-5 后 sequential-forward→true IDR）、EOF replay 均继续输出|10 个 health 全 0；峰值 55,723,008 bytes|
|PPSA99410|`BV1J7411374q` / `151345597` / 1080p|普通 HTTP MP4，H.264，`video_delay=2 has_b_frames=2`|6、5|反例；约 60 s 后 BSF `rc=-1094995529`，`FALLBACK_A`，此前 `presented=3480`、`dropped=0`|health 全 0；峰值 3,461,376 bytes|

M4 的实验集历史上是 4 条通过/1 条回退，因此当时不晋升；M5 已用登录态复核并修复 1080p 取流问题，当前真实 10 条集与默认判定见 §6.5.5。

#### 6.5.4 M5：1080p 普通 MP4 BSF 回退根因与修复（2026-10-06）

- **A/mpv 对照**：PPSA99506 在同一 `BV1J7411374q`、同一 1080p 普通 MP4 URL 上绕过 C，直接让 mpv 播放约 80 s；日志有 `m5-test: direct mpv url override`、`mpv: file loaded`、`audio active`，8 个 health 检查点均为 `dcb_full=0 ring_fail=0 tex_fail=0`，无回退。流本身正常。
- **失败证据**：PPSA99501 的失败包为视频包 index=1494、`pos=18830711`、PTS/DTS=`959360/956160`、flags=`0x2`、收到 `size=10238`；AVCC 首个长度字段声明 `11174`，因此包内 NAL 不完整。前一包 index=1493 为 `pos=18829542 size=492`、NAL type 1、payload 488；同一完整本地样本 `/tmp/m5-1080.mp4` 在 pos=18830711 的包大小为 11178，完整 FFmpeg `h264_mp4toannexb` 通过。失败前没有 SPS/PPS/SEI 或参数集切换，BSF 只是正确拒绝截断的 AVCC 包。
- **根因**：B 站 HTTP VOD 连接在 MP4 sample 尚未读完时返回短读/EOF；交叉 FFmpeg 7.0.1 的 HTTP/AVIO 路径在该连接关闭形态下向 MOV demux 暴露了短 packet，随后 `h264_mp4toannexb` 校验 `11174 > remaining` 返回 `AVERROR_INVALIDDATA`。不是 AVC3、带内参数集、多 SPS/PPS、AU 边界或 B 帧排序问题。
- **修复**：保留 `reconnect=1`、`reconnect_on_network_error=1`、`multiple_requests=1`；不启用全局 `reconnect_at_eof`，避免真实 EOF 被当成重连。C 在送 BSF 前校验 AVCC/AnnexB 包完整性；发现短包时重开 HTTP demux/BSF、按原 DTS/PTS 定位并读取同一 byte position，失败仍以 `FALLBACK_A` 收口。诊断日志保留最后 8 个包的 type/size/offset/PTS/DTS/声明长度。
- **修复后重跑**：PPSA99509 同一 1080p BV，显式 adaptive，监听 90 s；无 `packet-short`、无 BSF/FALLBACK_A，`presented=5040`（监听末端）、`dropped=0`、`errors=0`、`order_errors=0`、limit=6、pending peak=5；health frame=600…4800 全 0，direct_mem `4,247,808` bytes 稳定，mpv audio active，无 `img-net: failed`。

#### 6.5.5 M5 登录态、真实语料与能力扩展（2026-10-06）

**登录态口径。** 原生标题的会话文件是 `/download0/wiliwili/config/wiliwili_config.json`；配置代码路径为 `config_helper.cpp:1165/1183`。此前 `PPSA99xxx` 临时标题的 `download0` 只有匿名 `DedeUserID=0`，不能作为 1080p 证据。登录后在 `PPSA99233` 的配置元数据中确认存在 `SESSDATA`、`bili_jct`、用户 ID 等字段；测试取流时由 `native_vdec_play.c` 仅在 B 站媒体域读取该文件并生成 Cookie header，日志只写 `bili-cookie=config fields=N`，不写值。options 只含 BV/本次签名 URL，不含 Cookie；Cookie 未写入仓库、文档、语料表、提交或回报。

**真实 B 站流。** 下表只记录 BV/cid/清晰度/编码/容器；每条监听至少约 60 s，表中 `presented` 为监听末端，`dropped/errors/order_errors` 均为 0，无 `FALLBACK_A`。H.264/HEVC 1080p 两条均通过应用登录态查询到 DASH 1080p 轨道，并在 `PPSA99233` 中验证。

|BV / cid|清晰度|编码|格式|收据|结果|
|---|---:|---|---|---|---|
|`BV1Da411Y7U4` / `584421165`|360p|H.264|DASH|PPSA99521，presented=2880，limit=8/peak=7|通过|
|`BV1MSHY6eEq9` / `42417522439`|480p|H.264|DASH|PPSA99522，presented=2880，limit=7/peak=5|通过|
|`BV1MSHY6eEq9` / `42417522439`|480p|HEVC Main|DASH|PPSA99417，presented=3960，limit=8/peak=5|通过|
|`BV1AM4y1M71p` / `364849402`|720p|H.264|普通 MP4|PPSA99512，presented=3000，limit=8/peak=7|通过|
|`BV1Pq4y1M7kY` / `388696134`|720p|H.264|普通 MP4|PPSA99516，presented=2880，limit=8/peak=7|通过|
|`BV1h8411D7Me` / `1201228023`|720p|H.264|普通 MP4|PPSA99517，presented=2880，limit=8/peak=7|通过|
|`BV1J7411374q` / `151345597`|1080p|H.264|DASH|PPSA99233，presented≥3720，limit=8/peak=7|通过|
|`BV1MSHY6eEq9` / `42417522439`|1080p|HEVC Main|DASH|PPSA99233，presented≥3720，limit=8/peak=5|通过|
|`BV1J7411374q` / `151345597`|1080p|H.264|普通 MP4|PPSA99514，presented=3000，limit=6/peak=5|通过|
|`BV1X4411Z71A` / `111316028`|1080p|H.264|普通 MP4|PPSA99518，presented=2880，limit=6/peak=5|通过|

上述 10 条真实 B 站流构成原有自适应重判集：零回退、零顺序异常；其中 1080p DASH H.264/HEVC 已纳入。用户确认当前账号无大会员，真实 60fps 与 4K 清晰度不可得；本轮不把 30fps/低档降级流冒充高档。真实 HEVC Main10 也未取得可独立验收的 B 站轨道；对应工程验证与真实语料分开记录。普通 MP4 1080p 反例的根因和修复见 §6.5.4，旧失败不是当前结果。

**默认晋升。** 在上述通过集上，`video_delay`/`has_b_frames` 一致时使用 `delay+4`，无重排 floor=4，未知/不一致仍安全回 4、硬上限 12。由于当前真实集满足零回退/零顺序异常，代码默认改为 adaptive；`WILIWILI_VDEC_FIXED_PENDING=1` 仅保留作反例实验旋钮。双门控关闭时 A 路径不变；失败仍 `FALLBACK_A`。

**本地样本与能力扩展（不计入真实语料集）。**

|样本|属性|结果|
|---|---|---|
|`/tmp/m5-main10.mp4`|本地 HEVC Main10/P010，640×368，12 s|PPSA99233 `p010=1`，当前走 staged P010→NV12；早期正常播放收据 `accepted/outputs` 连续、`dropped=0/errors=0/order_errors=0`；这是 SDR 8-bit 转换验证，不是原生 P010 shader/HDR。|
|`/tmp/m5-4k.mp4`|本地 H.264 3840×2160，8 s|PPSA99233 adaptive `limit=6`，240 AU 全部解码/Flush、`dropped=0/errors=0/order_errors=0`；截图 `/tmp/m5-4k-adaptive-screen.png` 显示色条、弹幕和 OSD，证明本地 4K 源在 1080p scanout 上全屏 present。|
|`/tmp/m2-nob.mp4`|本地 H.264 640×368、无 B 帧，180 s|PPSA99290，M2 基线通过。|
|`/tmp/m5-4k-43.mp4`|本地 H.264 2880×2160（4:3），30 s|PPSA99233 adaptive `limit=6`，Fit 与 Crop 均连续 present，`dropped=0/errors=0/order_errors=0`、health 三项 0；截图分别为 `/tmp/m5-99233-4k-43-screen.png` 与 `/tmp/m5-99233-4k-43-crop-screen.png`。|

`evo_agc_blit_yuv_rect()` 现在接收 VideoView 实际 rect 和播放器 aspect mode：默认/固定比例为 Fit（黑边），`-2` 为 Stretch，`-3` 为 Crop；非全屏路径关闭 upscaler，并以 rect scissor 绘制。PPSA99233 的 3840×2160 小 rect 截图 `/tmp/m5-99233-4k-switch-screen.png` 通过；4:3 Fit/Crop 截图显示视频尺寸变化与裁切，弹幕/OSD 均在同一 view rect 上绘制，health 三项 0。

**P010/网络/压力状态。** P010 SDR 仍走稳定的 P010→NV12 staged 转换。新增 GPL-3.0 `video_yuv_p010_hdr_pipe.h`（来源 `references/EVO-PLAYER-PS5`，由 `tools/build_agc_pipes.py` 生成）并接入 `EVO_AGC_PIPE_VIDEO_HDR`：本地合成 PQ P010 `/tmp/m6-hdr-p010.mp4` 的 `trc=16` fresh 收据为 native `presented` 连续至 `inputs=240 accepted=240 outputs=240`，无 fallback/error/order/drop，health frame=600…6000 三项 0；该 pipe 是 PQ→SDR，不是 HDR10 输出信号。现有静态 P010 smoke 仍走 staged SDR 转换。PPSA99233 默认 adaptive 本地 4K smoke 通过，`policy=adaptive limit=6`、health 三项 0。播放中断收据为 PPSA99233：长源服务中途停止后 `FALLBACK_A reason=demux-error rc=-5`，随后 mpv audio active，health 三项仍为 0，无 crash。短样本 Range 压力已完成 seek=20/20、EOF=5/5，`EOF inputs=600 accepted=600 outputs=600`、无 error/order/drop，health 三项 0。当前 fresh C 长测在约 197 s、5850 inputs 后仍 `FALLBACK_A reason=flush-timeout rc=-9008`；失败前漂移 96 点 `p50=27.83 ms p95=72.02 ms max=84.02 ms mean=30.54 ms`；`direct_mem` 从 3.19 MiB 到 6.14 MiB，按完整 197 s 窗口端点斜率约 `0.90 MiB/min`，约 61 s 后进入平台，峰值分配 52.65 MiB；health 26 点全 0。C ≥30m 仍不通过，历史 A 长测继续稳定。

#### 6.5.6 M7：native 视频 rect 坐标回归（2026-10-07）

用户提供的 `/tmp/pos-now.png`（1280×720）显示 native 小窗只占左上约 520×290，右侧/下方为 `brls/clear` 浅灰。根因不是 `vid=no` 跳过页面 UI：`VideoView::draw()` 在 native draw 后仍执行 progress、danmaku、OSD、subtitle；`Application::frame()` 也照常遍历并提交全部 Activity。根因是坐标系错配：AGC SDL 固定 `window=1920×1080`、`content=1280×720`、`windowScale=1.5`；A 路径保存未缩放 NanoVG content rect，由 `nvgScale(windowScale)` 变为物理顶点，native raw DCB 却把同一 `x/y/w/h` 直接当 scanout 像素传给 `evo_agc_blit_yuv_rect()`。因此 800×450 逻辑小窗被画成 800×450 物理像素，截图约为预期的 2/3。

修复位于 `wiliwili/source/view/video_view.cpp`：仅 native AGC 调用前将 VideoView content rect 乘 `brls::Application::windowScale`，再传入 `wiliwili_vdec_play_draw()`；新增一次性 rect 诊断行，记录 logical/physical/scale/mode。A 路径仍把未缩放 rect 交给 `MPVCore::draw()`，未改变。`evo_agc_blit_yuv_rect()` 原本就按 scanout viewport/scissor 工作，Fit(0)/Crop(1)/Stretch(2) 的比例计算也按目标 rect 工作，无需再次换算；AGC 注释现明确该物理坐标契约。

回归归因：真正引入 rect 参数的是 root `cdf0a0a` 与 borealis `81e0f0df` 的 native destination-rect 接线；`a04bc92` 只增加 Fit/Stretch/Crop mode 参数，不是尺寸缩小的引入点。rect 缩放修复后，后续真机复现又确认全屏 clone 的 native 状态/decoder 所有权需要单独修复，详见下两段；最终包保留 hw 双门控。

本轮针对用户报告的“主页选卡片无反应”做了干净重启和对照：`/tmp/m7-c-home.png` 显示主界面正常；稳定输入 `right → down → cross` 后，最终 hw 包取得 `/tmp/m7-final3-small.png`，并在日志中进入 `vdec-play: start`/`decoder ready`。A 对照 `/tmp/m7-a-after-cross.png` 与仅开 `WILIWILI_VDEC_PLAY` 的门控对照 `/tmp/m7-play-only-after-cross.png` 也都能打开详情。第一次未进入的截图发生在被中断的 PeaSyo 输入会话中，后续干净会话可稳定复现成功；未发现 `setUrl`/`mpvCore->reset()` 重入或重试风暴，未改该链。

同一复现进一步暴露了真实的全屏回归：`setFullScreen(true)` 新建第二个 `VideoView`，其 `native_vdec_mpv_suppressed` 默认为 false，而共享 mpv 当前仍是 `vid=no`，所以 pre-fix `/tmp/m7-final-hw-full.png` 全黑并伴随 `mpv-sw: surface=81`。修复在 `video_view.cpp` 将 native 抑制状态复制给全屏 clone，并用 `native_vdec_play_owner` 防止 clone 析构停止原始 singleton decoder，同时保持 A clone 的旧 stop 语义。post-fix `/tmp/m7-final3-full.png` 全屏有画面，`/tmp/m7-fixed-exit-small.png` 退出全屏后仍继续 native 播放；日志收据为小窗 `logical=10,10 800x450 → physical=15,15 1200x675`、全屏 `logical=0,0 1280x720 → physical=0,0 1920x1080`，health 三项持续 0、FPS 约 60。最终交付包 build marker 为 `Oct 7 2026 08:31:41`。
#### 6.5.7 M8：C watchdog 遥测、长跑与备用切换（2026-10-07）

**根因判定。** M6 的失败包选项含 `WILIWILI_VDEC_TRACE_AU=1`；旧实现把 `Decode + Flush + 诊断 UDP 日志` 的组合时间一起与 250 ms 比较，并把超时归因为 `flush-timeout`。新 `/tmp/m7-c-trace-cycle.log` 在相同 trace_au 压力下运行约 260 s、无 `FALLBACK_A`：`combined-watchdog-would-fire` 的 `call_us` 为约 0.52–0.95 s，但 `operation_us` 仅约 1.3–4.3 ms，Decode/Flush 各自均远低于 250 ms；这是真实的 watchdog 计时边界/日志阻塞假象，不是 VDEC、槽位、AGC fence 或 demux 卡住。默认超时保持 250 ms，不用增大掩盖问题。

**遥测与修复。** `native_vdec_play.c` 现在分开统计 Decode 与 Flush 的 p50/p95/p99/max，每 600 次 Decode 输出一行；失败时记录前后 8 个 AU 的 pts/dts/IDR/pending/槽位、direct_mem、seek/EOF/reopen、demux/槽位/fence 等待。watchdog 只检查单次 ABI Decode 或单次 ABI Flush；slot wait、demux、`play_logf` 均不在该门槛内。`WILIWILI_VDEC_TRACE_LATENCY=1` 额外输出 PTS-clock 漂移和 direct_mem。

**失败上下文收据。** `/tmp/m7-failover-v2-cycle.log` 的受控中途断流先触发短包失败：`reason=demux-short-packet rc=-9025`，失败上下文为 `au=2215 slot=1 pending=0 demux_last_us=1004235 demux_max_us=1004235 direct_mem=48857088 slot_max_us=1740991 present_max_us=22194`；前后 AU 2208…2215 的 pts90k 为 `154674000…154695000`，均非 IDR、槽位按 `READY` 周期复用。它证明 demux/短包失败会被干净记录并回 A；旧 197 s flush-timeout 本身没有这些字段，新的 trace_au 回归运行则只出现上述 combined-watchdog 诊断而没有回退。

**失败驱动备用 URL。** `VideoView::setBackupUrl()` 现在把视频 backup URL 注册到 C；也支持 `WILIWILI_VDEC_BACKUP_URL`。demux read/短包失败时，C 等待 present retire、Reset、重开 backup、按旧 target90k seek 并等待新 IDR；解码器/AGC 失败仍 fail-closed 到 A。`/tmp/m7-failover-v3-cycle.log` 使用服务 `/tmp/m7-failover-server-v3.log`：primary 在 8 MiB 后断流并返回 503，日志为 `backup-switch begin index=0 rc=-9025 target90k=173325000` → `success ... seek_rc=0`，之后继续 C present 至 6000+，无 `FALLBACK_A`；health 三项/timeout 全 0。

**30 分钟 C 长测。** `/tmp/m7-c-30m-cycle.log` 使用本地 `/tmp/m5-33m.mp4`、`WILIWILI_VDEC_TRACE_LATENCY=1`，监听 1794 s（约 29.9 min），build marker `Oct 7 2026 13:09:31`。89 个完整延迟窗口中 Decode 为 p50 `1.312–1.340 ms`、p95 `1.765–1.878 ms`、p99 `4.061–4.196 ms`、max `4.133–5.463 ms`；Flush 为 p50 `0.082–0.084 ms`、p95 `0.089–0.092 ms`、p99 `0.094–0.103 ms`、max `0.100–0.135 ms`。漂移稳态 p50 约 `23.6–26.7 ms`、p95 `74.7–76.0 ms`、p99 `82.7–84.0 ms`、max `86.7–90.7 ms`；首个 seek warm-up 窗口 max `862 ms` 单独保留，不代表稳态。C decoder direct_mem `48857088` bytes 全程不变；AGC health direct_mem `3.19→8.48 MiB`，端点斜率约 `0.177 MiB/min`，health peak `52.67 MiB`。末端 `presented=107280 inputs=53726 accepted=53725 outputs=53725 dropped=0 errors=0 order_errors=0`；180 个 health 样本 `dcb_full/ring_fail/tex_fail/timeouts/vo_rc` 全 0，无 `FALLBACK_A`、无 `img-net: failed`、无 combined-watchdog。


#### 6.5.8 M9：C 小窗 UI 视口/混合状态回归（2026-10-07）

同一视频 `BV1b1e86XE3S`、同一详情页的对照收据：纯 A `/tmp/sw-a-same-video2.png` 右栏评论、UP 信息、按钮均正常；旧 C `/tmp/sw-c-same-video.png` 的视频本身正常，但右栏空白，标题/按钮/评论几何被压进视频矩形，底部控件与页面内容重叠。C 的 `/tmp/sw-c-same-video-cycle.log` 已记录详情请求的 `http: code=200`、JSON parse 与 `callback done`，并持续输出 health/present；不是评论接口未完成或 UI 线程被 native decoder 阻塞。

根因是 `evo_agc_blit_yuv_rect()` 在 VideoView 的 native raw draw 中把 AGC viewport/scissor 改成视频物理 rect，并将 blend 设为 `EVO_AGC_BLEND_NONE`；同一帧后续 NanoVG UI draw 只重新写 scissor、绑定 UI pipeline，没有重新写 viewport 和 premultiplied-alpha blend。于是完整页面坐标被映射到视频 rect，右栏落在视频 rect 外，alpha 字体/半透明层也变成黑色块。不是 `VideoView::draw()` 提前 return，也不是活动栈/评论回调丢失。

修复：AGC 增加 `evo_agc_runtime_restore_ui_state()`，按当前 target 恢复完整 viewport/scissor 与 `EVO_AGC_BLEND_PREMULTIPLIED`；native VideoView raw draw 后只在 C 分支调用 `wiliwili_vdec_play_restore_ui_state()`，A 的 `MPVCore::draw()` 路径未改。修复后 `/tmp/sw-c-same-video-fixed2.png` 右栏、标题、UP 信息、按钮和底部控件均恢复正常，字幕停留在视频 rect 内且不压控件；`/tmp/sw-c-same-video-cycle-fixed2.log` 的 `http` 回调完成、health 三项/timeouts/vo_rc 全 0。

### 6.6 P2：可发布生产

仍需补齐：

- 备用 URL 自动切换已有中途断流收据：primary 8 MiB 后 503，C `backup-switch ... seek_rc=0` 后继续 present，无回退。
- HDR10 输出元数据/真实 B 站 Main10 仍未验收；本地原生 P010 仅覆盖 PQ→SDR。
- 随机 seek×20、EOF replay×5 与 C ≥30m 长测均已通过；C 长测无 `FALLBACK_A`，A 历史长测稳定。
- 真实 B 站 60fps/4K 受当前账号无大会员限制不可得，不把低档降级流冒充高档；本地 60fps/4K 只作为工程验证。

生产验收建议：1080p60 和 4K30/4K60 各至少 10 分钟；HEVC Main10 至少 10 分钟；随机 seek/暂停/倍速/清晰度切换；无崩溃、无黑帧/撕裂、A/V 漂移 <100 ms；默认 profile 的 GPU/AGC health 三项 0；native 不支持的样本自动回退 A。

## 7. 性能、工作量与风险

### 7.1 成本量级

|阶段|工作量（1 人）|能换到什么|
|---|---:|---|
|P0 能力探针|2–4 人日（本轮含三标题、流生成、构建、部署、日志解析）|当前 12.00 的 codec/尺寸/P010/内存/decoder P95 结论；本轮 decoder 与真实 PTS/reorder gate 均通过|
|P1 8-bit NV12 原型|5–8 人日；连 P0 合计约 7–12 人日|固定视频链路能跑，CPU 解码释放，初步 4K/HEVC 能力；还不能发布|
|P2 生产 8-bit|10–16 人日|A/V、seek、fallback、清晰度、长测完整；可覆盖 H.264/HEVC Main|
|P2 + P010/高质量 AGC|额外 5–10 人日；若需新 AGC shader/toolchain，再加 5–10 人日|10-bit/HDR/高质量 GPU scale；最大不确定性在 pipe 资源和 P010 present|

综合判断：

- **仅要 1080p60**：不值得。A 已交付，C 的网络/同步/内存/VideoOut 风险远大于收益。
- **要 4K/HEVC/CPU 余量**：decoder、PTS/reorder、8-bit NV12 AGC M1 和固定无重排 MP4 的 M2 已通过；继续完成 M3 的 A/V 时钟、IDR seek、watchdog/fallback 与长测，再决定生产化。
- **要 4K HEVC Main10 + 高质量 60fps**：仍不能承诺。当前只证明 1080p P010 解码，不覆盖 4K Main10、HDR、GPU CSC、呈现和时钟。

### 7.2 主要风险与放弃条件

1. **当前 decoder/PTS gate 没有触发固件止损**：H.264 4K、HEVC Main 4K、HEVC Main10 1080p 和真实 B 帧配对均通过；P1 仍受 present/时钟/seek/fallback 条件约束。
2. **P010 只解出但无法显示**：若 P010 `out.valid=1`，但 R16/RG16/AGC pipe 无法正确采样或 tone-map，先交付 8-bit HEVC，10-bit 标为 unsupported；不要把 P010 转回 8-bit CPU 后宣称完成目标。
3. **VideoOut plane 假设错误**：没有 YUV scanout ABI；M1 已证明“NV12 作为 AGC quad 画入单一 BGRA scanout”可行。若后续 AGC pipe/fence/合成路径回归失败，停止 zero-copy/直写路线，最多回退 A，不继续挖 undocumented plane。
4. **内存预算**：4K P010 单帧约 24.9 MiB，仅是输出；decoder GPU/CPU workspace、3 槽、AGC transient、UI/图片缓存还要叠加。若 native 标题在 Query/Create 或长测出现 direct/flexible 分配压力，停止 4K Main10，并让 A 处理可回退样本。
5. **时序和槽复用**：出现一次 GPU 采样已开始而 decoder 重用槽、黑帧、撕裂或 seek 后旧帧回屏，P1 不通过；必须回到复制 ring 或显式 fence。
6. **网络/BSF/AU**：seek 后连续 `0x811D0303`、PTS 无法稳定、或 DASH 备份切换不能无崩溃恢复，停止生产化；不允许用“暂停视频/丢帧”掩盖同步错误。

## 8. 明确推荐

1. **现在不改正式 A 路径，不把 C 合入默认播放。**
2. **C 可以继续进入 M3**：M2 已打通固定 8-bit HTTP MP4 的 FFmpeg demux/BSF → `sceVideodec2` → 三槽 NV12 → AGC scanout，并验证正式 VideoView 层级和 A fallback；M3 负责 playback-time、pause/speed、seek/切档与长测。
3. P1 不做 VideoOut YUV plane、Main10/HDR 或 4K60 present 承诺；HDR/planar pipe 仍未导入。
4. 默认策略保持 A：native decoder 按 codec/profile/size 探测，失败、PTS watchdog 触发、起播不是 IDR 或 present 不满足门槛就回到 `sws-fast=yes` 的 mpv SW 链路。

## 9. 临时实验清理

M1 已保留 `videodec2_probe.c` 的既有 P0/PTS 门控，并新增 `WILIWILI_TEST_VDEC=1` + `WILIWILI_VDEC_AGC=1` 的静态 AGC 门控。临时主机包 `PPSA99279/PPSA99280` 已删除，主机当前只剩正式 `PPSA99233.ffpkg`；本地 `PPSA99280` dist、`/tmp/PPSA99280.ffpkg`、`/tmp/PPSA99280-boot.log` 和 `/tmp/m1-screen.png` 作为 M1 复核证据暂留，P1 收尾时清理。
