# 原生标题线 2.0 — 计划（依据四份调研）
> 迁移期文档（已完成）；当前状态见 `notes/03` 与 `notes/06`。


> 依据：本目录 `01-agc-bringup.md`、`02-hwdecode.md`、`03-native-line-status.md`、`04-ui-renderer.md`，
> 以及 `../../run-continuation/ps5-port-status.md` 的「硬解参考实现」「GPU 渲染参考实现」「payload 线的能力边界」「主页局部渲染尝试（已回退）」四节。
> 本树 `wiliwili-native/` = payload 线代码副本（git `1c84a1a`），老原生线在 `../wiliwili/`，工具链在 `../ps5-native/`。

## 0. 定位

| 线 | 定位 | 渲染 | 播放 |
|---|---|---|---|
| **payload 线**（`wiliwili-payload/`，已交付） | 产品/快速迭代：`wiliwili.elf` 经 `/hbldr` 启动，功能完整 | llvmpipe 软渲染（1080p 每帧光栅化 ~18 ms） | mpv（FFmpeg 软解 + llvmpipe 渲染）；弹幕样式默认 `incline` 后全屏播放 60 fps |
| **原生标题线 2.0**（本树） | 唯一能拿到 **AGC GPU** 与 **sceVideodec2 硬解**的地方（注册应用槽） | 目标：AGC；过渡：llvmpipe | 目标：硬解 + 自绘；过渡：mpv |

**为什么必须换上下文**：硬解与 AGC 都只在「带 TITLE_ID + param.json 的 fake-signed 应用槽」里可用；payload（hbldr/elfldr）上下文调用 `sceVideodec2Decode` 会拿 **errno 5200**（EVO 真机结论，明确点名 elfldr/hbldr），VSH GL 路线我们自己也已证伪（拿不到 surface）。

## 1. 硬约束（决定一切）

| # | 约束 | 内容 | 出处 |
|---|---|---|---|
| 1 | 上下文 | 只有应用槽有 AGC / 硬解；payload 没有 | `01`§0/§1、`02`§1 |
| 2 | VideoOut 唯一 owner | AGC runtime 与 SDL ps5-g19 桥**不能同时** open VideoOut（第二次 open 会 panic 主机）；AGC 还必须**在 self-unjail 之前**初始化 | `01`§1（EVO `gpu-notes.md:21-25`） |
| 3 | 缓存一致性 | init 时 flush ISA + 寄存器数组、每帧 flush DCB、end-of-pipe 两个 `ReleaseMem`；漏了表现为"提交成功但黑屏/上一帧/GPU 卡死" | `01`§2/§5 |
| 4 | 对齐 | AGC 用作渲染目标/纹理的缓冲需 **2 MiB 对齐**（我们现在的 VO 缓冲是 0x20000）；零拷贝路径要 256 对齐 | `04`§4/§7、`02`§4 |
| 5 | 内存 | fake-signed 模块 flexible memory 硬上限 ~450 MB；解码器走 direct memory，1080p 单解码器 ≈50-70 MB、4K ≈110-170 MB | `02`§6 |
| 6 | 许可 | wiliwili 是 **GPL-3.0**：可直接借鉴 EVO/ProsperoLight/ProsperoTV/gears/xash3d/ps5link-sdk/ps5-opengl；**GPL-2.0**（shadPS4/sharpemu/AnyPS5/prosperity）只能读、不可抄 | `01`附录A、`04`§1 |
| 7 | 未知项（需真机） | nanovg 非凸填充/描边依赖 stencil `INCR_WRAP/DECR_WRAP`，EVO 只验证过 KEEP/REPLACE/INCR_CLAMP ⇒ 需 1 轮真机确认，或改 CPU 三角化绕开 | `04`§2/§6 |

## 2. 阶段划分

### N0 — 在新树里把「payload 线代码」编译成原生标题（GL 路线）
- **内容**：`03`§3 的 Step 0-9：前置检查 → payload 构建（产出 CDB + `libromfs-wiliwili.a`）→ 回改应用层 8 个文件 → 回改 borealis 5 个文件 → 构建 → 打包安装 → 启动验证。TITLE_ID 用 **PPSA99013**。
- **验收**：标题从主界面启动，能进首页、能播放（软解）、能返回；日志/崩溃工具链可用（`run-title.sh`/`app-log.sh`/`resolve-crash.py`）。
- **工作量**：8-14 人時（1.5-2 人天）；软渲染变体另 +3-5 人天（非必需）。
- **注意**：两处**合并陷阱**——`config_helper.cpp` 的弹幕默认值（payload 有 `incline`，老树是 `stroke`，单向覆盖会丢修复）；老树仍残留 6 处 `GetCallback` 直连（`video_detail_api.cpp:59,299,319,344,367`、`live_data.cpp:170`、`video_snapshot_core.cpp:85`、`version_helper.cpp:99`），首启若崩在这些接口就照 `http.hpp:171-183` 换成 `runAsync`。

