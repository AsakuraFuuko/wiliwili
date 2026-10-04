# 06 — 原生线播放页帧耗：测量、已定位的两个问题与下一步

> 取证日期 2026-09-27，真机 PPSA99055–99060，视频 `BV1teau6XE5Z`（`WILIWILI_TEST_BV` + `DELAY=25` 无人值守复现）。
> 相关：`04-ui-renderer.md`（AGC 上的 UI 方案与工作量）、`02-hwdecode.md`（解码/上屏链路）、`03-native-line-status.md`。

## 0. 结论（先看这段）

播放页的瓶颈**不在视频**（视频上屏只要 1–7 ms），而在 **UI 的 GL 提交**：

```
frame: ui=2ms submit=290ms video=5ms swap=10ms calls=…     ← 播放页（修前）
frame: ui=2ms submit=0ms   video=0ms swap=14ms calls=0/30  ← 主页（静态层，不重绘）
```

`submit` = **`nvgEndFrame`**（nanovg 把这一帧记录的调用真正发给 GL 的地方）。它由两个乘数决定：

| 因子 | 实测 | 状态 |
|---|---|---|
| **每次 draw 的驱动成本** | **1.7–2.4 ms/次**（与调用数线性） | 未解决，见 §3 |
| **每帧 draw 调用数** | 44 → **262**／帧（2 分钟内单调增长） | **已修**，见 §2 |

修掉调用数泄漏后：`submit` 447 → 36–84 ms，播放页 **≈2.4 fps → 8–20 fps**（随弹幕密度波动）。
要再往上走只能压 §3 的"每次调用 2 ms"。

## 1. 测量工具（已落在原生线，永久保留）

| 工具 | 位置 | 说明 |
|---|---|---|
| 帧耗四段 + 每帧调用数 | `scripts/ps5/native/videodec2_probe.c`（钩子）、`library/borealis/.../application.cpp`（调用点） | 每 30 帧一行：`ui`=beginFrame+整棵视图树；`submit`=nvgEndFrame；`video`=硬解探针叠加+自管播放器上屏；`swap`=endFrame(pacing sleep+图片上传 1 张+glFlush+SwapWindow)；`calls`=30 帧的 draw 调用总数 |
| draw 调用计数 | 同上（`glad_glDrawArrays` **运行时换指针**） | borealis 用 glad 的函数指针调 GL，**链接期 `--wrap=glDrawArrays` 拦不到**（第一次就是这么踩空的：`calls=0/30`） |
| 驱动 stdout 落盘 | `scripts/ps5/native/native_shims.c`：`WILIWILI_CAPTURE_STDOUT=1` → `/download0/wiliwili-stdout.log`（**行缓冲**，崩溃也留得下） | 驱动的诊断计数只走 stdout（`[ps5-driver-cycles]` 相位周期、`[ps5-cpu-flush-summary]`、`[ps5-gallium]`/`[ps5-deferred-batch]`）；标题停止后 `read-download0.sh <ip> <title> wiliwili-stdout.log` 取回 |

复现：

```bash
PS5_NATIVE_TITLE_ID=PPSA9905X bash scripts/ps5/native/build-native.sh
printf 'WILIWILI_TEST_BV=BV1teau6XE5Z\nWILIWILI_TEST_BV_DELAY=25\nWILIWILI_CAPTURE_STDOUT=1\n' > /tmp/opts.txt
bash scripts/ps5/native/test-cycle.sh PPSA9905X 200 /tmp/opts.txt      # 每次都要新 TITLE_ID
```

## 2. 问题一（已修）：弹幕只进不出，draw 调用数无限增长

**症状**：播放页 `calls` 从 1338/30 单调涨到 7854/30（44 → 262 次/帧），`submit` 从 87 ms 涨到 420 ms，
界面越来越卡（2 分钟后基本不可用）。

**根因**：`DanmakuCore::draw` 用

```cpp
position = i.speed * (currentTime - i.startTime) * videoSpeed / 1e6;
if (position > width + i.length) { i.showing = false; danmakuIndex = j + 1; }   // 唯一的回收判据
```

而 `DanmakuCore::videoSpeed` 只在 `refresh()`（`LOADING_END` 事件）和 `VIDEO_SPEED_CHANGE` 事件里
从 `MPVCore::getSpeed()` 取。原生线**没有 mpv 事件循环** ⇒ `MPVCore::video_speed` 恒为 0 ⇒
`position` 恒为 0 ⇒ 回收判据永不成立 ⇒ 每条弹幕进屏后永远留在"在显示"集合里，每帧多画两条
draw call（描边+填充），直到把帧吃掉。

**修法**（`wiliwili/source/view/mpv_core.cpp:syncNativePlayerState`，每帧由 `VideoView::frame` 调用）：

```cpp
if (this->video_speed != 1.0) {
    this->video_speed = 1.0;
    mpvCoreEvent.fire(MpvEventEnum::VIDEO_SPEED_CHANGE);   /* 补发 mpv 的同名事件给弹幕侧 */
}
```

只给 `MPVCore::video_speed = 1.0` **不够**：弹幕侧不会自己去看这个字段，必须有事件。

**真机效果**（PPSA99060，同一视频）：`calls` 不再单调增长（450–2446/30，随弹幕密度起伏），
`submit` 36–84 ms，播放页 8–20 fps。**顺带修的**：弹幕位置/速度恢复正确（之前 `position=0`，
所有弹幕会叠在右边缘）。

## 3. 问题二（未解决）：每次 draw 约 2 ms 的驱动成本

**证据**（三次真机取样的同一结论，`calls/30` 与 `submit` 近似成正比）：

| calls/30 | 次/帧 | submit/帧 | 折算 |
|---|---|---|---|
| 450 | 15 | 36 ms | 2.4 ms/次 |
| 1052 | 35 | 63 ms | 1.8 ms/次 |
| 2446 | 82 | 167 ms | 2.0 ms/次 |

**已排除/已知的情况**：

- **主页 0 次调用**：主页走"静态层"（不重绘）⇒ 60 fps，`swap=14ms` 是 pacing sleep。所以这不是"驱动全局变慢"。
- **驱动已经会批处理**：链接的 SDK 驱动编译进 `PS5_MULTIDRAW_BATCH=1`、`PS5_DEFERRED_DRAW_BATCH=1`
  （`build/sdk/ps5-opengl-sdk-0.3.0/runtime-config.txt`），延迟队列容量 256、在飞 8 批，
  只在容量满时等待（`ps5_screen.c:11306`）。但**一个 GL draw 调用 = 一次完整的
  `ps5_draw_vbo_locked`（相位 0）**：它按帧把全部 AGC 状态重新写一遍（fbo/color target/depth/
  user_data/draw_state/index → `ps5_agc_gate2_set_*` → `run()`），**没有跨 draw 的状态缓存**。
- **nanovg 侧已经做过一次合并**：`nanovg_gl.h` 里 `#if defined(PS5_NATIVE_APP)` 的
  `wiliwili_multi_draw()` 把**同一次 fill/stroke 的多条 path** 合并成一次 `glMultiDrawArrays`
  （注释写的就是"每次 draw 约 0.5 ms"）。跨调用（图标/文字/矩形各一次）没有合并。
- **驱动自己带 profiler**：`-DPS5_DRAW_PROFILE=1` 已编译进 SDK 驱动，相位表：
  0=`ps5_draw_vbo_locked`、1=`ps5_batch_copy_descriptors`、2=`ps5_try_deferred_draw`、
  3=batch drain、4=`ps5_clear`、5=`ps5_buffer_subdata`、6/7=transfer map/unmap、
  8=`ps5_copy_identical_image`；每 10000 次相位 0 打印一次
  `[ps5-driver-cycles] phase=N calls=X cycles=Y`，并汇总 `[ps5-cpu-flush-summary] calls=… bytes=…`。
  取数通道见 §1（stdout 落盘）。

**下一步（按顺序）**：

1. 读 `[ps5-driver-cycles]` / `[ps5-cpu-flush-summary]`，确认这 2 ms 落在"准备/发射"还是"cache flush"。
   `ps5_flush_gpu_data` 在 x86 上就是逐 64 B 的 `clflush`（`util/cache_ops_x86*`），
   3 MB 的 NV12 上传正好对应视频段那 5 ms，所以"字节数 × 时间"是可以直接对上的量纲。

### 3.1 驱动重建链路（2026-09-27 打通，真机未验证驱动改动前勿删）

驱动不是黑盒：`ps5-opengl` 提供源码 + 工具链，本地可以重建 `libPS5OpenGLCore33.a`。
顺序不能颠倒（先 psbc 生成头文件，再 runtime；一次踩空记录：直接 `make runtime` 会报
`util/format/u_format_gen.h: No such file`）。

```bash
cd ps5-native/ps5-opengl
python3 tools/fetch-sources.py                 # 取 pinned 源码（Mesa / psbc / SPIRV-Headers…）
bash toolchain/build-opengnm-psbc.sh           # 宿主 psbc 编译器 + **生成头文件**
bash toolchain/build-opengnm-psbc-ps5.sh       # libpsbc.ps5.a（PS5 目标库）
make -C tests/ps5 --no-print-directory -f native-app.mk -j8 \
     PS5_FRAME_SUSPEND=0 runtime               # 重建 runtime 归档（含 ps5_screen.c）
bash toolchain/install-ps5-opengl-core33.sh <prefix>   # 组装成可消费的 SDK 前缀
# app 侧：PS5_OPENGL_PREFIX=<prefix> bash scripts/ps5/native/build-native.sh
```

- `PS5_FRAME_SUSPEND=0`：我们 makefile 的默认值是 1，而 SDK 出厂驱动
  （`build/sdk/*/runtime-config.txt`）没有这个定义；对照实验里必须与出厂一致，
  否则测到的是两个变量的差。
- `PS5_RENDER_ARENA_BYTES`：`toolchain/ps5-opengl-core33.mk` 原来写 0x10000000u（256 MB），
  SDK 出厂是 **0x2c00000u（44 MB）**。这个值决定渲染目标是走 arena 还是独占 direct 分配，
  而"是否可复用（跳过一次回刷）"的判据就依赖它（`ps5_flush_texture_backing` 的 `reusable`）
  ⇒ 必须对齐；**2026-09-27 已把 mk 改成出厂值**。
- 用非 SDK 前缀构建 app 时还要两件事，否则链接/模块转换会失败：
  1. 把出厂前缀 `lib/libSce*.so` 复制到新前缀的 `lib/`（install 脚本只装 AGC 两个桩，
     链接会缺 `-lSceAudioOut2`）；
  2. `PS5_NATIVE_STUB_DIR=<ps5-opengl>/build/sdl-folder/.deps/native/ps5-payload-sdk/target/lib`
     —— 桩目录默认按 GL 前缀的相对路径推导，换前缀后推导落空，模块转换会报
     `public SDK stub directory lacks needed module libSceVideodec2.prx`。
- 驱动打点（wiliwili 追加的 9/10/11 相位 + `/download0/ps5-driver-cycles.log` 落盘）
  写在 `src/gallium/ps5/ps5_screen.c`，注释里都标了 `wiliwili`。
- 驱动 stdout 在标题里**没有出口**（`printf` 既不进 UDP 日志也不进
  `WILIWILI_CAPTURE_STDOUT` 的落盘文件——后者只覆盖应用自己的 stdout），
  所以驱动侧诊断要自己写文件（`fopen("/download0/…")`）。
2. 若落在相位 0 内部，给 `ps5_screen.c` 的 draw 路径加细粒度相位（准备 / 描述符 / 发射 / run），
   重建驱动（`toolchain/build-opengl-psbc.sh` + `build-mesa-ps5.sh` + `install-ps5-opengl-core33.sh`；
   构建目录 `build/mesa-ps5-probe/` 已在，可增量；psbc 源码 `tools/fetch-sources.py` 已取好）。
3. 修法方向（按收益/风险）：
   - **跨 draw 状态缓存**：只发射与上一次不同的 AGC 状态（寄存器级 diff），
     把"每 draw ~30 次 `ps5_agc_gate2_set_*` + 一次 `run`"压到"少量 diff"；
   - **减少调用数**：把连续、uniform 相同的 nanovg 调用合并成一次 `glMultiDrawArrays`
     （描边弹幕之间 uniform 完全相同；填充弹幕颜色不同，可先按颜色分桶），
     nanovg 侧照 `wiliwili_multi_draw` 的模式扩展；
   - 视频那条路（NV12 每帧 3 MB）用真零拷贝（解码器输出缓冲直接当纹理），
     否则这 5 ms 会一直在。

## 4. 问题三：播放页 2–4 分钟必崩，崩点在 stb（已修，待真机复测）

三次真机崩溃（PPSA99057 / 99060 / 99061，都在播放页运行 2–4 分钟后）：

```
crash: addr=0x87ea19 (link 0x47ea19) → stbi__load_main + 0x5c9            2026-09-27 17:24
crash: addr=0x86ca87 (link 0x46ca87) → stbi__load_and_postprocess_8bit + 0x57  17:41
crash: addr=0x87ea59 (link 0x47ea59) → stbi__load_main + 0x609            17:48
```

栈上另有 `__emutls_get_address`（说明在 worker 线程）、`stbtt_Rasterize`。与历史记录同族：
git log `0be3b03`「Flush 改用独立帧缓冲（缓解 **addr=0x86cad7** 间歇崩溃）」。

