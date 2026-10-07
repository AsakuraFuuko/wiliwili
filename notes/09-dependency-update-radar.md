# 依赖更新雷达：PS5 原生工具链、视频参考与相关上游

> 研究范围：只读比较本地固定提交与指定上游提交；不改构建/源码，不构建，不连接真机，不 push。
> 证据来源：各仓库的 `git diff`、`git log/show`、工作树状态；“事实”是命令直接观察到的内容，“推断”是对 wiliwili 的影响判断。
> 研究基线：2026-10-07。

## 一句话结论

**现在不做全量升级；boilerplate 只纳入 RELRO 关键修复，删除 wiliwili 自己的重复 converter patch；ZIP/Ninja/ccache/全局符号策略经路径审查暂不整体同步。EVO 的 seek/预缓冲/P010 仍按独立阶段选择性评估。**

### 当前比较对象

| 仓库 | 本地 | 比较目标 | 本地来源/备注 |
|---|---:|---:|---|
| `ps5-native/ps5-opengl` | `3b64239`（工作树有未提交移植改动） | `v1.0.1=db752da`，另看 `origin/main=ad2807d` | `blackbearreloaded/ps5-opengl` |
| `ps5-native/ps5-native-app-boilerplate` | `722f222`（工作树有本地改动） | `2f672d1` | `blackbearreloaded/ps5-native-app-boilerplate` |
| `references/EVO-PLAYER-PS5` | `140b803` | `21524a4` | `sainsaji/EVO-PLAYER-PS5` |
| `xfangfang/wiliwili` | 官方 `yoga=88e5876` | 官方 `yoga=88e5876` | 本地两条线在其上叠加 PS5 提交 |
| `borealis` | `AsakuraFuuko/borealis` `ps5-native=45343d5` | 不存在安全的同基线上游目标 | 当前 fork 不是 xfangfang 主线的直接可替换版本 |
| `cpr` | `3639e5d`（1.10.5 加本地 CMake 修复） | 官方 `master=b2d95f2` | `libcpr/cpr` |
| SDL 源 | `8c56053f13ca13a0c050de613706ff69eb615836` | 只筛查 `libsdl-org/SDL` 相关修复 | 实际源远端为 `ps5-payload-dev/SDL` |
| `references/PS5_Vulkan` | `71026e7` | `5b5e4fc` | 两条交付线不消费 Vulkan |
| `references/ps5-vulkan` | `10a7651` | `e6d3c3f` | 两条交付线不消费 Vulkan |

---

## 1. `ps5-native/ps5-opengl`：v0.3.0 → v1.0.1/main

### 1.1 SDL2 原生桥：上游没有变；工作树有本地变体

**事实。**

- `git diff --name-status v0.3.0..v1.0.1 -- integration/SDL2` 为空；`integration/SDL2/` 的脚本、补丁、CMake、`SDL_ps5g19.c`、头文件和 README 在两个 tag 间没有上游改动。
- SDL 源 pin 仍为 `8c56053f13ca13a0c050de613706ff69eb615836`，CMake 版本字符串仍是 `SDL 2.30.12-g8c56053f13ca-g26`。原生前缀仍是静态 `libSDL2.a`、`include/SDL2/`、pkg-config/CMake metadata 和许可证文件；现有 receipt/`manifest.sha256` 仍由 `build.py` 生成。
- 上游桥保持一个固定尺寸、一个未共享 OpenGL 3.3 Core context；SDL event queue/`SDL_PollEvent` 行为不变，swap interval 只表示请求值（0/1），不是硬件帧率证明。
- `v1.0.1` 没有改变 SDL ABI、事件结构或 `SDL_CreateWindow`/`SDL_GL_*` 调用契约。
- 当前工作树的 `git status` 有用户改动：`integration/SDL2/CMakeLists.txt`、`build.py`、`static-ps5.patch`，以及新增的 `SDL_ps5osmesa_static.c`。这些改动**不是** v0.3.0→v1.0.1 上游差异，不能在升级时覆盖。
- 本地变体额外打开 `SDL_AUDIO`，给 `ao=sdl` 提供 PS5 audio backend；另有软件渲染路径和 `sceAudioOutInit()` 重入/F32 协商补丁。它解释了当前 `native-sdl2-audio` 前缀的行为，但不构成上游 v1.0.1 的收益。

**与两条线的关系。**

- payload 线使用 payload SDK 的 SDL/OSMesa 历史路径，不依赖 ps5-opengl 的 G19/EGL 桥。
- 正式原生标题线当前是 AGC/NanoVG + 静态 mpv SW + SDL 音频；native build 跳过 SDL video/PS5OpenGLCore33，SDL 前缀主要提供音频。故上游 SDL 桥未变，升级没有直接功能收益。

**建议。**