### N1 — AGC 最小上屏（独立探针标题）
- **内容**：`01`§5 的 D0（CPU 填色 + flip，验证所有权与 flip 链）→ D1（全屏三角形 draw，唯一能证明 GPU 真写像素）。**必须做成独立探针标题**（新 TITLE_ID、不链 SDL/EGL/Mesa），避免与 SDL 桥抢 VideoOut。着色器用 `.pipe` → `amdllpc -gfxip=10.1.3`（需 gfx1013 补丁，机器时间 ~1 小时）。
- **验收**：D0 出固定色；D1 出三角形且帧率/时间有 receipt；主机不 panic。
- **工作量**：D0 1-2 人天；D1 3-5 人天。
- **好消息**：native 线**已具备全部必需导入**（`native_build.py:237` 已链 `-lSceAgc/-lSceAgcDriver/-lSceSysmodule`，stub 里 19+5 符号）；唯一缺 `sceAgcDcbDmaData`（往 `ps5-opengl/native-app/agc_link_stub.c` 加同名函数重建 stub，NID 自动推导）。

### N2 — 硬解探针与接入（对播放器性价比最高的一段）
- **内容**：P0 探针（`sceSysmoduleLoadModule(207)` + 完整 bring-up + 喂一帧 IDR ⇒ 期望 `DECODE=0`）→ P1 接入：**B站 DASH 天然分离音视频 URL**，mpv 只保留音频与时钟（`playback_time` 已在 observe，弹幕/OSD 一行不改），视频自建 `ffmpeg demux + mp4toannexb + videodec2 + 自绘`。
- **呈现两条路**：B1 = NV12 → 纹理 + GLSL（零风险，GPU 代价同今天 mpv pass）；**B2 = CPU YUV→BGRA 直写 SDL 窗口表面（EVO 实测 1080p 0.98-2.11 ms、4K 7.4-11 ms）**——同时省掉解码与渲染，是这一阶段真正的收益点。
- **注意**：走 CPU 读像素就必须把帧池 prot 设成 **0x33**（不是 0x32）；`depth=1` + 每 AU `!valid` 时 `Flush`。
- **验收**：播放 1080p60 视频，`decoder-drops=0`、应用帧率不低于现状、字幕/弹幕/进度条正常、seek 正常。
- **工作量**：P0 1.5-2 人天；P1(B1) 3-4；P2 行为对齐 2-3；P3 codec/内存 1.5-2；P4(B2) 3-5。

### N3 — 「AGC 视频 + llvmpipe UI」混合（单平面双绘制者）
- **内容**：AGC 画视频层、llvmpipe/nanovg 把 UI/弹幕合成进**同一个 VideoOut 缓冲**（EVO `pp_agc_present`/`evo_agc_composite_bgra` 有真机先例），需要 per-slot fence、`ReleaseMem(45)` + `ReleaseMem(40, GCR 0x30c)`、`clflush`(WB_ONION) 的正确顺序（EVO 的"黑带"事故就是顺序错）。
- **收益边界（重要）**：**只救播放页**（现在 13.7 fps）；纯 UI 页（主页那 17.7 ms 全在 UI 光栅化）**没有收益**。
- **已排除**：VideoOut **没有**多平面/叠加层（四个参考项目都是单平面 2 缓冲）⇒ 硬件分层合成这条路不存在。
- **工作量**：10-16 人天。

### N4 — nanovg 的 AGC 后端（治本）
- **内容**：给 nanovg 写 AGC 后端（1 VS + 1 FS、`NVGvertex` 16B、常量 11×vec4）。AGC UI 已被 EVO 真机验证（整套 UI 只用 1 个 `ui_screen_2d` pipeline）。
- **风险**：stencil 操作（约束 #7）必须先真机确认，或改 CPU 三角化。
- **收益**：所有页面（主页/设置/播放页）彻底离开 llvmpipe，60 fps 不再是问题。
- **工作量**：19-31 人天（与 N3 合计 30-45 人天）。

## 3. 工作量汇总

| 阶段 | 内容 | 工作量 |
|---|---|---|
| N0 | 新树 → GL 原生标题跑通 | 1.5-2 人天 |
| N1 | AGC 最小上屏（探针） | 4-7 人天 |
| N2 | 硬解探针 + 接入（B1/B2） | 7.5-13 人天 |
| N3 | AGC 视频 + llvmpipe UI | 10-16 人天 |
| N4 | nanovg AGC 后端 | 19-31 人天 |

## 4. 建议顺序与决策点

1. **先做 N0**（成本低、把"payload 线代码 + 原生打包/启动/诊断"这条路走通，后面所有工作都挂在这棵树上）。
2. **N1 与 N2 可并行**（一个验 GPU，一个验硬解；都不碰 wiliwili 应用层）。
3. **N2 之后先落地 B2**（收益最大、不动渲染层）；N3/N4 等 B2 数据出来再决定要不要投。
4. 决策点：B1 vs B2（B2 若实测接近 EVO 的 1-2 ms/帧，则 N3 的价值大幅下降）；stencil 确认结果决定 N4 的路径。

## 5. 已完成/已验证的前置（本计划不重复做）

- 前置检查全绿（工具链、SDK、宿主工具、UFS2Tool、RELRO 同余补丁在位）。
- `scripts/ps5/native/` 22 个脚本已在新树就位（与老树逐字节一致）。
- 两树 `CMakeLists.txt` 逐字节相同 ⇒ **无需给 CMake 加 `PS5_NATIVE_APP` 分支**。