**根因**（`wiliwili/source/utils/image_helper.cpp`）：解码器拿的是 curl 的**下载字节数**当长度：

```cpp
imageData = stbi_load_from_memory((unsigned char*)r.text.c_str(), (int)r.downloaded_bytes, …);
//                                                                      ^^^^^^^^^^^^^^^^^^^ 不是 text 的长度
```

`cpr::Response::downloaded_bytes` 来自 `CURLINFO_SIZE_DOWNLOAD_T`（`cpr/response.cpp:15`），是 curl 统计的
**body 接收字节**；`text` 是写回调积累下来的内容。压缩（Content-Encoding）、部分传输、进度回调中断、
重试等情况下两者会不一致，一旦 `downloaded_bytes > text.size()` 就是**读越界**，
崩在 stb 的初次扫描里（`stbi__load_main`/`stbi__load_and_postprocess_8bit`，正是 SIGSEGV 现场）。

**修法**：

```cpp
const size_t imageBytes = std::min<size_t>(r.text.size(), (size_t)r.downloaded_bytes);
if (imageBytes != (size_t)r.downloaded_bytes)
    brls::Logger::warning("image size mismatch: url={} downloaded={} text={}", …);
```

两个解码器（`stbi_load_from_memory`、`WebPDecodeRGBA`）都用 `imageBytes`；不一致时打一行日志，
这样下次真机上还能看到"到底差多少"（stdout 需要 `WILIWILI_CAPTURE_STDOUT=1` 或 trace 才留得下，见 §1）。

> 备注：`stbtt_Rasterize`（字形光栅化）也在同一崩溃族里出现过一次，但三次的 `addr` 都落在 stb 图片解码；
> 若修完仍复现，再去查字形路径（nanovg 的 font stash 在渲染线程）。

## 5. 其它已确认的小问题

- **libcurl 详细日志刷屏**：`http.hpp` 里 `cpr::DebugCallback` 是无条件装的，每个 HTTPS 请求
  20+ 行（逐行 `wiliwili_boot_log` = open/write/fsync/close + UDP）。`curl:`/`dns:` 两类前缀
  建议照 `[ps5-` 的做法在非 trace 时丢弃（`native_shims.c:wiliwili_boot_log`）。
- **`--wrap=printf` 的 LIBC_TRACE 诊断构建会早期崩溃**（`addr=0x1e4010`，GL 上下文创建阶段）：
  那条路不要再用，驱动 stdout 走 §1 的落盘方案。
- **`/download0` 里的 stdout 捕获必须行缓冲**：块缓冲在崩溃时全丢（第一次取回是空文件）。

## 7. 播放页画面四连（2026-09-27 晚，真机截图驱动）

用户反馈：视频位置/大小不对、弹幕被视频挡、全屏左右白边、加载圈不消失、进度条不动、又崩一次。

### 7.1 视频矩形画小了（坐标系混用）

`VideoView::draw` 上报的 `x,y,w,h` 是 **nvg 逻辑坐标**（布局空间 1280x720 基线），而
`ps5_player.c` 拿它直接当**物理像素**去 `glViewport`——中间差了 `Application::windowScale`
（`1920/1280 = 1.5`）。后果：视频只画出应有尺寸的 1/1.5 ≈ 67%，且锚在左上角。

修法：`wiliwili_ps5player_set_rect(x, y, w, h, scale)` 增加缩放参数（调用点传
`brls::Application::windowScale`），内部换算成物理像素再算 letterbox。

> 教训：这个仓库里"逻辑坐标 vs 物理像素"至少有三套（nvg 布局坐标、`windowScale` 后的窗口坐标、
> GL viewport 坐标）。跨层的数值**必须在边界处换算并打日志**，否则只能靠截图反推。

### 7.2 全屏左右白边（拿解码尺寸当显示尺寸）

日志 `player: video size w=640 h=360`，但 `sceVideodec2` 的 `OutputInfo.height` 是**宏块对齐**过的
（360 → 368）。按 640/368 = 1.739 做 letterbox，全屏 1080 高时宽度只有 1878 ⇒ 左右各 ~21px 露出
每帧清屏色（`brls/clear` = 235,235,235，浅色主题）= "白边"。

修法：`decode_au` / `video_submit_flush` 按流的**显示尺寸**裁剪（`g_disp_w/g_disp_h` 取自
`codecpar->width/height`，ffmpeg 已按 SPS cropping 给出），只拷显示行/列；**UV 平面仍按原始
`oi.height` 定位**（NV12 两个平面是叠着放的，用错高度就整片错色）。同时把 `cand` 日志改成
`player: fit src=WxH rect=… -> WxH`，全屏切换时重打一行，便于核对。

### 7.3 弹幕被视频遮住（绘制顺序）

`Application::frame()` 原来是：UI 全部（含弹幕/OSD）→ `nvgEndFrame()` → 视频（不透明 raw GL 四边形）
⇒ 矩形内的弹幕被盖。

改成：**视频先画**（`nvgResetTransform` 之后）→ `nvgEndFrame()`（UI 叠上去）。之前的注释担心
"被 VideoView 的不透明背景盖成白屏"，核实后不成立：`View::backgroundColor` 默认 `TRANSPARENT`，
`video_activity.xml` 也没设 background，每帧底色来自 `videoContext->clear(brls/clear)`，
而清屏发生在两者之前。

分段钩子相应改为 `video_begin → 画视频 → video_end → nvgEndFrame → submit`，两段耗时仍分得清。
真机效果：`submit` 27 → **5 ms**，`calls` 2800 → **550/30**（17–20 次/帧），HUD 59.7 fps。

### 7.4 加载圈不消失 / 进度条不动（mpv 事件桥）

`VideoView` 的加载圈与 OSD 全靠 mpv 事件：`LOADING_START/END` → `showLoading()/hideLoading()`、
`UPDATE_PROGRESS/UPDATE_DURATION` → 进度条与两侧时间、`MPV_RESUME` → `showOSD(true)`。
原生线没有 mpv 事件循环 ⇒ 圈画上去没人收、进度条停在打开那一刻。

修法（`MPVCore::syncNativePlayerState()`，每帧调）：补 `video_progress` 字段，并在
"ready 且未上报过"时 `fire(LOADING_END)` + `fire(MPV_RESUME)` + `fire(UPDATE_DURATION)`，
之后按 250 ms 限流 `fire(UPDATE_PROGRESS)`；`!ready` 时复位上报标志（换片能重来）。

> 这与 §2 的 `video_speed` 是同一类 bug：**凡是 UI 依赖 mpv 事件的路径，原生线都必须自己在桥里补事件**。

### 7.5 崩溃真正根因：模拟 TLS 返回野指针（已修）

```
crash: 10 c=3 addr=0x86ce67            ← SIGBUS + BUS_ADRERR（访问不存在的物理地址）
stbi__load_and_postprocess_8bit+0x57:
  lea  0x430ba97(%rip),%rdi   # __emutls_v.stbi__vertically_flip_on_load_set
  call __emutls_get_address
  cmpl $0x0,(%rax)            ← rax 是野指针
```

即**沙箱的 emutls 实现会给某些线程返回坏地址**（不是 stb 的 bug，也不只是"解码失败"路径）。
之前只加 `STBI_NO_FAILURE_STRINGS` 堵住了 `stbi__g_failure_reason` 一个变量，不够。
正解：`-DSTBI_NO_THREAD_LOCALS` ⇒ `STBI_THREAD_LOCAL` 完全不定义 ⇒ stb 的所有线程局部变量
退化成普通全局（它们只是"每线程开关"，共享无副作用）。

> 通用结论：**标题沙箱里不要用线程局部变量**。任何第三方库若提供 `NO_THREAD_LOCALS` 类开关就打开；
> 否则要评估每一个 TLS 访问点（`__emutls_v.*` 符号可用 `nm` 直接列出来）。

### 7.6 画质：未登录只有 360P

日志 `[ERROR] 账号未登录` —— B 站未登录只发低清晰度，所以实测流是 640x360（默认清晰度其实是
116 = 1080P60，见 `config_helper.cpp` 的非 PSV 分支）。被放大到 1080p 会显得糊、"顿"感加重。
排查画质问题先看这条日志，别怀疑渲染链路。

## 8. 降 draw 调用数：相邻同状态合并（2026-09-27 晚，真机有效）

`nanovg_gl.h` 的 `glnvg__renderFlush` 里加了 **相邻调用合并**（仅 `PS5_NATIVE_APP`）：

```c
if (call->type == GLNVG_TRIANGLES) {
    int last = i + 1;
    while (last < gl->ncalls) {          /* 同类型 + 同纹理 + 同混合 + uniform 逐字节相同 */
        ... memcmp(&blendFunc) / memcmp(uniforms + offset, fragSize) ...
        ++last;
    }
    glnvg__blendFuncSeparate(...); glnvg__setUniforms(...);
    glnvg__trianglesMerged(gl, i, last - i);   /* 一次 glMultiDrawArrays，语义不变 */
    i = last - 1;
    continue;
}
```

真机 A/B（同一视频 `BV1GJ411x7h7`，取"进入播放器后前 30 秒"对齐比较）：

| | 合批前 (99068) | 合批后 (99070) |
|---|---|---|
| `submit` 均值 | **29.4 ms** | **10.0 ms** |
| `submit` 峰值 | 33 ms | 31 ms |
| `glDrawArrays` 计数峰值 | 9148/30 | 11704/30（更密的一段） |

注意 `calls=` 只统计 `glDrawArrays`，合批后不再统计被合并的调用——跨版本比这个数字会得出
相反结论，**要帧时间对齐比**。

## 9. 视频发布的节奏问题（未修，下一步）

常开的一行健康度（`ps5_player.c`，30 帧一次）：`player: clock=… lead=…ms pub=… gaps=a,b,c,d`。

真机实测（30fps 内容，期望 `gaps≈33ms`、`lead≈0`）：

```
player: clock=1104  lead=117ms  gaps=2,17,17,234
player: clock=2448  lead=53ms   gaps=2,2,80,2
player: clock=5328  lead=-147ms gaps=2,17,1,2
player: clock=6373  lead=247ms  gaps=1,17,2,1
```

- `gaps` 是 **1–2ms 连发 + 30ms 空档**：worker 在落后音频时钟时会把"已到期"的帧连着发布，
  而渲染线程每 16.7ms 只取最新一帧 ⇒ 串里的帧被丢弃，随后空档 = 一顿一顿。
- `lead` 在 ±200ms 间摆，说明是在"追赶—停顿"之间振荡，而不是稳态跟随时钟。

修法（照 EVO 的"发布与 UI 解耦"思路，但要小得多）：**到期才发、迟到即丢**——落后超过半帧
的帧直接丢弃，只保留最新且未过期的那一帧，让发布节奏跟住时钟而不是补发。

### 9.1 这个修法**试过、已回退**（2026-09-27，PPSA99071）

按上面的判据实现后真机更糟：`drop` 计数每秒涨几十，`gaps` 变成 **275–300ms 的巨缝**。

原因：迟到判据里的 `pace` 是 **wall 派生时钟**（`video_pace_clock_us`），与视频 PTS 不同源——
实测 `lead`（视频 PTS − 音频时钟）常在 **+90…+260ms**，即视频本来就"走在时间前面"，
于是"落后 pace 超过 1.5 帧"恒定成立 ⇒ 几乎每帧都被丢掉。**三套时钟（wall / 音频 / 视频 PTS）
没有对齐之前，任何"按 pace 判迟到"的规则都会错。**

要做正确的发布节拍，先把时钟统一，二选一：
1. 以**音频时钟为唯一主时钟**：`pace_user = g_audio_clock_us()`，视频帧按 `pts` 与该时钟比较；
   wall 只用于兜底（音频 EOF 时自走，已有 `video_pace_clock_us` 的漂移修正）。
2. 或者锚定流时间轴：`pace = first_pts + (wall_now − wall_at_first_pts)`，再用音频做慢漂移修正。

在统一之前，保持"到期即发"（`g_work_pts <= pace + VIDEO_LEAD_US`）——它虽然有 1–2ms 连发，
但不会产生 275ms 的空档。`g_frame_interval_us` / `g_drop_late` 两个诊断量随回退一起删掉了。

**回退效果的量化对照**（同一视频、各 240 秒）：

| | 99071（丢帧规则） | 99072（已回退） |
|---|---|---|
| 200ms+ 巨缝出现率 | **205/266 样本（77%）** | **4/200 样本（2%）** |
| 崩溃 | 0 | 0 |

两个版本都 0 崩溃（`STBI_NO_THREAD_LOCALS` 的效果稳定）。

### 10. mpv 复核（2026-09-28）：**结论被推翻——mpv 本体能初始化**

历史结论「mpv 在本机 app slot 环境下初始化即失败」**是错的**。真机探针（`WILIWILI_TEST_MPV=1`，
跳过桩走真实初始化路径，逐步打点）实测：

```
mpv probe: entering real init path
mpv probe: create ok          ← mpv_create() 成功
mpv probe: initialize ok      ← **mpv_initialize() 成功**
mpv probe: render context create
crash: 11 c=1 addr=0x0 …      ← mpv_render_context_create() 里调用空指针（SIGSEGV/SEGV_MAPERR）
```