- 保持 SDL pin 和 `integration/SDL2` 不动。
- 只有在更换 ps5-opengl SDK 前缀时才重建 SDL 前缀：`build.py` 将 SDK identity、display profile、receipt 和 `manifest.sha256` 绑定，不能把旧 SDL receipt 与新 GL SDK 混用。SDL 源本身不需要重新 pin。
- 不要把 `ps5-payload-dev/SDL` 的 PS5 backend 替换成 `libsdl-org/SDL` 主线：本地 source cache 的 remote 是 `https://github.com/ps5-payload-dev/SDL`，该 pin 在 `libsdl-org/SDL` 远端没有对应 ref；公共 SDL 主线不能单独提供当前 PS5 backend。

### 1.2 native-app SDK、桩、heap、manifest、FSELF

**事实。** `v0.3.0..v1.0.1` 的 `native-app/` 只有三类变化：

- `native-app/agc_link_stub.c` 增加 `sceAgcSetSubmitMode(int)` 的空桩；`agc_driver_link_stub.c` 未变。
- `native-app/app_heap.c` 从固定 128 MiB、单 mspace 路径扩展为：
  - weak 可覆盖的 `ps5_opengl_heap_size` 和 `ps5_opengl_heap_zero_fill`；
  - 大于 128 MiB 时尝试 CPU-cached direct memory，再逐级回退 mmap；
  - mspace 耗尽时转到 libc heap，`free/realloc` 按地址路由；
  - `realloc` 失败时搬迁到 libc heap；
  - 可选零填充、heap live-byte 统计，以及有限的 `sceKernelDebugOutText` 诊断。
- `native-app/cts_runtime_shims.c` 增加 cts-runner 相对 `fopen` 路径按 `/app0` 解析的兼容处理。

**manifest/FSELF 事实。**

- 源码树没有与 tag 同步提交的 `manifest.sha256`；SDK manifest、`receipt.json` 和具体 archive hash 是 `build-sdk-bundle.py`/发布包生成的产物。升级不能复用旧 bundle 的 manifest，必须拿与 v1.0.1 runtime/header 相配的完整 SDK 包重新验 manifest。
- v1.0.1 的新显示模式 SDK增加 `include/ps5_opengl_display_modes.h`；`tools/build-sdk-bundle.py`、`native-display-metadata.py` 和相关 verifier/test 为 dynamic profile 增加了 profile 记录及 120 Hz metadata。固定 profile 的旧 SDK 与 dynamic profile 不是同一 identity。
- FSELF magic、原生标题的 `eboot.bin`/`sce_module`/`sce_sys` 目录模型没有被这批提交改成另一种格式；变化集中在 runtime/SDK identity、显示 profile 和上游 GL runtime。`v1.0.1` 还修了 vertex-buffer reference 生命周期，属于 GL runtime，不是 FSELF ABI。

**破坏性判断。**

- **推断：** `sceAgcSetSubmitMode` 是增量 import，不改变现有 wiliwili 调用者；`app_heap.c` 的新 direct-memory import 和 allocator fallback 会改变链接/运行时依赖，必须让模块转换器使用与新 SDK 配套的 stub/import 集合。
- **推断：** 当前工程若继续使用本地 256 MiB heap 适配，不能直接覆盖本地 `app_heap.c`。上游 v1.0.1 默认 weak heap size 仍为 128 MiB，适配应保留为显式 strong override 或由工程层继续控制。
- 上游没有发现针对 PS5 固件 12.00 的专门修复。上游文档给出的 GL 硬件 qualification 是其他固件/设备范围；不能把它当作本机 12.00 的 smoke 证据。

### 1.3 VideoOut/YUV/scanout/4K120/EGL/GL

**事实。**

- v0.3.0→v0.5/v1.0.1 新增 runtime display modes：一个 dynamic SDK 可选 `1920x1080`、`2560x1440`、`3840x2160`，每种 60/120 Hz；公开 API 为 `eglSetDisplayModePS5`、`eglSetDisplayRefreshPS5`、`eglGetDisplayModePS5`。
- setter 只在 EGL 已 terminate、无 pending presentation/GPU batch 时成功；改变模式需要销毁 surface/context、terminate、设模式、重新 initialize 并重建 GL objects。离开 120 Hz 有五秒 presenter reopen guard。
- 120 Hz 需要 `sce_sys/param.json` 的 `attribute3` capability bits `0x80040`；声明能力不等于实测输出 120 Hz。上游文档明确区分 nominal refresh 与真实 frame rate。
- runtime profile 会按 2160p120 capacity 保留 display buffers/arena，低分辨率并不自动收回 4K capacity。
- `src/egl/ps5_egl.c` 还增加 config descriptors/ID、robust/no-error context extensions、pbuffer RGBA 语义等；`src/platform/ps5_scanout.h`/AGC runtime 增加 bounded in-flight batches、presentation/shutdown ownership 保护。
- `v1.0.1` 修复了 vertex-buffer reference 在释放旧 reference 前取新 reference 的顺序；`origin/main` 还包括 scanout pool 一次性 flush、shutdown 各路径 re-arm 检查和 vertex alignment/constant-buffer reference 修复（`67c873f`、`ad2807d`、`217da45`）。
- **未发现** ps5-opengl 新增 `sceVideodec2`、NV12/P010 sampling、视频平面或 YUV 直扫 API。它的 scanout 仍是 GL render target/presentation ownership，不是 wiliwili 实验线 C 的视频解码呈现管线。

