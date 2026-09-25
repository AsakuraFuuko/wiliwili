# N0 进展：payload 线代码 → 原生标题跑通（2026-09-25 真机）

## 结果
- 新树 `wiliwili-native/` 构建出原生标题 **PPSA99013**（`eboot.bin` 89.2 MB / ffpkg 105.5 MB），安装到 `/data/homebrew/PPSA99013.ffpkg`，经 `launch-native.sh` 启动 ✓，**电视上有画面** ✓。
- 启动序列（UDP 9999 实录）全部命中：
  `preinit` → `build Sep 25 2026` → `constructors: start/done` → `main entered` → `net init=0 pool=3001 ssl=2` → `sdl: video driver=ps5-g19` → `sdl: window 1920x1080` → `sdl: gl vendor=PS5 homebrew renderer=PS5 AGC version=3.3 (Core Profile) Mesa 26.2.0` → `main activity: content available` → `http: code=200`。
  ⇒ 确认走的是 **AGC 上的 GL 3.3 core**（不是 llvmpipe）。

## 迁移中修掉的三个真问题（都已落在新树里）
1. **CDB 生成**：payload 构建不产 `compile_commands.json`。`ninja -t compdb` 的版本①带 `-MD/-MT/-MF`（native 构建 `-Werror=unused-command-line-argument` 直接挂 40 个 TU）②路径按*构建目录*相对（native 按仓库根解析）。⇒ 过滤依赖参数 + 补 `build-ps5/` 前缀。
2. **`native_shims.c` 守卫不一致**：`wiliwili_wx_probe()` 定义在 `#if defined(WILIWILI_OSMESA_PROBE)` 内，调用点无守卫 ⇒ **GL 路线必编译失败**（老树只跑软渲染路线，所以从未暴露）。⇒ 调用点补同宏守卫。
3. **romfs 资源 TU 重复**：路径修好后 `libromfs_resources.cpp` 被 native 构建重编译，与链接进来的 `libromfs-wiliwili.a` 符号冲突（`RomFs_wiliwili_*`）。⇒ 从 TU 清单移除（282 → 280）+ 清残留对象。

## 性能：一个吃掉整个帧预算的热路径问题（已修）
- 现象：标题能跑但"帧数有点低"。
- 取证：UDP 日志实测 **一分钟 1.6 万行**，其中 16923/17337 行是 ps5-opengl 驱动的 `[ps5-gallium] first-vertex …` / `[ps5-multidraw-batch] …` / `[ps5-deferred-batch] …`（`ps5_screen.c:9490`、`ps5_agc_native_runtime.c:1273` 等都是**未加条件的 printf**，每个 draw batch 一行）。
- 代价：`wiliwili_boot_log` 每行一次 `open + write + **fsync** + close` 外加一个 UDP 数据报 ⇒ 约 **280 次 fsync/秒**。
- 修复：在日志出口（`native_shims.c` 的 `wiliwili_boot_log`）过滤 `[ps5-` 前缀行（`trace` 开关打开时保留）；另把 stdout 默认丢弃（`freopen("/dev/null")`，trace 时保留）。
- 效果：日志噪声 **16923 行 → 0 行**（75 秒仅 122 行有效日志，主要是 DNS/curl 解析跟踪）。

## 待确认 / 下一步
- 帧率数字：应用自身只在 `WILIWILI_TRACE=1` 时打 `sdl: N fps`（而 trace 本身会把帧率压到 ~5 fps，不能用作测量）⇒ 用 Onion HUD 读数为准。
- 剩余可选优化：DNS/curl 跟踪行（约 1.3 行/秒）也可加开关；驱动层若还要更多性能，见 `00-plan.md` 的 N1/N2（AGC 直接接管、硬解）。

## 视频红蓝互换（已修，2026-09-25 真机反馈）
- 现象：原生标题里播放视频，颜色 R/B 互换。
- 根因：native GL 路线的 nanovg 会把最终颜色按 **BGRA** 写出（`borealis/extern/nanovg/nanovg_gl.h:697` 的 `outColor = result.bgra`，注释写明 "video-out displays BGRA"），UI 因此正确；而 `MPV_NO_FB` 是**无条件定义**（`wiliwili/include/view/mpv_core.hpp:40`）⇒ mpv 直接渲染进同一个 framebuffer 且写 **RGBA** ⇒ 视频的 R/B 相对 UI 反了。
- 修复：挂一段 mpv 用户着色器，只改 **MAIN pass**（原地，不额外增加渲染遍）：
  ```
  //!DESC wiliwili: swap red/blue (video-out is BGRA, mpv writes RGBA)
  //!HOOK MAIN
  //!BIND HOOKED
  vec4 hook() { return HOOKED_tex(HOOKED_pos).bgra; }
  ```
  由 `MPVCore::colorSwapShaderPath()` 在配置目录生成 `rb-swap.hook`，`init()` 里 `--glsl-shaders` 挂上；`setShader()` 把内置脚本**排在最前**、`clearShader()` **只清用户脚本**，所以切换画质档位不会把它清掉。
- 作用域：`#if defined(PS5_NATIVE_APP) && !defined(WILIWILI_SOFTWARE_RENDER)` —— 只影响原生 GL 路线，payload 线与原生软渲染路线均不受影响。

## 播放器崩溃排查（2026-09-25 深夜，未收敛但有明确边界）

**现象**：点击视频 / 用 `WILIWILI_TEST_BV` 直进播放器 ⇒ SIGSEGV（`fault addr=0`）——对 NULL 的函数调用。

**已排除**：
- **不是颜色修复引入的**（把 `colorSwapShaderPath()` 临时改成返回空串后仍崩）；
- **不是 mpv 的函数指针解析问题**：非 bundle 构建里 `mpvSetOptionString`/`mpvCreate` 等是宏，直接链接（`mpv_core.hpp:111-124`）；`mpv_create()` 实测成功返回；
- **不是 mpv 的日志/终端后端**：已按平台关闭 `terminal` 与 `msg-level=all=v`（前者默认 false 本就不执行），并显式设 `msg-level=all=no`；崩点从"日志类选项"推进到选项列表之后（即 `mpv_initialize` 附近）**仍然崩**；
- **不只是 mpv 缓存内存**：把 `demuxer-max-bytes/back-bytes/readahead/vd-lavc-threads/audio-buffer` 压到最小后同样崩（注：缓存在 init 之后才分配，此测法本身不充分）。

**已定位到**：`VideoView` 构造 → `MPVCore::instance()` → `MPVCore::init()` → 选项设置完成后（`mpv_initialize` 一带）跳到 NULL。

**工具限制**（重要，避免重复踩）：标题进程内核**不填 user context**（`native_shims.c` 有注释），所以 `rip/rsp` 无效、`_Unwind_Backtrace` 只有崩溃处理器自己一帧；只能用 `stk:` 栈扫描 + 镜像基址（= `base - link(wiliwili_boot_log)` = 0x400000）+ `llvm-nm -n` 最近符号还原，且结果混有栈上残留值。`app-log.sh` 拉 `download0.dat` 很慢（>5 分钟），实时日志用 UDP 9999。

**结论与建议**：mpv 在本机 app slot 环境下初始化即失败，深挖需要 core dump（控制台 `/devlog/system/sce_coredumps.0/` 未取到）或 mpv 侧更细打点。**更划算的路线是绕开 mpv**：按 `02-hwdecode.md` 的 P0/P1 走 "ffmpeg 自管视频（解码+自绘）+ mpv 只保留音频与时钟"——那条路同时也是硬解的前置，且能顺带把内存基线降下来。