即：**mpv 的解复用/解码/A-V 时钟在标题沙箱里都能起来**，唯一卡住的是 **render context 创建**。
而 `addr=0`（调用空指针）与当年记的"mpv 没终端后端所以跳 NULL"是同一个现象，根因在别处。

两条线索（都指向我们的垫片）：
1. mpv 通过 `MPV_RENDER_PARAM_OPENGL_INIT_PARAMS` 拿 GL 入口时走 `wiliwili_logged_gl_proc`
   → 日志里**一条 `gl: MISSING` 都没有**，所以不是这条路。
2. mpv 创建 render context 时会自己 `dlopen`/`dlsym` 平台入口（EGL/GLX 一族），而我们的垫片
   `dlsym` 只认 `wiliwili_gl_symbols` 表、**表外一律返回 NULL**（`native_libc_compat.c:1580`）
   ⇒ mpv 拿到 NULL 再调用 ⇒ 跳 0。**最可能就是这个**。

下一步二选一：
- **(a) 先用 `MPV_RENDER_API_TYPE_SW`**（软件渲染 API）：mpv 把帧转成 RGBA 写进我们自己的缓冲，
  **完全不碰 GL/EGL 查找**，与 payload 线的 `MPV_SW_RENDER` 同形（代码已在 `mpv_core.cpp` 里）。
  代价是 CPU 做 YUV→RGBA + 缩放（1080p 需实测，历史上被列为风险项）。
- **(b) 补符号表**：把 mpv 问到的 EGL/GLX 入口加进 `wiliwili_gl_symbols`（或让 `dlsym` 记录 miss），
  让 GL render API 能建起来 —— 性能上更优（mpv 直接渲进我们的 FBO），但要先知道缺哪些名字。

**收益**：mpv 一旦可用，A/V 时钟、帧调度、seek、demux 全部白送 —— 正是 §9 里我们手搓三版、
反复失败的那部分。

### 10.1 结论：换 **SW 渲染 API 就通了**（2026-09-28 真机）

同一探针把 render API 从 OPENGL 换成 **`MPV_RENDER_API_TYPE_SW`** 后：

```
mpv probe: create ok
mpv probe: initialize ok
mpv probe: SW render context ok -> mpv core usable     ← 成功，无崩溃
```

其后应用继续正常播放（197 条 `player: clock=` 日志），说明 **mpv 的 demux / 解码 / A-V 时钟 /
seek 在标题沙箱里全部可用**，唯一的坏路径是 **OpenGL 渲染 API**（mpv 建 GL context 时会自己
`dlsym` 平台入口，我们的垫片表外返回 NULL ⇒ 跳 0）。SW API 不碰 GL/EGL，所以直接过。

### 10.2 三种可选架构（取舍）

| 方案 | 做法 | 好处 | 代价 |
|---|---|---|---|
| **A. 全切 mpv（SW 渲染）** | mpv 负责 demux+解码+A/V 时钟+seek，SW 渲染出 RGBA，我们上传纹理上屏（payload 线 `MPV_SW_RENDER` 同形） | 时钟/调度/seek 白送，彻底摆脱 §9 的自研 pacing | **丢失硬解**（改回 libavcodec 软解）、每帧 CPU 做 YUV→RGBA+缩放（1080P 需实测） |
| **B. mpv 主时钟 + 硬解** | mpv 只跑音频与时钟（`vo=null`），视频仍由 `sceVideodec2` 解、按 mpv 的 `playback-time` 做发布节拍 | 保留 1.46ms/帧硬解，同时拿到 mpv 的 A/V 同步 | 两套播放器共享一条流（seek/换源要同步），工程量中等 |
| **C. 补符号表** | 给 `dlsym` 垫片加 miss 记录，把 mpv 要的 EGL/GLX 入口补进表，让 GL 渲染 API 可用 | mpv 直接渲进我们的 FBO，无 CPU 转换 | 要先知道缺哪些名字；EGL 族符号可能不在可导出集合里 |

建议顺序：**B > A > C**。B 既保留硬解又白送 mpv 的时钟（正是 §9 卡住的地方），是最贴合
我们现状的形态；若 B 的流复用太麻烦，退 A（简单、但丢硬解）；C 留作性能优先时的备选。

### 10.3 但**音频出口**把上面三条路都卡住了（重要，先看这条再选）

`libmpv.a` 是**预编译静态库**（`/opt/ps5-payload-sdk/target/user/homebrew/lib/`，**没有源码**），
所以**无法给 mpv 加音频后端**。它内置的 AO 只有 `sdl / null / pcm / oss / lavc`；而原生标题里
**SDL 音频后端是坏的**（`SDL_InitSubSystem(SDL_INIT_AUDIO)` 返回 -1，见 `notes/03`），oss/pcm/lavc
在本机也无出口 ⇒ **mpv 在原生标题里出不了声**（只剩 `ao=null`）。

⇒ 于是：
- **A（全切 mpv）**：画面有、**声音没有**（除非放弃硬解后再自己写 AO —— 而库不能改，写不了）。
- **B（mpv 主时钟 + 硬解）**：mpv 没有音频在播，它的时钟就退化成"墙钟/vsync 驱动"，**拿不到
  音频主时钟**；我们自己的 sceAudioOut 音频与它会各走各的 ⇒ 又回到"两个时钟"的老问题。
- **C（补符号表）**：只解决 GL 渲染 API，不解决音频。

**结论**：mpv 可用 ≠ 值得切。除非将来能拿到 mpv 源码（自建 + 加 `sceAudioOut` 的 AO），
否则**继续用自管播放器**更划算；§9.2 里那条"AU→帧 pts 归属"的修法才是当前最短路径。

### 10.4 但音频这件事有**确定的解法**：SDL 的 PS5 音频驱动没被编进原生线的 SDL

用户指出"payload 线的 mpv 有声音"，据此核对两份 SDL：

| SDL 构建 | `ps5audio` 符号数 | 结果 |
|---|---|---|
| `ps5-opengl/build/native-sdl2/sdk/lib/libSDL2.a`（**原生线用**） | **0**（设备表里只有 `PS5_bootstrap` 的未解析引用 ⇒ 实现缺失） | `SDL_InitSubSystem(AUDIO)` 失败 ⇒ mpv `ao=sdl` 无声 |
| `/opt/ps5-payload-sdk/target/user/homebrew/lib/libSDL2.a`（payload 线用） | **12** | 驱动已编入 ⇒ payload 线 mpv 有声 |

三份 SDL 源码树里**都有** `src/audio/ps5/`（`ps5-native/cache/SDL`、payload 的 `ps5-sdl-src`、
ps5-opengl 的 `build/native-sdl2/SDL`），差别只在**构建时有没有把这个文件编进去**。

⇒ **解锁方案（明确、有界）**：给原生线用的那份 SDL2 构建补上
`src/audio/ps5/SDL_ps5audio.c`（以及 `PS5_bootstrap` 注册 + `-lSceAudioOut` 导入与头文件），
重新构建 SDL2 前缀。之后：
- mpv `ao=sdl` 有声 ⇒ §10.2 的 **A/B 两条路都重新成立**（mpv 的音频主时钟可用，不再退化成墙钟）；
- 顺带修好原生线里"任何想用 SDL 音频的东西"。

**注意事项**：ps5-opengl 的 SDL 构建是它自己的 vendored 副本 + CMake，改的是那份构建配置（不是应用代码）；
重建后要重链 `PS5_NATIVE_SDL2_PREFIX` 指向的新前缀并真机验证 `SDL_GetCurrentAudioDriver()` 不再是 (none)。

**首轮真机结果（PPSA99078/99079）**：`audio: SDL_InitSubSystem rc=-1 driver=(none) num=1`
⇒ 驱动**已注册**（`num=1`，改前设备表里连实现都没有 ⇒ SDL 看不到任何音频驱动），
但**初始化失败**。对照驱动源码（`src/audio/ps5/SDL_ps5audio.c`）里 `PS5AUDIO_Init` 的
**唯一失败路径**就是 `if (need_init && sceAudioOutInit()) return SDL_FALSE;`。

**★ 真正的根因（2026-09-28，真机 + 源码，已修）**：改成 `SDL_AUDIO ON` 后**仍然失败**，
但日志给出了决定性一行：

```
audio: SDL rc=-1 driver=(none) num=1 tries=9 err=SDL not built with audio support
```

`SDL not built with audio support` 是 SDL 在 **`SDL_AUDIO_DISABLED`** 下由
`SDL_InitSubSystem(SDL_INIT_AUDIO)` 返回的固定串（`SDL.c` 的 `#else` 分支）。而生成头里
确实写着 `#define SDL_AUDIO_DISABLED 1`（`#cmakedefine SDL_AUDIO_DISABLED
@SDL_AUDIO_DISABLED@`，其值来自 CMake 变量 `SDL_AUDIO`，缓存里是 `SDL_AUDIO:BOOL=OFF`）。

**我一开始改错了文件**：真正生效的是构建树里 9/22 **生成**的
`ps5-opengl/build/native-sdl2/integration/CMakeLists.txt`（第 21 行同一段
`foreach(feature AUDIO …)` 强制关闭）；仓库里的 `integration/SDL2/CMakeLists.txt` 该构建
根本不读。这也解释了怪象：`PS5_bootstrap` 与 `num=1` 都在（**驱动文件**照编），
但**子系统**被禁 ⇒ 初始化必失败。

**修法**（生效文件 + 仓库副本都改，保持一致）：从 `foreach` 去掉 `AUDIO`、显式
`set(SDL_AUDIO ON CACHE BOOL "" FORCE)`、`SDL2-static` 接口库补 `SceAudioOut;samplerate`。
结果：生成头变为 `/* #undef SDL_AUDIO_DISABLED */`、`#define SDL_AUDIO_DRIVER_PS5 1`，
SDL 重编 148/148，真机：

```
audio: SDL rc=0 driver=ps5 num=3 tries=1 err=      ← PPSA99082，核心目标达成
```

**另一个必须处理的语义坑**：`sceAudioOutInit()` **第二次**调用返回 `0x8026000E`
（"已初始化"；实测 `1st=0 2nd=-2144993266`），而 SDL 驱动只认 0 ⇒ 一旦进程里
**别人先初始化过音频**（本应用自管播放器就用 `sceAudioOut`），SDL 音频就会不可用。
修法：`SDL_ps5audio.c` 的 `PS5AUDIO_Init` 容忍该错误码（`rc != 0 &&
(unsigned)rc != 0x8026000Eu` 才算失败），走 `integration/SDL2/static-ps5.patch`
（仓库与构建树两份；已用 `patch -p1 -R --dry-run` 反向校验通过）⇒ 可复现。
真机验证（PPSA99083，探针故意抢先初始化）：
`audio: pre-init sceAudioOutInit=0` + `audio: SDL rc=0 driver=ps5 num=3` ⇒ **共存成立**。

**正式路径**（PPSA99084 起）：`MPVCore::init()` 在 `#ifdef PS5_NATIVE_APP` 里、mpv
初始化**之前**无条件调一次 `SDL_InitSubSystem(SDL_INIT_AUDIO)` 并打一行
`audio: SDL rc=… driver=… num=…` 作为启动检查点（重试循环与 pre-init 探针已按约定清理）。

### 10.5 OpenGL 渲染 API 那条路能不能救？——能，机制已明

`nm libmpv.a` 显示它引用 `dlopen`/`dlsym` 各一次（**没有** `eglGet*`/`glXGet*` 的直接未解析符号）
⇒ 建 GL context 时 mpv **自己在运行时查找 GL/EGL 入口**。而原生线的现状是：

- 标题沙箱**禁止运行时加载代码** ⇒ 真 `dlopen("libEGL.so.1")` 必然失败；
- 我们那份 `dlopen`/`dlsym` 垫片（返回假句柄 + 从 `wiliwili_gl_symbols` 表解析）**只编在
  `WILIWILI_SOFTWARE_RENDER` 里**（`native_libc_compat.c:1518-1602`），**GL 构建里没有它**；
- 于是 mpv 拿到 NULL 再调用 ⇒ `crash: 11 c=1 addr=0x0`（与实测完全吻合）。

⇒ 救法（两步，都有界）：
1. **把 dl 垫片也编进 GL 构建**（放开那处 `#ifdef`）——它已经带 miss 记录
   （`dl: sym <name> -> NULL`）；
2. 跑一次探针，从日志里**列出 mpv 实际询问的符号名**，把缺的补进 `wiliwili_gl_symbols`
   （GL 3.3 core 那批入口在镜像里都有，经 `gl46_entrypoints.o`/`glapi_bridge` 导出）。

收益：mpv 能**直接渲进我们的 GL 上下文**（无 CPU 转换），比 SW 渲染 API 更省。

**优先级提醒**：即使 GL 渲染 API 修好，**音频仍是前置条件**（§10.4 的 SDL 音频驱动）——
两者都齐了才谈得上"切 mpv"。顺序建议：**先 SDL 音频（§10.4），再看要不要 GL 渲染（本节的半节改为：SW 渲染已经可用，够用就不必动 GL）**。


### 9.2 第三版（设备时间轴）也没解决 + 新的机制判断

第三版把 pace 换成 `min(设备已播放块时间, 首块起的墙钟)`（静音也计入设备时间轴，避免欠载停住），
真机仍是连发（≤3ms 占 1094 次、~33ms 仅 18 次）。三版都不动，说明**问题不在 pace 的时钟选择**。