**与两条线的关系。**

- payload/OSMesa：不消费 ps5-opengl EGL/AGC runtime，4K120/动态 EGL 不影响当前 payload 交付。
- 原生 AGC/NanoVG：正式路径不链接 `PS5OpenGLCore33`，UI 直接由 AGC/NanoVG 负责 scanout；动态 EGL/GL 行为不会自动改善 AGC UI 或实验 C 的 NV12/P010。
- **推断：** 若未来恢复 GL 3.3 consumer，dynamic SDK 的 EGL restart、config ID、buffer lifetime 与旧固定 profile 不兼容点需要专门适配；不能只替换头文件。

### 1.4 性能与文档可复用做法

**事实。** v1.0.1 新增/完善了 native submission/preparation 文档：最多 8 个 in-flight batches、fence/descriptor/resource 延迟 retirement、依赖范围 CPU wait、可选两 worker ordered collection；`PS5_ASYNC_NATIVE_PREP=1` 时默认两 worker，改配置必须重建 SDK 并重链 consumer。文档同时明确：host checks 不证明硬件 cache coherence，fresh SDK 不继承旧 binary 的 hardware qualification。

**可借鉴但不等价的做法。**

- 将“nominal refresh、UI FPS、视频 FPS、GPU completion”分开记录；不要用 `SDL_GetDisplayMode` 或 120 Hz metadata 冒充实测帧率。
- 保持 scanout buffer 的 owner/fence/retirement 状态机；当前 wiliwili C 路径已有三槽/fence，但 ps5-opengl 的 GL batch 代码不能直接复制到 C 解码路径。
- 异步 CPU preparation 只适合 GL draw preparation；不会降低当前 mpv SW `sws` 或 `sceVideodec2` 解码成本。

### 1.5 ps5-opengl 升级建议

**现在不升。** 原因：SDL bridge 无上游变化；正式原生线绕过 GL/EGL；dynamic display mode 没有 YUV/video-plane 能力；升级会替换 runtime archive/manifest 并触发整条 native build 和真机回归，而当前收益只落在未来 GL consumer。

**触发条件：**

1. 需要同一 SDK 在 1080p/1440p/4K 或 60/120 Hz 间切换；
2. 恢复/新增 GL consumer，且需要 v1.0.1 的 vertex-reference、scanout lifetime 或 async preparation 修复；
3. 上游明确加入我们需要的 VideoOut/YUV/直扫能力（当前没有）。

**触发后的步骤：**

1. 在独立工作树固定 `v1.0.1` 或明确的 `origin/main` commit，保存当前 `ps5-opengl` 工作树改动；不要覆盖本地 audio/OSMesa 适配。
2. 获取与目标 profile 匹配的完整 SDK；校验 SDK `manifest.sha256`、runtime archive、public headers 和 profile metadata。
3. 重新运行 `integration/SDL2/build.py native` 生成新的静态 SDL prefix/receipt/manifest；SDL 源仍 pin `8c56053f…`。
4. 检查 `native_build.py` 的 stub-dir、AGC/AGC Driver imports、heap wrappers、`app-symbols.map`；特别核对 `sceAgcSetSubmitMode`、direct-memory imports 和本地 256 MiB heap适配。
5. 重新编译、链接、转换、FSELF/ffpkg；校验 `eboot.bin`、`sce_module/libc.prx`、`sce_sys/param.json` 和 assets，不复用旧 dist。
6. 主机侧先做 verifier/host smoke；随后才在本机 `PPSA99233` 做正式标题 smoke：启动 marker、`agc health` 三项 0、无 `img-net: failed`，再做 payload 线回归。该真机步骤不属于本次研究执行。

---

## 2. `ps5-native/ps5-native-app-boilerplate`：722f222 → 2f672d1

### 2.1 变动要点

**事实。** 92 个文件变化主要是工具链/示例/文档增强；与我们的原生构建直接相关的部分如下：

- `81d423f` 已把模块转换器的 RELRO file offset 从 `.got` 改为 RELRO 最低地址 section（通常 `.data.rel.ro`），并断言 16 KiB `p_offset % 0x4000 == p_vaddr % 0x4000`。这正是本工程曾在 `sce_module_writer` 上维护的 `relro-congruence` 修复；目标提交已包含它。
- `tooling/native/app-symbols.map` 从只隐藏 C++ allocation operators 改为 `{ local: *; }`，与本工程原生标题的全局隐藏策略一致，避免静态库符号被转换器发布为 app exports。
- `Makefile`/`tools/build.sh` 增加可配置 source/param/sce_sys/assets/root files、`APP_WRAP_SYMBOLS`、Ninja/ccache/依赖文件；这使骨架可作为多个 title 的构建器，不改变最终 `dist/<TITLE_ID>/{eboot.bin,sce_module,sce_sys,assets}` 目录模型。
- release ZIP 新增 `tools/zip-open-modes.py`，把 ZIP 每个文件/目录的存储权限改为 `0777` 并复核；提交 `2f672d1` 的主题就是“console wants an app”。这修复了 ZIP 解包后普通 0644 权限导致应用不能启动的兼容问题。
- 新增 `tools/validate-loader-elf.py`、host tests、incremental build/ccache、curl multi 示例、update-check/self-update 示例和可选 Lapy sandbox-elevation 示例。它们不是 wiliwili 正式标题的必要依赖。
- `tooling/native/self_container.cpp` 增加来源归属；本次比较没有显示 FSELF magic、PT_SCE_PROCPARAM、签名容器几何或 `runtime/libc.prx.sha256` 被改成另一格式。

**不存在的变化。** boilerplate 仓库没有 `native-app/agc_link_stub.c`、`agc_driver_link_stub.c` 或 ps5-opengl 的 `app_heap.c`；这些文件属于 `ps5-opengl/native-app/`。因此不能把 boilerplate 升级误当成 AGC stub/heap 升级。

### 2.2 与 wiliwili-native 的关系

- 原生线的 `scripts/ps5/native/native_build.py` 是本工程自己的 orchestration，不会因 boilerplate 仓库更新自动迁移。
- 本地 boilerplate 工作树已有 `sce_module_writer.cpp` 的 heap-size/RELRO 改动；其中 RELRO 部分与 `81d423f` 重合，heap-size 部分不是上游目标提交。升级时必须先拆分：采用上游 RELRO fix，保留并重新审阅本地 heap policy，避免重复 patch 或丢失 256 MiB 约束。
- **推断：** 全局 `app-symbols.map` 策略是安全收益，但若某个转换器/导入流程曾依赖 application export，升级后会被隐藏；本工程当前要求“只发布 imports”，所以方向一致。
- 上游没有找到专门针对固件 12.00 的 FSELF/loader 修复。其文档记录的是其他固件/主机验证范围；12.00 仍需本机 smoke 才能确认。

### 2.3 建议、触发条件、工作量/风险

**建议：下一次原生工具链维护窗口升到 `2f672d1`，但不与应用功能改动混做。** RELRO fix 已被本地需要，ZIP 0777 和 verifier 对发布安全有实益；现在不必为了本轮 VideoOut/C 路径立刻切换。

**步骤：**

1. 保存本地 `sce_module_writer.cpp`/wrapper 改动，比较后删除已被 `81d423f` 覆盖的重复 RELRO patch；保留仍未上游化的 heap policy。
2. 更新 host tools、`prospero-clang18` 权限/依赖、`app-symbols.map`；重新生成 `ps5-native-tool`、`libc.prx` 校验和及转换器输入。
3. 用 `native_build.py` 的完整 source list 重新编译、转换、检查 PT_LOAD congruence/FSELF integrity/manifest；不能只替换一个转换器二进制。
4. 重新生成 `PPSA99233` dist/ffpkg；真机 smoke 需确认 12.00 启动、imports、日志 marker 和 AGC health。此次未执行。

**工作量/风险（推断）：** 低到中等，约一个 host-tool 维护窗口；最大风险是本地 RELRO/heap/app-symbols 改动与上游新 Make/build contract 叠加，而不是 FSELF 格式本身。

### 2.4 2026-10-08 选择性纳入记录

已 `git fetch origin` 并核对 `2f672d1` 的历史。用户给出的短号 `81d423f` 在远端实际解析为同主题提交 `81d4235`（`Fix RELRO load-segment congruence (#3)`）；该提交已 cherry-pick 到本地 boilerplate，提交为 `cb0d95e`。它把 RELRO file offset 锚定到 RELRO 起始 section（兼容 lld 丢弃空 `.data.rel.ro` 的形状），并增加 16 KiB mapped-LOAD congruence 检查及 host regression。

本地仍需要的 256 MiB heap policy 从旧重复 patch 中拆出，作为独立 boilerplate 提交 `11b0b64` 保留；RELRO 的旧本地实现不再存在。`wiliwili-native/scripts/ps5/native/sce_module_writer-native-app.patch` 已删除，`build-native.sh` 不再提示手工 apply。这样 converter 只有上游 RELRO 实现一份，heap policy 仍是本工程明确的本地约束。

没有整体 fast-forward 到 `2f672d1`：上游 ZIP `0777` 只服务 boilerplate release ZIP，正式线使用自己的 `install-ffpkg.sh`/UFS2Tool；Ninja/ccache 只影响 boilerplate 示例构建，正式线由 `native_build.py` 读取 compile database、使用预构建 SDL 前缀；上游 `tooling/native/app-symbols.map` 也不是正式链接输入，正式线使用 `wiliwili-native/scripts/ps5/native/app-symbols.map`。因此这些变更不进入本轮工具链路径，避免引入 92 文件的无关构建契约变化。
验证收据：boilerplate `tests/test_executable_writer.py` 两个布局用例均通过（含 `.data.rel.ro` 与 lld 丢弃该 section 的 plain PIE）。删除旧 patch 后完整清空 `build-ps5/native` 重建，289/289 translation units 编译，重新生成 converter/FSELF/dist；构建 marker `Oct 8 2026 00:28:29`。`/tmp/m12-relro-smoke.log` 无模式 env 启动 `user mode=0`、health 9 点三项/timeouts/vo_rc 全0、无 `FALLBACK_A`/`img-net: failed`；`/tmp/m12-relro-auto-play.log` 无 `WILIWILI_VDEC_PLAY`，`auto-gate=pass`、`decoder ready`、持续 present 至 `presented=8160 inputs=4086 accepted=4085 outputs=4085 dropped=0 errors=0 order_errors=0`，health 全0，无回退/图片失败。变更未触及 `wiliwili-payload/` 公共脚本，payload 线无需因本次 native-only 修改重建。
---