新判断（下一轮先验证）：**`g_work_pts` 本身经常是伪值**。看 `player_video_step_to_stage`：

```c
g_work_pts = (g_last_video_pts_us != before) ? g_last_video_pts_us : pace;   /* ← 退化成 pace */
```

`g_last_video_pts_us` 是**最后一个提交的 AU 的 demuxer PTS**（`video_submit_packet` 里按 time_base 换算），
而不是"这一帧"的 pts：① 解码器输出顺序与 AU 提交顺序不一致（B 帧）；② 一个 AU 可能不产帧，
也可能产出属于更早 AU 的帧。于是只要这次提交没让 `g_last_video_pts_us` 变化，`g_work_pts` 就被写成
`pace` ⇒ **立即到期** ⇒ 连发（1–2ms 间隔正是这么来的）。

修法方向：
1. 让解码器/输出信息给出**帧的真实 pts**（`OutputInfo` 里若有 frame pts 就用它，没有就用"AU 序号 → pts"
   的队列：提交时按序压入 pts，取帧时按序弹出）；
2. 或退一步：给"伪 pts"帧加节流——`g_work_pts == pace` 时不立即发布，至少等 `last_pub + 0.9×帧间隔`；
3. 先加一条诊断：每次发布的 `pts 增量`（`delta=…ms`），确认连发时 delta 是 ~0 还是 ~33。


### 10.6 ★ 切到 mpv 后端（2026-09-28，改动已落地，真机验收中）

音频缺口补齐后（§10.4：`driver=ps5`），把原生线的播放后端从自管播放器切到 **mpv**：

| 文件 | 改动 |
|---|---|
| `wiliwili/source/view/video_view.cpp` | 新增 `ps5UseMpvPlayer()`（`WILIWILI_PLAYER_LEGACY=1` ⇒ false）；`setUrl`/`setBackupUrl`/`resume`/`pause`/`stop`/`togglePlay`/`setSpeed`/`draw` 默认走 mpv，自管路径降级为可回退分支 |
| `library/borealis/.../core/application.cpp` | 自管播放器的两处绘制钩子仅在 `WILIWILI_PLAYER_LEGACY=1` 时执行（否则两路同时上屏） |
| `scripts/ps5/native/native_build.py` | 软渲染构建追加 `-DMPV_SW_RENDER` ⇒ 启用 `mpv_core` 的 SW 取帧路径 |
| `wiliwili/source/main.cpp` | 删除**编译不过且重复**的启动期 SDL 音频探针（音频检查点已在 `MPVCore::init()`） |

**为什么这是正解（都是既有代码，不是新写轮子）**
- DASH 双 URL：`VideoView::genExtraUrlParam()` 产出 `referrer=https://www.bilibili.com,network-timeout=5,…,
  audio-file="…"` —— 正是 B 站强校验的 Referer + 独立音轨；
- 视频上屏：`MPVCore::setFrameSize/draw` 的 `MPV_SW_RENDER` 分支把 mpv 的 RGBA 软帧
  `nvgUpdateImage` 成纹理，再按**播放器矩形** `nvgImagePattern` 绘制 ⇒ 视频只在矩形内，
  **OSD/弹幕天然画在其上**（顺带解决"OSD 被遮挡""弹幕层级"两项待办）；
- UI 状态：`mpvSetWakeupCallback(on_wakeup)` → `brls::sync(eventMainLoop())` + 已注册的属性观察
  （core-idle/eof-reached/duration/playback-time/pause/…）自动驱动转圈/进度条，不再需要
  `syncNativePlayerState` 的人工桥接（仅在 legacy 分支保留）。

**验收判据（真机）**：1) mpv 载入真实 DASH 并 `playback-time` 前进；2) 画面在播放器矩形内、
OSD/弹幕在其上；3) 有声音（`ao=sdl`）；4) `WILIWILI_PLAYER_LEGACY=1` 可回退对照。

### 10.7 ★ 软渲染（OSMesa）与 ps5-g19 视频驱动**不能共存** ⇒ 架构定为「AGC/GL 的 UI + mpv SW 帧的视频」

真机证据链（2026-09-28，PPSA99085→99088）：

1. **PPSA99085**：`sdl: SDL_Init(VIDEO) enter` → `SDL_Init failed: ps5 not available`。
   即 `sdl_platform.cpp` 的 `WILIWILI_SOFTWARE_RENDER` 分支（payload 线形态）排在
   `PS5_NATIVE_APP` 之前，把驱动设成 payload SDL 的 `ps5`；而原生标题的 SDL 是
   ps5-opengl 的桥，驱动名 **`ps5-g19`**（`SDL_ps5g19.c`：`VideoBootStrap PS5_bootstrap =
   { "ps5-g19", "PS5 frozen G19 EGL", … }`）。**已修**：判定顺序改为先判 `PS5_NATIVE_APP`。
2. **PPSA99086**：改名后 SDL 真去初始化 ps5-g19 ⇒ 崩在 `SDL_Init(VIDEO)` 内
   `crash: 11 addr=0x0`（正是交接文档里那条"软渲染版 rip=0x0"）。
   根因：`SDL_ps5g19.c` 自身是 **EGL/AGC 呈现路径**（`#include <EGL/egl.h>` 并调 EGL），
   而 OSMesa 变体在 `native_build.py` 里**只链 Mesa、不链 `-lPS5OpenGLCore33`**（那句在
   `else` 分支）⇒ 桥内 EGL 入口是未解析 NULL ⇒ 跳空。
3. **PPSA99087**：给 OSMesa 变体补链 `-lPS5OpenGLCore33` 后，链接器直接给死局——
   **SDK 的 G19 核心库自带一份 Mesa**（`nir_lower_tex`/`nir_cross3`/`nir_normalize`… 与
   OSMesa 那套 Mesa 重复）⇒ `duplicate symbol`。二者在同一映像里不可共存。

**结论（已落地）**：原生线的正确组合是 **UI 走 AGC 真 GL、视频走 mpv 的 SW 帧**
（`MPV_RENDER_API_TYPE_SW`，CPU 产出 RGBA → 当纹理贴进播放器矩形）：
- 不需要 OSMesa，也绕开标题沙箱的 JIT 限制（llvmpipe 本就被 EPERM 堵死）；
- mpv 的 SW 帧与 UI 的 GL 后端互不相干 ⇒ 没有 Mesa 双份冲突；
- `native_build.py`：`MPV_SW_RENDER` 对原生线**恒开**（`PS5_NATIVE_SKIP_MPV_SW=1` 可关），
  并在链接处注明软渲染变体**不得**再链 G19 核心。

### 10.8 ★ 当前唯一崩溃点：`MPVCore::setFrameSize` 的 SW 分支里 `nvgCreateImageRGBA` 跳空

真机（PPSA99088，两轮）：
```
sdl: video driver=ps5-g19 / window 1920x1080 / gl vendor=PS5 homebrew renderer=PS5 AGC
     version=3.3 (Core Profile) Mesa 26.2.0        ← 视频/GL 全通
audio: SDL rc=0 driver=ps5 num=3                   ← 音频全通
mpv probe: create ok → initialize ok → render context create → SW render context ok
crash: 11 c=1 addr=0x70 rip=0x0                    ← 打开播放页那一刻
```
符号化（`readelf -sW build-ps5/native/llvm-pie-symbols.elf`，加载基址 = 崩溃行 `base=`(运行时
`wiliwili_boot_log`) − 该符号链接地址；**注意 `resolve-crash.py` 目前是只回显的残留桩**）：

| 运行时地址 | 链接地址 | 符号 |
|---|---|---|
| 0x783ddd | 0x383ddd | `MPVCore::setFrameSize(brls::Rect)+0x1cd` |
| 0x786148 | 0x386148 | `MPVCore::reset()+0xb8` |
| 0x8a1bf1 | 0x4a1bf1 | `glnvg__renderCreateTexture+0x2e1`（`nvgCreateImageRGBA` 内部） |
| 0x961a92 | 0x561a92 | `__emutls_get_address+0x72`（emutls/TLS 模拟） |
| 0x108fa02 | 0xc8fa02 | `_mesa_lookup_or_create_texture+0x82`（AGC GL 栈 = Mesa 26.2.0 驱动 AGC） |

- `addr=0x70` + `rip=0x0` ⇒ **对 NULL 基址偏移 0x70 取字段后跳转**（不是普通野指针）。
- **用 `WILIWILI_PLAYER_LEGACY=1` 重跑同一镜像，栈完全一致** ⇒ 与"mpv 当播放后端"无关，
  是本次为原生线新开的 `MPV_SW_RENDER` 代码路径（`setFrameSize`/`draw` 的 SW 分支）本身的问题。
- 逃生阀：`PS5_NATIVE_SKIP_MPV_SW=1`（构建期）可关掉 `MPV_SW_RENDER`，回到本次改动前的可用形态。

**下一步诊断（按性价比排序）**
1. 在 SW 分支里打点：`brls::Application::getNVGContext()` 是否非 NULL、`drawWidth/Height`、
   `pixels`，并**优先用 `draw()` 传入的 vg**（而不是全局访问器）建/更新纹理；
2. 用 `NULL` 数据先建 nvg 图像（`nvgCreateImageRGBA(vg,w,h,flags,nullptr)`）再每帧
   `nvgUpdateImage`，确认是不是"带数据创建"这条分支在 AGC/Mesa 上跳空；
3. 若仍跳空：沿 `__emutls_get_address` 查 emutls 初始化次序（Mesa/AGC 驱动用 TLS，
   而本标题的 emutls 是 clean-room 实现）——这与历史上探针/诊断代码踩过的 TLS 坑同类。

**§10.8 追加诊断（PPSA99089，真机）**：在 `setFrameSize` 的 SW 分支里打出上下文与尺寸后：

```
mpv-sw: vg=200275820 2880x1620 pixels=201075a80      ← vg/pixels 都非 NULL
crash: 11 c=1 addr=0x70 rip=0x0
```

⇒ 不是"上下文为 NULL"，而是**请求了 2880x1620 的纹理**：`rect` 在原生线已经是物理像素
（布局 1920x1080），SW 分支又乘了一次 `windowScale`(1.5) ⇒ 缓冲/纹理 2.25 倍
（2880×1620×4 ≈ 18.7 MB）。崩溃发生在 `glnvg__renderCreateTexture` 内（栈上伴随
`__emutls_get_address`，Mesa 的 AGC 驱动建纹理时走 TLS）。

**处置（PPSA99090）**：SW 分支去掉多余的 `windowScale` 乘法，按 `rect` 原尺寸建缓冲/纹理
（同时省掉 2.25 倍内存），并保留 `mpv-sw:` 一行诊断日志作为检查点。若仍跳空，则按
§10.8 第 3 条沿 emutls/TLS（clean-room 实现 + Mesa 的 AGC 驱动）继续查。

**★ §10.8 的真正根因（PPSA99093 实测，已修）**：崩溃点**不是**建纹理，而是建完之后那句
`mpvRenderContextRender(mpv_context, mpv_params)`——探针打点给出了决定性一行：

```
mpv-sw: surface=30 render_ptr=1593140 ctx=0 1920x1080   ← 函数指针正常，mpv_context 是 NULL
mpv-sw: render enter
crash: 11 c=1 addr=0x70 rip=0x0
```

`MPVCore::init()` 里留着**探针时期的守门代码**：

```cpp
#if defined(PS5_NATIVE_APP)
    wiliwili_boot_log("mpv probe: SW render context ok -> mpv core usable");
    mpv_context = nullptr;   /* ← 元凶：探针期为了不让 mpv 参与渲染而掐掉 */
    return;
#endif
```

当时原生线由自管播放器出画面，所以创建完渲染上下文就置空并早退。**原生线改由 mpv 播放后
（§10.6）这句必须去掉**——否则 `reset()→setFrameSize()` 拿 NULL 上下文调
`mpv_render_context_render`，在 mpv 内部对 NULL+0x70 取字段后跳 0，与真机现场一致。

**排查中否掉的假设（留档，避免重复）**：
- 维度阶梯：4x4/16x16/64x64/256x256/1024x1024 **全部成功**；
- 定点探针：2048x2048、1024x1080、1920x1024、**1920x1080 也全部成功** ⇒ **与尺寸/NPOT 无关**；
- `WILIWILI_PLAYER_LEGACY=1` 同栈崩 ⇒ 与 mpv 播放逻辑无关；
- `mpvRenderContextRender` 指针非 NULL（`1593140`）⇒ 不是静态绑定缺失。

**修法**：删除 `mpv_context = nullptr;`，保留早退（SW 路径不需要上游的 GL/FBO 初始化
`initializeVideo()`）。同时确认 `mpv_core.hpp` 的 `MPV_NO_FB` 只在 `USE_GL2` 时定义 ⇒
原生线走 `on_update()` 的 `#ifdef MPV_SW_RENDER` 分支：**每帧**把 mpv 的帧渲染进 `pixels`，
再由 `draw()` 用 `nvgUpdateImage` 上传并按播放器矩形绘制。
打点已收敛为一条检查点 `mpv-sw: surface=… ctx=… WxH`（尺寸阶梯与临时渲染日志已删）。

**验证（PPSA99094，真机）**：删除守门后——

```
audio: SDL rc=0 driver=ps5 num=3
mpv probe: create ok → initialize ok → render context create
mpv-sw: surface=26 ctx=200c531b0 1920x1080      ← ctx 有效（此前为 0）
mpv-sw: surface=27 ctx=200c531b0 800x523        ← 按播放器矩形建面
http: code=200 …（首页/播放页接口正常）
（全程无 crash 行；运行到超时结束仍在出帧：frame: clear=15ms ui=6ms submit=27ms calls=1950/30）
```

⇒ **`mpv_context` 保留后崩溃消除**，SW 视频面按播放器矩形创建。剩余待确认项（下一条追踪日志）：
mpv 是否真的载入 DASH 并在播（`playback-time` 前进、`ao=sdl`、`video-format`/`audio-codec-name`），
以及画面颜色/方向是否需要调 `sw_format`（rgba/abgr）或 y 翻转。

### 10.9 ★★ 音频在原生标题打通（2026-09-28 真机）

**症状**：mpv 有音轨（`tracks=2 [1]=aac`）但 `aid=-1 aud=0 a=- ao=` ⇒ 无声。

**根因（mpv 自己的日志，靠"把 mpv 日志接进启动日志"才拿到）**：

```
ao: Trying audio driver 'sdl'
ao/sdl: requested format: 48000 Hz, stereo channels, floatp
ao/sdl: already initialized                       ← 关键
ao: Failed to initialize audio driver 'sdl'
cplayer: Could not open/initialize audio device -> no sound.
cplayer: Audio: no audio
```

**mpv 的 `ao_sdl` 要求由它自己初始化 SDL 音频子系统**；应用若先调 `SDL_InitSubSystem(AUDIO)`
（我们为了确认驱动可用加的那句），mpv 就报 `already initialized` 并放弃音频，随后把
`--audio` 关掉、撤销音轨选中（`aid=-1 / aud=0`）。**修法：删掉应用侧的 SDL 音频初始化**，
让 mpv 自己来（SDL 构建已开启 `SDL_AUDIO`，mpv 会打开设备）。

**验证（PPSA99103）**：`mpv-trace: t=163.26 dur=212.26 pause=0 idle=0 v=h264 a=aac ao=sdl
tracks=2 aid=1` ⇒ 视频 + 音频同时工作；`calls` 由 2640 降到 ~750、`submit` 由 25ms 降到 2ms。

**可复用的排查手段（本次靠它定位）**：把 mpv 的日志接进启动日志——
`mpv_request_log_messages(mpv,"v")`（客户端必须显式请求，否则收不到 `MPV_EVENT_LOG_MESSAGE`）
+ `msg-level` 放开 + 在 `eventMainLoop` 的 `LOG_MESSAGE` 分支镜像 `wiliwili_boot_log("mpv-log: …")`。
**注意**：这些只能放在**注册了 `mpvSetWakeupCallback` 之后**——探针期的早退会跳过回调注册，
导致事件（含日志）一条都收不到（本次同时修掉了这个早退）。

### 10.10 「播放页图片加载慢」的最终归因 + 处置（2026-09-28）

**现象**：播放页里的头像/封面（https）要 1–2.5 s 才出来；首页封面（早期 VERIFY 未开时走 http）很快。

**分段实测（curl 自己的计时，`WILIWILI_IMG_TRACE=1` 时的 `img-net:` 行）**

| 段 | 空闲时 | 播放中 |
|---|---|---|
| DNS | 1–7 ms | 1–7 ms |
| TCP 建连 | 9–16 ms | 10–16 ms |
| **TLS 握手** | **29–62 ms** | **0.1–2.5 s（浮动）** |
| 首字节 | ≈TLS + 10 ms | 同左 |

⇒ 时间**全部在 TLS 握手**，且随 CPU 负载浮动（同一台机器、同一份证书/bundle）。

**排除项（都有实测）**
- DNS：把解析换成 payload 线自己的 `netdb.o`（`native_build.py: extract_libc_object()`）后行为不变，
  `dns=` 仍是 1–7 ms ⇒ 不是解析问题；
- 证书/CA：验证开(39 ms)与关(178 ms)都在同一量级 ⇒ 不是 121 张根证书的解析/校验；
- 等待原语：`usleep(1ms)/nanosleep(1ms)/poll(10ms)` 实测就是 1/1/10 ms ⇒ 不是 clean-room
  运行时的粒度量化；
- 上传/排队：`up=0-2 ms`、排队≈1 帧 ⇒ 与图片管线无关（且已按 payload 线改成 `brls::sync` 立即上传）。

**归因**：原生线在播放时把 CPU 吃到很满（mpv 软解 1080p + SW 帧渲染 + 每帧上传），
**新建 HTTPS 连接的握手被拖慢**；payload 线用的是同一份 libcurl+mbedTLS、同样的超时/CA/线程数，
所以它没有"特殊技巧"——差异在**播放路径的 CPU 占用**与运行环境。

**处置（已落地）**：`wiliwili_warm_connections()`（`image_helper.cpp`）在播放页打开时于后台
对 `i0/i1/i2.hdslb.com` 各建一条连接并放进 **curl 共享连接池**（与 ImageHelper 同一个
`CURLOPT_SHARE`），播放页的头像/封面直接复用 ⇒ 用户不再承担这笔握手。入口在
`MPVCore::init()` 的 PS5 分支（detached thread）。

**仍可选的进一步措施**：`vd-lavc-threads` 2→1 给解码让 CPU；或接受（首帧后连接复用即快）。

**★ §10.10 追加：真正的结构性根因——应用的 curl 共享句柄只共享 DNS，未共享连接**

`wiliwili/include/api/bilibili/util/http.hpp` 的 `CurlSharedObject` 原本是：

```cpp
curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
// TODO: 下列两个选线不支持多线程，需要实现自定义线程池，每个线程共享一个 curl share 对象
// curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
// curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
```

⇒ **每个请求都是新的 easy handle ⇒ 新建连接 ⇒ 重做一次 TLS 握手**。结合实测的握手成本
（空闲 29-62 ms，播放负载下 0.1-2.5 s），播放页每张头像/封面都要等一次握手，于是"加载好久"；
连"预热连接"也无效——预热建立的是**别的句柄**的连接，无法被图片句柄复用。

**修法（已落地）**：打开连接与 SSL 会话共享（curl 对多线程共享要求调用方提供 lock/unlock
回调，而该文件本来就实现了按 `curl_lock_data` 分槽的 `recursive_mutex` 回调，机制齐备）：

```cpp
curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
```

效果：每个 host 只需一次握手，之后的图片请求直接复用连接（`tls=0ms`），
预热也才有意义（预热连接进入同一个共享连接池）。

**验证（PPSA99118，真机）**：打开连接共享 + 预热后——

```
img-warm: code=200 use=60ms / 651ms / 328ms        ← 预热三次都及时完成（此前两次间隔 2 分钟）
img-net: i0 第 1 张  tls=1030ms                     ← 每个 host 仍付一次握手（播放负载下 1 s 级）
img-net: i0 第 2 张  tls=36ms   total=47ms          ← ★ 复用成功，秒开
crash=0，http code=200 × 10，视频/音频不受影响
```

**结论**：连接/会话共享是这一类问题的正确修法（每 host 一次握手，之后复用）；
"首个连接在播放负载下 1-2.5 s" 属 CPU 争用，仍可选 `vd-lavc-threads` 2→1 或接受。

**★★ §10.10 更正：连接/会话共享（CURL_LOCK_DATA_CONNECT / SSL_SESSION）必须保持关闭**

本会话为"播放页图片秒开"曾把这两个共享打开（配合预热），真机随即出现**"加载卡住 / 视频出不来"**：

```
http: slow 119758ms dns=0 tcp=18  tls=1134 first=1170    ← 传输本身只 1.2 s
http: slow 119765ms dns=0 tcp=0   tls=0    first=32      ← 连接复用，传输 32 ms
（同轮：request start ×9、done ×7，播放页录像 mpv 从未收到 loadfile）
```

**判读**：总时长 ~120 s，而 curl 自己的分段（DNS/TCP/TLS/首字节）只有几十毫秒~1 秒 ⇒
时间**不在网络传输里**，而是花在 **curl 开始传输之前——等待共享句柄的连接缓存锁**。
一条慢请求占住锁，后续请求全部排队到分钟级 ⇒ 表现为加载卡死、playurl 回不来 ⇒ 播放器没有 URL。

**结论（已回退）**：共享恢复为**只有 DNS**（与 payload 线一致）；预热函数一并删除（没有连接共享时
预热建立的连接无法被别的句柄复用）。若将来确实要连接复用，正确做法是上游 TODO 里那种
"**每线程一个 share / 自定义线程池**"，而不是把全进程的连接缓存放进同一个 share。

**保留的诊断**：`HTTP::runAsync` 里的慢请求检查点（>2 s 时打印
`http: slow <ms> dns= tcp= tls= first= code= err=`）——这次就是靠它把"锁等待"与"网络慢"分开的，
属于值得长期保留的健康检查点。

**验证（PPSA99122，回退后）**：

```
http: request start ×9 / done ×9        ← 不再卡住（回退前 9:7）
http: code=200 ×7                       ← 接口全部正常返回
mpv: file loaded / mpv: playback restart ← ★ 流真的加载并开播（新增健康检查点）
http: slow 2320ms dns=3 tcp=18 tls=2269 first=2313   ← 剩下的慢是"每新连接一次 TLS 握手"
crash=0
```

⇒ 回归消除：加载不再卡住、视频能出。**残余**：播放负载下每个新 HTTPS 连接仍要 ~2.3 s 的握手
（DNS/TCP 各 <20 ms）——这是"只共享 DNS"（payload 同款）的固有代价，正解是上游 TODO 的
"每线程一个 share + 有界并发（~4）"，届时才谈得上连接复用与图片提速。

> ⚠ **已被取代（2026-10-03）**：API 请求现为 `HTTP::runAsync` 线程阻塞路径（`wiliwili/include/api/bilibili/util/http.hpp:139-194`，detach 线程 + 阻塞 `session->Get()`）；`multi_runner.hpp` **不再存在**；图片 runner（`ImageRequestRunner`）仍在（`image_helper.cpp:134-471`）。本节及 §10.12 描述的 MultiRunner 结构已过时，仅留作历史参考。

### 10.11 ★★ 正解落地：单线程 curl multi 请求器（图片/网络请求）

**背景**（§10.10 的教训 + 探针实测）：本平台**不能**在多线程间共享 curl 连接缓存（一条慢请求
把别的请求阻塞到 ~120 s）；但 **multi 接口本身完全可用**——探针实测
`multi: loop done status=0 elapsed=215ms`，3 条 HTTPS 并发 DNS 5-10 / TCP 9-25 / TLS 17-45 ms。
当年 fault 的是 **cpr 自带线程池的驱动方式**，不是 multi。

**实现**（`wiliwili/include/api/bilibili/util/http.hpp` 的 `MultiRunner`，仅 `PS5_NATIVE_APP`）：
- **一个专职线程**持有 `CURLM*`，独占全部 easy handle ⇒ 共享连接/会话**在此模式下安全**；
- `MAX_IN_FLIGHT = 4` 有界并发；每请求写回调、取消回调（`isCancel` 已改 `std::atomic<bool>`）、
  超时、CA/verify 与原先一致；完成后 `fetch()` 同步返回给调用线程（图片解码仍留在各自 worker）；
- `ImageHelper::requestImage` 在原生线改走它；`img: slow …reused=…` 为慢请求检查点（>2 s）。

**真机验证（PPSA99124）**

| | 阻塞式（PPSA99122） | 单线程 multi（PPSA99124） |
|---|---|---|
| 慢请求 | 5 条 × 2.3-2.9 s（每次全新握手，tls≈2.3 s） | **1 条**（`img: slow 2541ms reused=0 tls=2429`，该 host 首连） |
| 请求完成 | 9 start / 9 done | 9 start / 9 done，`code=200` ×13 |
| 视频 | `mpv: file loaded` ✅ | `mpv: file loaded` + `playback restart` ✅ |
| 崩溃 | 0 | 0 |

⇒ "每个请求付一次 2.3 s 握手" → **"每个 host 只付一次"**；多线程共享那套（会锁死）已被
单线程 multi 完全取代。**结论：需要"有界并发 + 连接复用"时，一律走 MultiRunner，不要打开
跨线程的 CONNECT/SSL_SESSION 共享。**

**§10.11 追加（2026-09-28 晚，真机）：接口请求也接入 Runner，慢请求归零**

`HTTP::_cpr_get` / `_cpr_post` 在原生线改走 `MultiRunner`（POST 走新增的 `CURLOPT_POSTFIELDS`
支持；响应经新抽出的 `HTTP::dispatchResponse()` 统一分发，保持"异步 + 不阻塞调用线程"语义）。

**为什么连 POST 也要改**：`_cpr_post` 原本用 `session->PostCallback(...)`——**cpr 的 async
（multi + 自带线程池）**，正是当年在安装型标题里 fault 的那条驱动方式；登录/点赞/收藏/关注
都会踩到。统一到 Runner 后，原生线只剩一条网络路径。

| 真机结果 | 改动前 | 改动后 |
|---|---|---|
| `http: slow`（>2 s 接口请求） | 4 条 × ~2.5 s（每条全新握手 tls≈2.45 s） | **0** |
| `img: slow`（>2 s 图片） | 1 条 | **0** |
| 请求完成 | 9 start / 9 done | 9 start / 9 done（PPSA99126 起） |
| 视频 | `mpv: file loaded` ✅ | `mpv: file loaded` + `playback restart` ✅ |
| 崩溃 | 0 | 0 |