## 3. `references/EVO-PLAYER-PS5`：140b803 → 21524a4

该范围有约 149 个文件、约 2.3 万行变化，主体是 provider/UI/网络存储。下面只列与我们的实验线 C 直接相关的变动。

### 3.1 直接视频/呈现变动

#### A. 网络预缓冲：`0751788`

**事实：** 网络源启动时先让 demux 队列达到约 48 packets（约 1.6 s），最多等 4 s；decode/audio 线程停在 `pb_prebuffer_hold`，超时、queue-full、EOF 或 allocation failure 都会释放 hold。目标是吸收“源本身启动 1.6–2.6 s、decode/render/GPU 都不慢但帧不存在”的抖动。

**建议借鉴：** 这适合我们 C 路径的网络 VOD/live 起播，但应映射到 AU/PTS pending 队列，不要照抄 packet queue。与当前 wiliwili C 的 `pending_limit`/PTS watchdog 是同一类问题的不同层次；默认值需要现有真实 B 站语料重新取证。

#### B. seek 后死锁、scrub 旧帧与音视频互等：`3bf37f0`

**事实：**

- demux 在一条 stream queue 满时允许受限 overshoot（最多约 `2 * cap`、不超过 ring limit），如果另一条 queue 接近 low-water 就继续喂它，避免 audio 等 video、demux 又停在 audio 的闭环。
- audio “领先 video”等待约 250 ms 无 video progress 后让出；progress 向前或向后都重新 armed，避免 seek 重新锚定后被单向比较误判。
- scrub pause 时复制 hold snapshot；否则 native decoder 的借用 frame slot 继续被覆写，画面会在 scrub OSD 后缓慢前进。

**建议借鉴：** 对我们的三槽 C 管线，重点是“跨队列 starvation 不能由单一 cap 形成闭环”和“seek 之后时钟 progress 允许回退/重锚”。这比增加固定 sleep 安全；应继续保持当前 `Reset → true IDR → PTS FIFO → clocked publish` 契约。

#### C. P010 SDR pipe：`eb407c6`

**事实：** 新增 `EVO_AGC_PIPE_VIDEO_P010_SDR=71`、`video_yuv_p010_sdr.pipe`、生成的 `video_yuv_p010_sdr_pipe.h`，并在 `tools/gen_video_pipes.py` 注册。shader 按 P010 高 10-bit、BT.709 limited-range 做 R16/RG16 采样和 YUV→RGB；`evo_vdec_native.c` 同时改进 HEVC Main/Main10/profile 与实际 pitch 的 10-bit 判断。

**与我们直接相关：** 本工程当前只有 `video_yuv_nv12_pipe.h` 和 `video_yuv_p010_hdr_pipe.h`；`grep` 未发现 P010 SDR pipe。handoff 仍记录 P010 SDR staged→NV12，故这是可选择性借鉴的明确缺口。

**建议：** 先只移植 generator 输入、生成 header、pipe ID 和选择逻辑，保留现有 GPL-3.0 来源/生成记录；用本工程已有 P010/rect/aspect/health 收据验证。不要把 EVO 的整套 provider/UI 或其 AGC runtime 当作 drop-in。

#### D. HDR、VideoOut、slot/fence 的边界

**事实：** 140b803 基线已经包含多种 P010/NV12 HDR/PQ/HLG pipe 和 AGC runtime；本次到 21524a4 新增的是 P010 SDR 与 `evo_agc_probe_rgb` 等诊断/颜色探针，没有看到新的 `sceVideodec2` ABI、VideoOut ownership、三槽/fence 生命周期协议或 HDR10 output metadata/signaling 的完整新实现。

**明确未解决项：** 这段 upstream delta 没有为我们的短包恢复、non-IDR/HEVC CRA 起点、`dts=UINT64_MAX`、M3 playback clock 或 flush timeout 提供新的直接解法。`evo_vdec_native.c` 的少量 profile/bit-depth 修正可参考，但不能替代当前 wiliwili 的已验证 guard。

### 3.2 打包/部署可借鉴项

**事实：** `5435543` 在 `.ffpfsc` 上传后先 FTP read-back SHA-256，再原子 rename；`scripts/package-app.sh`/`deploy-app.sh` 还强化了 title 生命周期检查。它与我们现有“eboot 最后上传、同 title 原子替换、不要覆盖活进程”的纪律一致。