⇒ 原生线网络层最终形态：**单线程 curl multi（有界并发 4）+ 安全连接/会话共享 + payload 线解析
（SDK netdb.o）**；慢请求（加载卡顿）现象消除。

> ⚠ **已被取代（2026-10-03）**：同 §10.11 标注。`multi_runner.hpp` 已删除，原生网络层最终形态是 `HTTP::runAsync`（接口）+ `ImageRequestRunner`（图片），不再有 MultiRunner。

### 10.12 原生线网络层收尾（2026-09-28 夜）：MultiRunner 独立成头文件 + 三个坑

**结构**：`MultiRunner` 从 `http.hpp` 抽到独立头 `wiliwili/include/api/bilibili/util/multi_runner.hpp`
（命名空间 `bilibili`，仅 `PS5_NATIVE_APP`）。图片、接口 GET、接口 POST 全走它；
`http.hpp` 只保留 `dispatchResponse()` 与两条 `_cpr_get`/`_cpr_post` 分支。

**踩到并已修的三个坑（都值得记住）**
1. **命名空间嵌套**：`#include "bilibili/util/multi_runner.hpp"` 若落在 `namespace bilibili { … }`
   **内部**，会在里面再开一层 `bilibili::bilibili`，于是 `bilibili::HTTP::…` 解析失败、
   `MultiRunner` 变成 `bilibili::bilibili::MultiRunner`。**必须放在文件顶部**（命名空间之外）。
2. **工具注入污染源码**：一次脚本编辑把
   `__omp_shell("resolve_with_timeout(host, std::to_string(port), ip)) {")`
   写进了源文件，表现却是"类内解析全乱"——`curl_multi_cleanup`/`queue_mutex_` 被报
   `unknown type name`（clang 恢复解析的假象）。定位手段：**用交叉编译器对单文件
   `-fsyntax-only` 本地复现**（2 秒/次），再逐段/逐句 bisect。全仓已扫描，无其它污染。
3. **环境陷阱（务必牢记）**：ps5-opengl SDK 里存在一份 **PS4 时代的 curl 空桩头**
   （`ps5-native/ps5-opengl/third_party/opengnm-psbc/openorbis-compat/include/curl/curl.h`：
   `typedef void CURL;`、`curl_easy_*/curl_multi_*` 全是 `static inline` 空实现）。
   若它进入包含顺序，**网络会静默变成 no-op**（与 AGENTS 里"AGC 空桩让 GPU 提交变 no-op"同类）。
   当前应用编译用的是 payload SDK 的真实头
   （`-isystem /opt/ps5-payload-sdk/target/user/homebrew/include`，curl 8.18）✓，
   链接的也是真实 `libcurl.a` ✓。改包含顺序时务必确认这一点。

**★ §10.12 追加：Runner 的"收割时机"错误导致"加载几张图后整个卡住"（已修）**

真机症状（首页场景，PPSA99133）：

```
multi-run: done moved=59726ms dns=0ms code=0 canceled=1      ← 传输早该结束却长期未被回收
multi-run: start wait=40368ms https://api.bilibili.com/…/feed/  ← 后续请求排队 40 秒
multi-run: start wait=24354ms / 24358ms …                    ← 排队 24 秒（8 个槽位全被占住）
```

**根因**：loop 里写成"**仅当 `running == 0` 才收割** `curl_multi_info_read`"；只要有任意一条传输在跑
（`running > 0` 就一直成立），**已完成的任务不会被回收 ⇒ 槽位不释放**；`MAX_IN_FLIGHT` 达到上限后，
所有新请求只能排队 ⇒ 表现为"前面几张图正常、之后整体卡死"，而卡住的任务最终是在别处（应用取消/晚到的
超时）被解除的，于是 `moved` 高达 60 s。

**修法**：① 收割改为**每轮都做**（`curl_multi_perform` 之后立即 `info_read`，再做 `curl_multi_poll` 等待）；
② 增加**看门狗**：存活时间超过"请求超时 + 5 s"的传输强制回收（`curl_multi_remove_handle` + 标记
`CURLE_OPERATION_TIMEDOUT`），确保任何异常都不会永久占住槽位。

> ⚠ **已被 §10.18 取代（2026-10-03）**：本节的"固定 SW 面到 1280x720"方案已被 §10.18 的"随 rect 动态重建"取代。

### 10.13 原生标题全屏 SW 视频面修复（2026-09-30）

`VideoView::setFullScreen()` 会创建第二个 `VideoView`；旧实现把新的 UI `rect` 直接传给
`MPVCore::setFrameSize()`，播放中从约 `800x450` 切到全屏时释放/重建 mpv SW buffer 和
NanoVG texture，并立即调用 `mpvRenderContextRender()`。这同时放大了每帧 RGBA 上传成本，
也是 PPSA99233 全屏 SIGILL（FFmpeg 视频线程控制流异常）的高风险重配置窗口。

原生标题现固定 SW 面到标题逻辑画布（通常 `1280x720`），`rect` 只控制 NanoVG 显示区域；
首面创建后全屏不再释放/重建 `pixels`、纹理或改变 mpv SW 尺寸。非原生平台保持原有按
`rect` 调整尺寸的行为。真机需确认：普通播放持续出帧、点击全屏不崩、全屏 surface 日志
只出现一次且 `display=` 随布局变化。

### 10.14 纯 AGC 版本播放冒烟（2026-09-30）

重新构建时显式设置 `PS5_NATIVE_AGC=1`，不设置 `PS5_NATIVE_OSMESA_DIR`；290 个原生编译单元成功，最终符号无 `build_member_deref`/`nir_lower_tex_block`，且没有 `sdl_video.cpp.o`。此前误带 OSMesa 的 107M 包已废弃。

PPSA99233 以 `WILIWILI_TEST_BV=BV1teau6XE5Z` 直进播放，构建标记 `Sep 30 2026 19:41:14`。真机日志证据：

- `mpv-sw: surface=26 ... 1280x720 display=1920x1080` 只出现一次；普通播放矩形随后为 `800x450`，没有再次创建 SW surface。
- `mpv: audio active aid=1(rc=0) tracks=2(rc=0) codec=aac ... ao=sdl`；音频路径保持正常。
- 运行约 100 秒至 `agc health frame=3000`，`ring_fail=0`、`tex_fail=0`、`timeouts=0`，进程未崩溃；期间 swap 约 11–16 ms。

本次自动入口只验证了普通播放，未模拟手柄点击全屏按钮；全屏点击后的不崩与显示矩形变化仍需手动确认。

### 10.15 gdbsrv 捕获长运行 native heap OOM（2026-10-01）

启动已安装的 `gdbsrv-ps5-v0.9.elf` 后，通过 TCP 2159 的 `vAttach` 附加运行中的 PPSA99233（PID 165）。用与当前标题完全匹配的 `llvm-pie-symbols.elf` 加载 PIE 偏移，并在 `operator new` 的失败 `ud2`（runtime `0x9662b9`）断下。

断点现场确认：`rax=0`，`ps5_heap_state=2`，`ps5_heap_failures=7`，`ps5_heap_blocks=0x809d2`，`ps5_heap_live_bytes=0x6e45645`（约 110.3 MiB），`ps5_heap_peak_bytes=0x6e479ec`（约 110.3 MiB）。失败发生在 `__wrap_malloc` 内，失败请求大小为 `rbx=0x3e040`（254016 bytes）；当前 128 MiB mspace 已接近容量并发生分配失败。

这次证据把问题从“FFmpeg 非法指令”收敛为“共享 native heap 预算/碎片耗尽”；仍需结合分配来源决定增大预算还是修复长期持有者，不能只捕获 `bad_alloc` 或关闭 `ud2`。

### 10.16 256 MiB heap 最终复测（2026-10-01）

`app_heap.c` 改为 `PS5_OPENGL_HEAP_SIZE=256 MiB` 后，链接反汇编确认 `mmap` 大小为 `0x10000000`。最终自动视频复测使用构建标记 `Sep 30 2026 20:39:06` 的 PPSA99233：

- `mpv-sw` surface 创建成功，`1280x720`；
- AAC 音频 `aid=1`、`ao=sdl` 正常；
- 运行约 180 秒，`agc health frame=5400`，期间视频自然结束并重新加载一次；
- `ring_fail=0`、`tex_fail=0`、`timeouts=0`，无 `crash:` 日志；
- swap 约 10–16 ms，direct memory 峰值约 58.9 MiB / 128 MiB。

随后移除 `wiliwili-options.txt`，重新打包部署干净 PPSA99233；干净包启动至 `agc health frame=600`，AGC 与首页网络均正常。

### 10.17 原生标题小窗右下边缘修复（2026-10-01）

问题来自 `MPVCore::draw` 的 SW 纹理 pattern 原点固定为 `(0, 0)`。NanoVG 的 `nvgImagePattern` 使用绝对坐标；小窗 `VideoView` 的 `rect` 有非零 `x/y` 时，纹理采样随区域偏移，右侧和下侧出现异常。

修复为以 `rect.getMinX()/getMinY()` 作为 pattern 原点，保持纹理与播放器矩形对齐。使用包含 `PS5_NATIVE_AGC=1` 的完整参数重建 PPSA99233 并部署直进视频；用户确认小窗显示恢复正常。测试开关已从最终本地 dist 移除。

### 10.18 原生 SW 视频面动态分辨率（2026-10-01）

原生标题的 SW RGBA surface 改为随视频 `rect` 尺寸重建：切到全屏时用 `1920x1080`，回到播放器小窗时用 `800x450`。首轮测试曾把 rect 再乘 `windowScale=1.5`，造成 `2880x1620` 过分配；实测原生 AGC NanoVG 的 rect 已是物理像素，因此改为直接使用 rect 宽高。

历史测试包 `PPSA99235` 使用 `BV1teau6XE5Z` 真机运行约 180 秒，启动日志确认 `mpv-sw surface=1920x1080 display=1920x1080`，随后小窗为 `800x450 display=800x450`；期间至 `agc health frame=10200`，`ring_fail=0`、`tex_fail=0`、`timeouts=0`，未见 crash，swap 约 9–15 ms。测试验证了动态尺寸创建与长时间运行；手柄全屏反复切换尚未自动化覆盖。此后测试统一使用正式标题号 `PPSA99233`。

### 10.19 恢复正式标题号并移除 OSMesa（2026-10-01）

上一轮切换标题号时误带了 `PS5_NATIVE_OSMESA_DIR`，生成的是 Mesa/OSMesa 变体。已用 `PPSA99233`、`PS5_NATIVE_AGC=1` 且未设置 `PS5_NATIVE_OSMESA_DIR` 重新编译 290 个单元；最终产物只保留 `build-ps5/native/dist/PPSA99233`。

后续构建、安装和真机测试统一使用 `PPSA99233`，不再创建临时标题号。

### 10.20 图片请求失败隔离与 AGC 纹理预算（2026-10-01）

原生图片请求由 `ImageRequestRunner` 的单线程独占一个 `CURLM`，最大并发 4；每个 easy handle 完成或取消后立即移出 multi 并释放槽位。保留 `HTTP::VERIFY`、CA bundle、10 秒请求超时和取消回调；额外 watchdog 在 `TIMEOUT + 5 s` 后回收仍未完成的传输并记录 `img-net: failed`，避免异常请求永久占用并发槽。payload 路径不变。

**单图失败注入真机验证（临时注入已移除）**：把首张图的 URL 临时替换为拒绝连接的 loopback 地址；日志为 `img-net: failed curl=7 elapsed=0ms`，同批其余图片继续完成（HTTP 200，10–239 ms），首页仍渲染至 `frame=1200`，`ring_fail=0`、`tex_fail=0`、`timeouts=0`。截图 `/tmp/ps5-99233-one-failure.png` 显示失败项保留占位，其余封面继续加载，约 60 FPS。

首次提速包的 AGC direct memory 在 `frame=9000` 达 `133,775,616/134,217,728` 字节，随后标题进程消失；启动日志无 crash 记录，klogsrv 抓取无数据，不能据此断言 OOM，但已接近 128 MiB 池上限。PS5 原生 `TextureCache` 改为精确容量 48（**后于 2026-10-03 调整为 24**，见 §10.28 与 `config_helper.cpp:822`），并隐藏会暴露桌面缓存范围的设置。限额版运行至 `frame=16200`，direct memory 保持 `7,807,744` 字节；最终无测试注入包（构建标记 `Oct 1 2026 19:38:58`）运行至 `frame=3000`，图片加载成功、`ring_fail=0`、`tex_fail=0`、`timeouts=0`，截图 `/tmp/ps5-99233-final.png` 约 60 FPS。

### 10.21 原生图片请求超时（2026-10-01）

异步 resolver 版首轮真机仍记录 `img-net: failed curl=28 elapsed=70950ms`，远超 `HTTP::TIMEOUT=10000` 与 `TIMEOUT+5s` watchdog。检查发现原生 CMake 缓存里的 `CPR_CURL_NOSIGNAL=OFF`；cPR 只有定义该宏时才会对请求 handle 设置 `CURLOPT_NOSIGNAL=1`。