**建议：** 只借鉴 hash-before-promote 和生命周期证据；不引入 EVO 的 package/app layout。我们的 `install-ffpkg.sh`/`test-cycle.sh` 已有自己的 PPSA99233 约束。

### 3.3 触发条件与工作量/风险

- **现在：** 不同步整个 EVO；它不是 wiliwili 的 API/构建依赖，且大部分 delta 与 provider/UI 无关。
- **触发条件：** C 路径需要 10-bit SDR 直接采样、起播网络抖动需更大 cushion、或出现与 EVO 同形的 seek starvation/deadlock。
- **选择性移植工作量（推断）：** P010 SDR pipe 约 1–3 个开发日（生成、shader metadata、native branch、host/静态收据）；预缓冲/跨队列策略约 2–4 个开发日，必须用现有真实语料做回归。完整 EVO media/runtime 同步风险高，不建议。

### 3.4 2026-10-08 选择性移植记录

没有整仓同步 EVO，也没有直接复制 GPL 源码。`3bf37f0` 的音频队列 overshoot/audio throttle 不适用于当前 C：本工程音频仍由 mpv 管理，C 只有一个 FFmpeg demux/decode 线程和三槽 AGC 发布队列，不存在 EVO 的 audio packet queue ↔ video packet queue 闭环。按同一思路在 `native_vdec_play.c` 自研了三项防护：seek request generation 防止新 seek 被旧 reset 清掉；seek 期间对 borrowed AGC frame 最多等待 500 ms，随后先等 AGC idle fence 再回收 stale present；所有非零 seek 在发布前等待目标后的 true IDR，防止 pre-roll 旧 PTS 进入新 generation。

`0751788` 的适用条件是网络源起播抖动；已映射为 C 的网络 URL 48 个视频 packet、最多 4 s、16 MiB 上限的有限 prebuffer。local file 不走该路径；seek generation >1 不重复保留旧 packet，seek 会中断并清空 prebuffer，EOF/超时/分配失败释放 hold。C 没有独立 audio decode queue，因此没有照抄 EVO 的 `pb_prebuffer_hold`，避免让 mpv audio 在 native video 尚未发布时提前跑钟。

`eb407c6` 的 P010 SDR pipe 对应 `EVO_AGC_PIPE_VIDEO_P010_SDR=71`、BT.709 limited-range R16/RG16 shader；本轮不导入。当前正式路径只承诺 8-bit auto，P010 仍是显式实验并已有 P010→NV12 staged 方案；替换还需要 AGC runtime pipe 注册、native branch 和真实 Main10 SDR 收据，当前没有足够实测收益抵消新增 shader/artifact 风险。现有 GPL-3.0 artifact 清单不增加新文件。
最终收据：boilerplate RELRO host tests、native build 后，最终代码 seek stress `/tmp/m12-evo-seek-stress-final.log` 为 20/20、29 seek-ready、无 fallback/img，健康 20 点全0；failover `/tmp/m12-evo-failover-cycle.log` 为 primary 短包/503 → backup success，无 fallback/img；四项注入均 fail-closed 到 A，reset 使用 `/tmp/m12-evo-inject-reset-cycle4.log` 确认 `injected-reset-failure rc=-9017` 且 order_errors=0。最终 `/tmp/m12-evo-final-cycle.log` 约 1804 s，89 窗口，末端 `presented=11040 inputs=5527 accepted=5526 outputs=5526 dropped=0 errors=0 order_errors=0`，180 个 health 点全0，无 `FALLBACK_A`/`img-net: failed`。

最终包收据：`/tmp/m12-evo-final-auto-cycle.log` marker `Oct 8 2026 01:56:03`、`user mode=0`，FTP `/data/homebrew/` 只有 `PPSA99233.ffpkg`，没有新增临时 title。

---

## 4. 低优先上游跟踪

### 4.1 `xfangfang/wiliwili`

**事实：** 官方 `yoga` 当前为 `88e5876`，与本地历史基线相同，没有官方新增 delta。`wiliwili-native` 在其上叠加了约 97 个 PS5/native/notes 提交；payload 线也有自己的少量 PS5 提交和子模块指针。

**结论：** 不需要从官方 yoga 更新；继续以两条独立工作树的本地 PS5 提交为准。上游功能更新若未来需要，应另开应用合并评估，不与工具链升级混做。
- **变动要点：** 官方 `yoga` 无新增；本地 native/payload 线的差异来自本地 PS5 提交。
- **与我们哪条线相关：** 两条线都相关，但当前 pin 已是同一官方基线之上的独立工作树。
- **升级或借鉴建议：** 不做上游 fast-forward；应用功能需要时另开合并评估。
- **触发条件：** 官方 `yoga` 出现明确解决当前应用 bug 的提交，且能与 PS5 patch 分层合并。
- **工作量与风险：** 低到中；风险是把 native/payload 专用提交和子模块指针误回退。

### 4.2 `borealis`：实际来源不是可直接替换的 xfangfang 主线

**事实：** 两条线的 submodule remote 是 `https://github.com/AsakuraFuuko/borealis.git`，native pin 为 `ps5-native=45343d5`，payload pin 为 `ps5-payload=3db30124`。该 fork 含我们消费的 AGC/NanoVG、P010、VideoView rect/fence 改动。

`xfangfang/borealis` 的当前 `main` 为 `a918867`，README 已指向另一套新/legacy 组织；其 `master=cbdc1b6` 也不是当前 AGC fork 的兼容替换。将它当作普通 fast-forward 会删除本工程已消费的 PS5 AGC API 和 artifact。

**结论：** 不升。若要跟进，只跟 `AsakuraFuuko/borealis` 的 `ps5-native`/`ps5-payload` 分支，并逐 commit 评估；不是本轮依赖更新目标。
- **变动要点：** xfangfang 主线已不是当前 AGC fork 的同基线；AsakuraFuuko 分支保留本工程消费的 AGC/P010/rect/fence 代码。
- **与我们哪条线相关：** 原生标题线直接相关；payload 线只消费其通用 borealis 部分。
- **升级或借鉴建议：** 只按 AsakuraFuuko 的 PS5 分支逐 commit 跟进，不替换为 xfangfang main/master。
- **触发条件：** fork 分支出现与现有 AGC API 兼容、并有明确回归证据的修复。
- **工作量与风险：** 中到高；API 和生成 artifact 分叉大，整包替换会破坏原生线。

### 4.3 `libcpr/cpr`

**事实：** 当前 pin 是 `1.10.5` 加本地 `3639e5d`（只把 SDK SDL2 排除出 link line）；官方 master 已到 `b2d95f2`，中间含 1.15 系列。与 native 网络相关、值得留意的上游主题有：

- `22a41e6` curlholder double-free 修复；
- `ac27258` connection pool；`8573f1d` 共享 SSL session 以支持 TLS resumption；
- `424f53b` CA buffer、`93c2ad6` `CURLOPT_CAINFO_BLOB`；
- `5db20fb`/`b3304d1`/`85917bc` MultiPerform handle/lifecycle 修复；
- `d98f8d7` 静态初始化顺序修复。

**与我们关系：** 这些可能改善通用 cpr 安全/连接复用，但没有 PS5 native 沙箱适配，也没有证明能替代当前 CA bundle 裁剪和 native `ImageRequestRunner`。当前 native 直接依赖的 cpr API 跨 1.10→1.15 需编译/行为回归。

**结论：** 不升；触发条件是需要 connection pool/TLS session resumption、确认命中某个 double-free/MultiPerform 修复，或官方发布明确兼容当前 curl/SDK。升级时先保留 `3639e5d` 的 SDL link-line patch，重新验证 TLS、取消、并发和 CA 行为。
- **变动要点：** 官方 1.10.5 之后新增 connection pool、TLS session 复用、CA blob、MultiPerform 生命周期和 double-free 修复。
- **与我们哪条线相关：** 两条线都使用 cpr；native 线的 CA/并发路径风险最高。
- **升级或借鉴建议：** 暂不升级；先针对命中的 cpr issue 做最小 cherry-pick/编译实验，保留 `3639e5d`。
- **触发条件：** 需要 TLS session resumption/connection pool，或确认当前路径命中具体生命周期 bug。
- **工作量与风险：** 中到高；API/行为变化和 PS5 curl 沙箱均需 host + native 网络回归。

### 4.4 SDL 官方主线

**事实：** 当前 source cache remote 是 `ps5-payload-dev/SDL`，不是纯 `libsdl-org/SDL`；pin commit 在 `libsdl-org/SDL` fetch 中没有对应 ref。ps5-opengl 的 `integration/SDL2` 在 v0.3.0→v1.0.1 没有变化。

**结论：** 不从 SDL 官方主线单独升级。任何 SDL 安全/核心修复都要先合入/重建 `ps5-payload-dev/SDL` fork，再通过现有 static patch、host contract、native prefix receipt 验证。
- **变动要点：** 当前 pin 来自 ps5-payload-dev/SDL fork；libsdl-org/SDL 没有该 PS5 commit ref，桥自身在 ps5-opengl 两个 tag 间无变化。
- **与我们哪条线相关：** payload 线使用另一套 payload SDL；原生线使用该 fork 的静态 SDL 音频前缀。
- **升级或借鉴建议：** 不直接追官方 SDL main；先在 PS5 fork 合入修复，再跑现有 static patch/receipt/host contract。
- **触发条件：** fork 合入与 `SDL_ps5g19`/audio backend 兼容的修复。
- **工作量与风险：** 中；PS5 backend、静态 dynapi、音频 ABI 和 manifest 不能拆开升级。

---

## 5. Vulkan 参考仓库：只做相关性判断

两条交付线都不链接 Vulkan；本节不评估 Vulkan 可行性。

### 5.1 `references/PS5_Vulkan`：71026e7 → 5b5e4fc

**事实：** 变化主要是 RADV/CTS 文档、GPU broker 和测试记录。确实出现了 AGC/VideoOut、VideoOut WSI、多组 buffer、display-mode 的观察；部分记录来自不同软件栈/不同硬件（例如文档中的 firmware 13.40 测量），不是我们的 12.00 AGC/NanoVG runtime。