修复：`build-native.sh` 强制 `CPR_CURL_NOSIGNAL=ON`，缓存不符时重新配置；`native_build.py` 的对象缓存记录编译参数，确保 `session.cpp` 因宏变化重编。默认 cURL 归档由 `build-curl.sh` 构建为 8.18.0 POSIX threaded resolver，并由 native build 自动链接。

真机 `PPSA99233`（构建标记 `Oct 1 2026 21:20:45`）运行约 180 秒至 `frame=10200`；`img-multi: lanes=4 max-inflight=4 async-dns=1`，图片请求日志无失败，首屏请求 8–344 ms；AGC `ring_fail=0`、`tex_fail=0`、`timeouts=0`，direct memory `7,795,200/134,217,728`。截图 `/tmp/ps5-99233-nosignal-final.png` 显示首页封面完整、约 60 FPS。

### 10.22 PS5 `curl_multi_poll` 非阻塞泵（2026-10-01）

即使启用 threaded resolver 和 `CPR_CURL_NOSIGNAL=ON`，PS5 上 `curl_multi_poll` 仍可能在单次等待中越过请求超时和 watchdog；这会让一个失联图片请求继续占住图片 worker。`ImageRequestRunner` 改为循环执行 `curl_multi_perform`、读取完成消息、检查取消/`TIMEOUT+5s` watchdog，再进行固定 20 ms sleep，不再把调度交给 `curl_multi_poll`。

重新启动的 `PPSA99233`（构建标记 `Oct 1 2026 21:54:07`）验证首页封面完整；图片请求 11–289 ms，持续至 `frame=7800`，日志无 `img-net: failed`，AGC `ring_fail=0`、`tex_fail=0`、`timeouts=0`。截图 `/tmp/ps5-99233-relaunched.png`。

### 10.23 原生图片连接超时与正式构建参数（2026-10-02）

`curl_multi_perform` 的 watchdog 只能在调用返回后检查；PS5 某些失败连接路径仍可能让一次连接超过预期。因此原生图片会话单独设置 5 秒 `ConnectTimeout`，总请求超时仍为 10 秒，连接失败不会长期占满图片 worker。

正式 `PPSA99233` 构建只启用 `PS5_NATIVE_AGC=1` 和静态 SDL2 前缀，不设置 `PS5_NATIVE_OSMESA_DIR`；OSMesa 属于历史软渲染实验，不是当前 AGC 标题链路。真机运行 180 秒至 `frame=10200`，图片请求 38–146 ms，无 `img-net: failed`；AGC `ring_fail=0`、`tex_fail=0`、`timeouts=0`，direct memory `7,795,200`。截图 `/tmp/ps5-99233-connect-timeout-final.png`。
- 直播页现场复验未重启标题：首次切换立即显示灰色占位，约 12 秒后 12 张封面全部加载；对应请求 1,875–2,043 ms、无 `img-net: failed`。截图 `/tmp/ps5-99233-live-page-final-2.png`。

### 10.24 图片 runner socket 调度与失败重试（2026-10-02）

上一版只用 `curl_multi_perform`；真机仍出现单次约 115 秒的 libcurl 状态机阻塞，4 个图片槽同时被占满，后续卡片只能保持占位。现改为 `curl_multi_socket_action` 配合 `select`，等待上限 20 ms；watchdog 和取消检查不再依赖 `curl_multi_perform` 返回。失败或空响应最多重试 2 次，底层 easy handle 同时显式设置总超时、连接超时和低速超时。

构建标记 `Oct 2 2026 12:31:16` 的 `PPSA99233` 真机复验：直播页 12 张封面约 15 秒内全部加载，滚动后新增 4 张也约 15 秒内全部加载；请求 368–1,975 ms，无 `img-net: failed`、`img-decode` 或 `image size mismatch`。截图 `/tmp/ps5-99233-socket-live-after15s.png`、`/tmp/ps5-99233-socket-live-scroll-after15s.png`；AGC 保持 60 FPS、`ring_fail=0`、`tex_fail=0`、`timeouts=0`。

### 10.25 HTTP 200 解码失败重试（2026-10-02）

代码检查发现另一条网络重试未覆盖的永久占位路径：请求返回 HTTP 200 且有非空 body，但 `stbi`/WebP 解码失败时旧代码直接 `clean()`，图片槽虽已释放，视图却永远保持灰色占位。`ImageHelper` 新增每次加载的解码重试计数；PS5 解码失败最多重新进入 `ImageRequestRunner` 2 次，最终失败才清理，日志增加 `img-decode ... try=`。

本轮重建部署的 `PPSA99233`：首页封面完整；直播页切换后 1 秒仍为正常请求中的灰色占位，约 6 秒后全部封面加载；启动与页面日志无 `img-net: failed`、`img-decode`、`image size mismatch`。截图 `/tmp/ps5-99233-decode-live-1s.png`、`/tmp/ps5-99233-decode-live-6s.png`、`/tmp/ps5-99233-decode-retry-home.png`。

### 10.26 重启后 SDL 启动崩溃与 AGC 参数恢复（2026-10-02）

图片解码重试验证时重建命令漏传 `PS5_NATIVE_AGC=1`，生成了非正式 G19/OpenGL 变体。PS5 重启后启动该包，日志为 `SDL_Init failed: ps5-g19 not available`，随后标题崩溃；这不是图片请求路径导致的崩溃。

恢复正式命令后，`PS5_NATIVE_AGC=1` 触发 `BOREALIS_USE_AGC`、跳过 G19 SDL 视频路径并启用 AGC backend。构建标记 `Oct 2 2026 22:18:10` 的 `PPSA99233` 在同一台重启后的主机启动成功，运行至 `frame=9000`，`ring_fail=0`、`tex_fail=0`、`timeouts=0`，PID 142 存活。截图 `/tmp/ps5-99233-agc-after-reboot-fixed.png`。

### 10.27 原生 AGC 缩略图底部渐变（2026-10-02）

视频卡片 XML 两条线完全相同，底部信息层使用 `background="vertical_linear"`。payload 的 GL NanoVG backend 在 `glnvg__convertPaint()` 中对 `paint->xform` 求逆；AGC `paintColor()` 原先直接使用正向矩阵，导致屏幕空间顶点计算出的渐变近似纯黑。修复为先调用 `nvgTransformInverse()`，再按与 GL 相同的 paint 坐标计算。

`PPSA99233` 构建标记 `Oct 2 2026 22:52:03` 已重新部署。实机推荐页截图 `/tmp/ps5-99233-gradient-fixed-home.png`；AGC 日志 `ring_fail=0`、`tex_fail=0`、`timeouts=0`，启动和图片加载正常。


### 10.28 2026-10-03 实测快照与未收口项

**两个包**（已观察）：
- 主机**运行中**的构建标记 = `Oct 3 20:23:59`（PPSA99233）；
- 最新本地 `dist/PPSA99233/eboot.bin` 时间戳 = `Oct 3 20:31:43`，FTP 上传完成 = `20:32:37`（**已上传未启动**）。

**运行健康数据**（已观察，运行中包 20:23:59）：
```
frame=22200, ring_fail=0, tex_fail=0, timeouts=0, direct_mem=13,831,424/134,217,728
```
⇒ AGC ring/纹理/超时全零，direct memory ~13.2 MiB / 128 MiB（远低于 §10.20 提速包的 133.7 MiB 险线）；进程存活、持续出帧。

**三条未收口问题**（已观察）：

1. **图片 worker 被 libcurl 阻塞**——原始日志行：
   ```
   img-net: failed curl=28 ... elapsed=57642ms / 112985ms try=1
   ```
   `curl=28` = `CURLE_OPERATION_TIMEDOUT`，但 elapsed 远超 `HTTP::TIMEOUT=10s` 与 watchdog `TIMEOUT+5s=15s`（§10.20–10.24）；`ImageRequestRunner` 的 `curl_multi_socket_action` + 20 ms `select` 在 PS5 某些连接路径下仍会让单次阻塞越过 watchdog 检查点。worker 被占住 ⇒ 后续图片排队。
2. **`wstring_convert` to_bytes 崩溃**——原始日志行：
   ```
   terminate: wstring_convert: to_bytes error
   → SIGABRT
   ```
   出现在构建 `18:11:55` 与 `20:03:53` 各一次。唯一 `to_bytes` 调用点 = `wiliwili/source/activity/search_activity_tv.cpp:230`（`std::wstring_convert` 转 narrow，遇到无法转换的字符抛 `std::range_error` → `terminate`）。根因推测：搜索页某些输入含非 BMP 字符或 surrogate 半片。
3. **`frame:` 探针数值错误**——`frame:` 每 30 帧输出一行但数值不对。根因（已确认）：AGC 路径无人调用 `wiliwili_frame_phase_clear()`（该钩子只在 GL 路径 `library/borealis/.../platforms/sdl/sdl_video.cpp:555`），因此 `g_t_clear` 保持 0，日志表现为 `clear=-83252312ms`、`ui=+83252313ms`（即 `g_t_clear - g_t0` 与 `g_t1 - g_t_clear`），而 `submit`/`swap` 正常。

**下一轮修复计划 F0–F6**：
- **F0**（合并执行）：不单独跑 20:31:43 的包；工作区已包含 20:31:19 的 `image_helper.cpp` 改动，直接构建含 F1/F2/F3/F6 的新包做一轮真机验证；
- **F1**：img-stall 取证——`ImageRequestRunner` 已给 `curl_multi_socket_action`（`CURL_SOCKET_TIMEOUT` 与 fd 事件两处）和 `select` 加 ≥1 s 计时输出（`img-stall: op=... ms=... running=... active=...`，全进程上限 64 行），用来定位 libcurl 阻塞发生在哪个调用；根因修复待真机取证确定；
- **F2**：to_bytes——`search_activity_tv.cpp` 已移除 `wstring_convert`，改用不抛异常的 UTF-8/wstring 转换（BMP + 代理对，非法码点→U+FFFD）；待真机从搜索路径复测；
- **F3**：frame 探针——`videodec2_probe.c` 的 `frame:` 输出已改为 `WILIWILI_TRACE` 门控，并在 `AgcVideoContext::clear()` 补 `wiliwili_frame_phase_clear()`（与 `sdl_video.cpp:555` 的 GL 路径同语义）；
- **F5**：提交——35 个未提交文件 + borealis AGC 后端 untracked（见 `notes/03` 备注）整理后提交；
- **F6**：脚本默认值——`build-native.sh` / `install-ffpkg.sh` / `deploy-native.sh` / `launch-native.sh` 的标题号默认值已改为 `PPSA99233`（环境变量/参数仍可覆盖）。

> 区分：以上"已观察"项均有原始日志行或源码 file:line 支撑；`frame:` 根因已由代码与日志数值确认；`to_bytes` 的具体触发输入仍是推测，待 F2 真机复测确认。

**验证结果（2026-10-03 21:52:22 构建，PPSA99233，已 ffpkg 安装运行）**：

- 默认运行：`img-multi: lanes=4 max-inflight=4 async-dns=1`；12 张图片请求全部 `code=200`（10–162 ms，queue ≤180 ms）；`img-net: failed=0`、`img-stall=0`、`terminate:0`、`crash:0`；`frame:` 行 **0**（门控生效）；`agc health` 至 `frame=15600`，`dcb_full=0 ring_fail=0 tex_fail=0 timeouts=0`，`direct_mem=7,794,432/134,217,728`。
- trace 运行（`WILIWILI_TRACE=1` 打包）：`frame: clear=0ms ui=0ms submit=0ms video=0ms swap=15ms calls=0/30` —— 钩子生效，clear/ui 不再是垃圾值；验证后已恢复干净包部署。
- **交互复测（2026-10-04 凌晨，PeaSyo 手柄）**：
  - 搜索链路：Y 键 → TV 搜索页 → 选热词 → 搜索结果页正常；`terminate/crash=0`，搜索接口 200（`search_activity_tv` 的转换路径真机走通）。
  - 直播页 + 动态页：20+ 张图片全 200，`img-net: failed=0`、`img-decode=0`；`img-stall` 命中 10 次，**全部 `op=socket_action`、`running=0 active=1`、1.4–2.1 s**，与 `img-net: total` 的 `tls≈1.4–2.1s / newconn=1` 逐条对应 ⇒ 阻塞 = **新连接的 TLS 握手在 `curl_multi_socket_action` 内同步完成**；旧的 57–113 s 极端值未复现。
- 未覆盖：动态尺寸长测（全屏↔小窗反复切换）；trace 模式 `fps:` 行的相位字段仍是旧记录（只有 slot 1 被置位），不可信，默认运行不受影响。
- 提交：borealis `39f2da9d`；应用层 + notes `f3c7f21`、`228ff02`（未 push）。

**网络队列修复（2026-10-04，构建 `Oct 4 2026 03:26:12`）**：

- 症状：4 条 lane 每条被"新建连接的 TLS 握手"占用 1.4–3.2 s（`img-stall op=socket_action`），无进度请求最长占满 curl 超时（10 s）/watchdog（15 s）⇒ 图片队列排到 2 s 级别。
- 改动（`wiliwili/source/utils/image_helper.cpp`、`config_helper.cpp`）：
  - lane 上限 4 → **8**；PS5 图片线程选项 {4,6,8}、默认 **6**（`img-multi: lanes=8 max-inflight=6`）；
  - 新增**无进度硬期限**：未收到任何响应字节的请求 6 s 就 `finish(...TIMEDOUT)` 释放 lane 并重试（有进度的仍走 `TIMEOUT+5s`）；
  - `CONNECTTIMEOUT` 5 s → 3 s；`LOW_SPEED_TIME` 10 s → 6 s；
  - 连接池 KeepAlive 参数（idle 30 s / 间隔 15 s），减少重复握手。