**相关性结论：** 可作为“VideoOut buffer ownership、scanout re-arm、display mode 需要实测”的旁证；没有新增 `sceVideodec2`、NV12/P010 pipe、HDR output metadata、原生标题 FSELF/ffpkg 或 wiliwili 可直接消费的 SDK 接口。**不升级、不引入。**
- **变动要点：** 新增内容主要是 Vulkan WSI/AGC/VideoOut 文档与 broker/测试记录，不是视频解码 API。
- **与我们哪条线相关：** 两条线均不相关；仅可作 scanout ownership 的旁证。
- **升级或借鉴建议：** 不升级、不引入代码；保留文档链接作为背景证据即可。
- **触发条件：** 未来明确研究 Vulkan WSI，而非当前 AGC/NanoVG/C 路径。
- **工作量与风险：** 当前为零；引入会扩大范围且无法证明 12.00 兼容。

### 5.2 `references/ps5-vulkan`：10a7651 → e6d3c3f

**事实：** 变化集中在 DXVK PE frontends、BGRA readback、x86/x64 Vulkan memory mapping、visual scanout controls 与 RADV physical-display checkpoint；没有 AGC/NanoVG、`sceVideodec2`、P010/NV12、FSELF 或我们的 SDL/VideoOut import 变更。

**相关性结论：** 对当前 payload、原生 AGC/NanoVG、实验 C 均无直接可复用变化。**放着不升。**
- **变动要点：** 新增内容集中于 DXVK PE、BGRA readback、memory mapping 和 visual scanout controls。
- **与我们哪条线相关：** 两条线均不相关，没有可直接消费的 AGC/VDEC/FSELF/SDL 接口。
- **升级或借鉴建议：** 放着不升；不移植 Vulkan/DXVK 代码。
- **触发条件：** 仅在未来明确进入 Vulkan/DXVK 研究时重新筛选。
- **工作量与风险：** 当前为零；提前跟进会制造与现有渲染链无关的验证负担。

---

## 6. 总表

| 仓库 | 落后程度/事实 | 现在升不升 | 触发条件 | 主要风险/工作量 |
|---|---|---|---|---|
| `ps5-opengl` | v0.3.0→v1.0.1；SDL bridge 0 文件差异；新增 dynamic EGL/display、GL lifetime/perf、heap 能力 | **不升** | 需要 runtime 4K/120、GL consumer 修复，或出现新的 YUV/直扫能力 | SDK+manifest+SDL prefix+native relink；中等，必须真机 smoke |
| `ps5-native-app-boilerplate` | 722f222→2f672d1；RELRO 16 KiB fix、全局符号隐藏、Ninja/ccache、ZIP 0777 | **下一工具链窗口升** | 要收掉本地 RELRO patch、发布 ZIP 或修 converter | 本地 patch 重叠；低-中，需重建 converter/FSELF |
| `EVO-PLAYER-PS5` | 140b803→21524a4；P010 SDR、预缓冲、seek/starvation 修复，其余大多 provider/UI | **不整体升；选择性借鉴** | C 需要 P010 SDR/网络 cushion/seek deadlock 修复 | shader/artifact 与三槽/PTS契约需单独验收；中等 |
| `xfangfang/wiliwili` | 官方 yoga 无新增；本地已叠加 PS5 提交 | **不升** | 需要明确的上游应用功能 | 与 native/payload 工作树冲突 |
| `borealis` | 实际是 AsakuraFuuko PS5 fork；xfangfang 主线不兼容 | **不升** | fork 分支出现针对当前 AGC API 的明确提交 | 大量 API/artifact 分叉；高 |
| `libcpr/cpr` | 1.10.5→官方 1.15 系列；有通用安全/连接池/MultiPerform 修复 | **不升** | 命中具体 bug 或需 TLS session/connection pool | API/行为和 PS5 curl 沙箱未验证；中-高 |
| SDL source | `ps5-payload-dev/SDL@8c56053f…`；libsdl-org 无对应 pin ref | **不升** | fork 合入修复并完成桥验证 | PS5 backend/静态 patch/receipt 不能丢；中 |
| `PS5_Vulkan` | 71026e7→5b5e4fc；VideoOut/scanout 仅文档/不同 WSI 旁证 | **不升** | 仅在未来研究 Vulkan WSI 时再看 | 当前路径无收益 |
| `ps5-vulkan` | 10a7651→e6d3c3f；DXVK/BGRA/memory/scanout | **不升** | 仅在未来研究 Vulkan/DXVK 时再看 | 当前路径无收益 |

---

## 7. 本次验证边界

已执行：对新增仓库抓取指定上游 commit；读取 tag/commit、diff、文件树、remote 和当前工作树状态；确认 SDL source remote/pin、相关 submodule 来源。

未执行：任何构建、host test、native smoke、ffpkg 安装、真机连接、部署、push。以上“需真机验证”的内容是风险/后续步骤，不是本次验收证据。