- 同一直播页复测：队列等待峰值 **2146 ms → 180 ms**；18 条图片请求 `img-net: failed=0`、`crash=0`；`img-stall` 仍有 4 次（1.9–3.2 s，均为 TLS 握手，属控制台 CPU 成本），但不再堵队列；进入真实直播间播放正常（`ring_fail/tex_fail/timeouts=0`，direct_mem 17.8/128 MiB）。
- 未做（后续可选）：HTTP/2 多路复用（需要 curl 带 nghttp2）或连接预热，进一步压缩每新连接 ~2 s 的握手成本。

### 10.29 网络慢握手深挖：payload 模型对比与排除项（2026-10-04）

**问题**：图片新连接偶发 `tls≈1.4–2.8 s`（`img-stall op=socket_action`），6 条 lane 被占住时队列等待可达数秒。

**payload 线的做法（对照）**：`ImageThreadPool`（`cpr::ThreadPool`，min 1 / max `REQUEST_THREADS`，PS5 默认索引 3=4 线程）+ 每张图一个任务、线程内**阻塞 `session.Get()`**（独立 easy handle）；**没有 lane/watchdog/重试/队列重启**，只共享 DNS（SSL session/连接共享在 payload 里同样是注释掉的 TODO）。curl 同为 8.18.0。

**本批已提交的缓解（`3396572`）**：lane 4→8、默认并发 4→6、无进度 6 s 硬释放、`CONNECTTIMEOUT` 5→3 s、`LOW_SPEED_TIME` 10→6 s、KeepAlive idle 30 s。首轮复测队列等待峰值 2146→180 ms，但慢握手本身仍在。

**排除项（全部有真机/PC 证据）**：
1. 网络与 CDN：PC 到 `i0/i1.hdslb.com`、`album.biliimg.com`、`api.bilibili.com` 的 TLS 17–47 ms；PC 8 并发同样 23–37 ms。
2. 标题内**裸** `SSL_connect`（阻塞 socket，TLSv1.3）= **6 ms**；加 `SSL_VERIFY_PEER` + 载入 185 KB ca-bundle 仍 6 ms；`RAND_bytes` 1 ms、CA 加载 13 ms、`/dev/urandom` 可打开（`fd=13`）⇒ 熵/CA/校验/CPU 全排除。
3. `CURLOPT_IPRESOLVE_V4` 后 stall 依旧（`img-stall … peer=v4:106.225.x`）⇒ 双栈/Happy Eyeballs 排除。
4. stall 点 `ph=dns/conn/app` 显示耗时在 TLS 段；socket 标志 `fl=0x4`（O_NONBLOCK=4 on this libc）⇒ 非阻塞标志正常。
5. in-process `curl_easy_perform` 变体（minimal / +low-speed+KeepAlive / +进度回调 / +CERTINFO）在 preinit 全部 **28–55 ms** ⇒ 这些 session 选项无罪。
6. 把图片 runner 换成 payload 同款（每请求一线程 + 阻塞 easy，启动行 `img-threads: workers=8`）后**慢握手照旧（1.4–2.7 s）**；同期 API 路径也出现 `http: slow 10989ms dns=3 tcp=3 tls=2813 first=7487 code=200 err=8` ⇒ **与 HTTP 客户端模型无关**；而同一时刻后台探针的裸握手仍是 5–7 ms（同一进程）。

**结论/未解**：慢的是"curl 传输在标题运行时里"这一路径，而裸握手不慢；原因仍未定位（线程亲和、sandbox 网络栈在 curl 路径上的某些调用、或 curl 内部 poll 行为都还是候选）。实验用的"阻塞 runner"与 curl 变体探针已回滚，正式线保持已提交的 multi runner；只保留一次性探针：`WILIWILI_CRYPTO_PROBE=1`（options 文件）→ `crypto:` / `tls:` 行（`native_shims.c`）。

**下一步建议**：① 给 `img-net` / `http: slow` 日志加 URL/host 维度，确认是否与特定主机相关；② 在 payload 环境用同一探针做 A/B（需一次 payload 构建）验证"payload 不慢"这一前提。

**追加探针（2026-10-04 07:1x–07:25，全部 `WILIWILI_CRYPTO_PROBE=1` 一次性触发）**：

- 并发裸握手（4 线程同主机）：`handshake=35/54/56/57 ms`（单条 6–9 ms）⇒ **并发新建连接不是瓶颈**。
- 裸握手 + SNI + ALPN(`h2,http/1.1`) + `TCP_NODELAY`（对齐 curl 的 ClientHello）：`handshake=9 ms`。
- `select` 等待可读 vs 阻塞 `read`（同一条 :80 连接）：`28 ms vs 6 ms` ⇒ select 唤醒正常。
- 结论：标题内**所有裸层路径都正常**（6–57 ms），唯一能复现 1.4–2.8 s 的只有 "curl 传输" 本身（multi 与 blocking-easy 两种驱动都一样）。

**新增日志维度（本批代码）**：`img-net: total=...`、`img-net: failed ...`、`http: slow ...` 现在都带 `host=` 与 `up=`（进程内 uptime ms），便于与 UDP 时间戳对齐。首份样本（2026-10-04 07:14，直播页加载）：4 条**同时**新建连接（`up≈43.1s`）到 `i0.hdslb.com`，各自 `tls=1722/1734/1736/1736 ms`、全 200；而同一次启动的首屏（`up=0–368 ms`）i0/i1/i2 请求只有 7–348 ms ⇒ 慢事件与"某次页面加载时刻的批量新连接"相关。

**payload 环境 A/B（2026-10-04 07:4x–07:5x，`ps5_net_probe.cpp` 一次性探针，A/B 后已删除）**：

- payload 线（同一主机 `i0.hdslb.com`、同一个 curl、CA 显式 `CURLOPT_CAINFO`）：裸握手（SNI+ALPN+校验）**8–9 ms**、4 并发裸握手 **8–15 ms**、curl 单条 **24–103 ms（tls 19–50 ms）**、curl×4 并发新建连接 **116–303 ms（tls 111–178 ms）**，全部 `code=200`。
- native 标题线同代码：同一页面加载时刻 5 条并发新建连接各自 `tls≈2.15 s`。⇒ **"payload 不慢" 的前提成立**，差异在标题的运行环境侧。
- 注意：payload 环境没有默认 CA 路径，探针里必须显式 `CURLOPT_CAINFO=/data/homebrew/wiliwili/ca-bundle.crt`（否则 curl `rc=77`、裸握手失败）。

**native 标题侧对照补齐（2026-10-04 08:0x，同一次进程）**：

- 裸握手 3 变体 7–11 ms；4 并发裸握手 32–64 ms；`select` 等待 6 ms；`poll` 等待 6 ms（`rc=1 revents=1`）；阻塞 `read` 7 ms。
- **探针 curl（easy、新建连接、同一个 CA）61–203 ms**；关键对照：app 的 5 条并发图片请求 `tls=2155–2173 ms`（`up≈31 s`，全部同刻完成）**期间**，探针 curl 仍为 61 ms。
- 慢样本的 socket fd = 71–81（快样本 44–59），fd 上限 13952 ⇒ `FD_SETSIZE`（1024）假设排除。
- 结论：慢只出现在"**app 自身、多条并发新建连接的 curl 传输**"这一组合；单条 curl、裸并发握手、等待原语（select/poll/阻塞读）、fd 编号都不是原因。`img-net` 行现在带 `fd=` 便于继续观察。

**单变量 A/B：HTTP/1.1（2026-10-04 08:14）**：图片 runner 强制 `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_1_1` 后，同页面驱动仍出现 4 条 `tls=1592–1631 ms`（`i0.hdslb.com`、`newconn=1`、同刻完成），与基线 2155–2173 ms 同形态 ⇒ **h2 不是原因**（实验已回滚）。

### 网络慢握手根因（2026-10-04 08:2x–08:4x）

**轨迹证据**（`WILIWILI_IMG_TRACE` 式逐事件时间戳，单条慢传输 1801 ms）：

```
+2ms    DNS 已解析 → Trying connect
+21ms   ssl-out 1555B（ClientHello 已发出）
+28ms   SSL Trust Anchors:            ← curl/OpenSSL 开始建信任库
+1727ms   CAfile: /app0/assets/ca-bundle.crt   ← 1.7 s 全花在这里
+1727ms ssl-in → Server hello
+1788ms SSL certificate verified via OpenSSL / Established connection
```

**离线对照（`caprobe`，同一进程）**：

| 场景 | `SSL_CTX_load_verify_locations(188905 B ≈ 140 张 CA)` |
|---|---|
| 单条 | 11–12 ms |
| 顺序 5 次 | 13 / 21 / 13 / 47 / 163 ms |
| **并发 5 次** | **2242–2263 ms**（全部同刻完成） |
| 同一文件的纯 `read` | 0–1 ms（188905 B 全部读出） |

⇒ **根因**：每建一条新 TLS 连接，libcurl 都会为这个 easy 句柄新建 SSL_CTX 并重新解析整个 CA bundle；该调用在标题运行时里被进程内共享资源串行化（顺序 55 ms → 并发 5 路 2.2 s，且同刻完成）。页面加载时一次开 5–6 条新连接，于是每条都 ~2 s，表现为"图片/接口间歇 1.5–2.8 s"；单条探针 curl（无并发）与 payload 环境（并发解析不慢）都不复现。与 h2、fd、select/poll、SNI/ALPN、网络、CDN 均无关（见上）。

**修复方向**：① 裁剪 CA bundle 到 B 站实际使用的 CA（保持 `SSL_VERIFY_PEER`，只是少解析无用证书）；② 或让 CA 解析只发生一次（共享 SSL_CTX/预解析，curl 无原生支持，需要在 curl 层定制）。先测 ①：5 张 / 40 张证书的并发解析耗时对比（`casubset` 探针）。

**并发代价随 bundle 大小缩放（`capar` 探针，5 路并发）**：

| bundle | 大小 | 证书数 | 单条 | 5 并发（各自） |
|---|---|---|---|---|
| `/app0/assets/ca-bundle.crt` | 188905 B | ~140 | 11 ms | **2214–2223 ms** |
| `probe-ca-40.crt` | 64053 B | 40 | 3 ms | 674–680 ms |
| `probe-ca-5.crt` | 7211 B | 5 | 1 ms | **111–113 ms** |

⇒ 并发代价与证书数量近似线性；把 bundle 裁到 ~5–10 张（B 站实际用到的 GlobalSign 等）可把每连接开销从 ~2.2 s 降到 ~0.1–0.2 s，`SSL_VERIFY_PEER` 保持不变。另一个方向是找到并消除这个进程内串行化（顺序 5 次只要 55 ms，说明不是 CPU 而是锁/等待；嫌疑：curl 用的 stdio/allocator 在 clean-room 运行时里的全局锁）。

### 修复：裁剪 CA bundle（2026-10-04 08:4x）

- 新增 `scripts/ps5/native/ca-bundle-trimmed.crt`（7 张证书 / 10291 B：GlobalSign R46/E46/R3 交叉签、DigiCert G2、ISRG X1、Sectigo R46、Amazon R1），生成方式写在 `scripts/ps5/native/make-ca-bundle.sh`；`native_build.py` 优先取它，缺省回落到 SDK 全量 bundle。`SSL_VERIFY_PEER` 保持开启，用 `openssl s_client -CAfile` 对 i0/i1/api/passport/grpc/bilivideo 全部验证 `code 0 (ok)`。
- **真机验证（同一键位驱动）**：新建连接的 `tls` 从 **2155–2173 ms（5 并发）→ 238 ms**；≥1 s 的慢样本从 4–5 条降到 2 条，且剩下的两条是 `newconn=0 tls=0 first=total`（复用连接上的**响应体下载**慢，属另一类问题，非握手）。
- 队列层缓解（8 lane、无进度 6 s 释放、短超时）继续保留：修复前它是唯一止血手段，修复后仍是对 CDN 侧抖动的防御。

### 沙箱目录遍历实测（2026-10-04，`dirprobe`，`WILIWILI_CRYPTO_PROBE=1`）

| 路径 | `opendir` | errno | 说明 |
|---|---|---|---|
| `/app0`、`/app0/assets` | 失败 | 1 (EPERM) | 应用镜像自身目录也不能列 |
| `/download0` | 失败 | 1 (EPERM) | 可写目录也不能列 |
| `/` | 失败 | 1 (EPERM) | — |
| `/data/homebrew/wiliwili` | 失败 | 2 (ENOENT) | 标题命名空间里**看不到 /data** |
| `open("/app0/assets/ca-bundle.crt")` | 成功（fd=13） | — | 按已知名字打开文件正常 |

⇒ 之前 `native_build.py` 里"沙箱禁止目录遍历"只是注释断言，现已有实测依据；**外置资源/CA 只能"按清单逐个按名字打开"，不能遍历目录**；且外置文件只能放 `/download0`（可写且在命名空间内），`/data` 不可见。payload 线不受此限（桌面式环境，`fetch_payload.c` 里 opendir 正常）。
