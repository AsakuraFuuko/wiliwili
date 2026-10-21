# 12 — 跨页纹理/画布释放（详情页、播放器、全屏）

目标：退出详情页/播放器后，页面独占的图片纹理与不再需要的画布**立即**回到 AGC 直连内存池，
`direct_mem` 回落接近首页水平，反复进出不累积；返回列表封面可重新加载；播放器内的相关推荐不释放。

## 1. 基线与定位

**改动前基线**（`0df05c1` 及之前，见 §本项目上下文与 `10-full-functional-sweep.md`）：
首页空闲 `direct_mem≈22MB/256MB`，进详情页/播放器升到 **111–123MB**，退出后不回落。

**定位**（`0df05c1` + 临时插桩跑一次，日志 `/tmp/xpage.log` 19:24–19:34，改动前）：

| 时刻 | TextureCache | 直连池 | 快照精灵 | 播放面 |
|---|---|---|---|---|
| 首页 | 35 项 / 21.95MB（33 项被引用） | 23.29MB | – | – |
| 进详情页 | 60 项 / 36.78MB（**全部被引用**） | 39.2MB | – | – |
| 进播放器 | 78 项 / 37.07MB | 39.7MB | – | 800×450 |
| 播放器稳定 | 94 项 / 38.54MB（ref=94） | **95.5MB** | **51.84MB（1 张 3600×3600）** | 1.44MB |

结论：退出后不回落的三块持有者都在页面自己身上，而非泄漏：

1. **`TextureCache` 只按字节预算（32MiB）淘汰**。页面退出后其封面 `ref` 归零，但只要总量没超预算就不会被
   回收——旧页面留下的 20–38MB 会一直躺在池里，等下一页的图把它挤出去。`ref=94` 说明页面上引用计数正确，
   问题只是"归零后没人收"。
2. **`VideoSnapshotCore` 的进度条预览精灵图**：B 站高能进度条精灵图单张 3600×3600 RGBA = **51.84MB**，
   只在 `MpvEventEnum::RESET` 时释放。退出播放器走的是 `video->stop()`（不动 mpv 事件），所以退出后
   整张精灵图仍在池里。
3. **mpv 软件视频面**：`MPVCore::setFrameSize()` 分配 `nvg_image`（NVG 纹理）+ `pixels`（CPU 暂存），
   退出播放器没有任何释放路径（只有应用退出时 `getExitDoneEvent()` 才 `free(pixels)`），
   800×450 时 1.44MB，全屏/大窗时 8–9MB。

## 2. 改动

### 2.1 `TextureCache`：页退出时释放"无人引用"的纹理

`library/borealis/library/include/borealis/core/cache_helper.hpp`

- `LRUCache::purgeUnreferenced()`：删除所有 `count == 0` 的条目并返回数量；`count > 0`（可见视图仍在画）
  一律保留。
- `LRUCache::getReferencedBytes()/getUnreferencedCount()`：诊断用引用统计。
- `TextureCache::requestPurgeUnreferenced()` / `processPendingPurge(bool viewsPending)`：
  弹出页面时置位，`Application::frame()` 在**删除视图池处理完之后**执行。`viewsPending`（池非空）保证
  "被在飞图片请求锁住的 Image 视图"晚一两帧才真正析构时，purge 仍然保持挂起，等它们释放后再收一次。
  没有页面弹出时 purged 标志为 0，正常浏览（打开/关闭对话框）不会清缓存。

`library/borealis/library/lib/core/application.cpp`

- `popActivity()` 的删除回调里 `requestPurgeUnreferenced()`（仅 `free == true`）。
- `frame()` 删除池之后 `processPendingPurge(!undeletedViews.empty())`。

删除纹理走 `nvgDeleteImage()`，AGC 后端已把它放进 `retired_textures`（约 4 帧后归还直连内存，见
`10-full-functional-sweep.md`），因此退出后 1–2 个 5s 采样就能看到池回落，且不会出现"GPU 仍在读的纹理被
提前释放"。

### 2.2 播放器退出：释放快照精灵图与视频面

- `BasePlayerActivity::~BasePlayerActivity()`（UGC 与番剧播放页共用）在 `video->stop()` 之后：
  `VideoSnapshotCore::instance().reset()` + `MPVCore::instance().releaseVideoSurface()`。
  - `reset()` 自增 `snapshotGeneration`，在飞的回调按世代丢弃，不会解绑新请求或释放新纹理。
  - `releaseVideoSurface()`（`mpv_core.cpp`，`MPV_SW_RENDER` 下）：先清 `pixels/nvg_image/mpv_params[3].data`、
    `sw_size/pitch/redraw`，再 `nvgDeleteImage` + `free`。`on_update()`/`draw()`/`mpvRenderContextRender()`
    全部在主线程且都以 `pixels` 为空提前返回，所以先置空再释放是安全的；页面已从活动栈移除、不会再绘制。
- `LiveActivity` / `DLNAActivity` 析构同样释放视频面（直播间/投屏也是播放页）。

### 2.3 诊断（`WILIWILI_MEM_TRACE=1`）

`ImageHelper::logVideoMemory(tag, dumpEntries)` + `startMemoryTrace()`（首次 `with()` 时挂 run-loop，
每 5s 一行，经 `wiliwili_boot_log` 走 UDP）。一行包含：缓存项数/字节、被引用字节、无人引用项数、
快照精灵字节/张数、直连池 used/total/peak/allocs。`dumpEntries` 额外逐项打印纹理尺寸、字节与引用计数，
用来区分"仍在显示"和"忘了释放"。默认不开，只在 `assets/wiliwili-options.txt` 写 `WILIWILI_MEM_TRACE=1`
时生效。

## 3. 真机证据（build marker `Oct 21 2026 19:52:02`）

测试片：首页第一张（1:03）、刷新后的长视频（48:45）等；导航 `right/down` 选卡、`cross` 进详情、
`cross` 全屏、`circle` 退全屏、`circle` 退播放器。

| 时刻 | 场景 | direct_mem（池） | HUD |
|---|---|---|---|
| 19:55:20 | 首页空闲 | 23,290,624 B（22.2MB），cache 35 项/21.95MB，ref 21.1MB | `MEM:22.2/256MB` |
| 19:57:01 | 进详情页（第 1 轮） | —（快照 51.84MB + 面 800×450） | `MEM:73.5/256MB` |
| 19:57:35 | 全屏（有画面，FPS 60，00:49） | peak 78,160,640（74.5MB） | `MEM:73.6/256MB` |
| 19:57:43 | 退全屏回小窗，时间 00:52（**未 seek/重播**） | 持平 | `MEM:73.5/256MB` |
| 19:58:16 | 退播放器（`player-exit`） | 快照 51.84MB→0，面 `released surface image=60` | — |
| **19:58:20** | **返回首页（退出后 4s）** | **23,617,024 B（22.5MB）**，cache 67→**33 项/21.14MB** | — |
| 20:02:27 | 第二次退播放器（另一视频） | 同样：快照→0、面释放 | — |
| **20:02:31** | **返回首页** | **23,617,024 B（22.5MB）**，cache **33 项/21.14MB** | `MEM:22.5/256MB`（20:02:38 / 20:03:57 截图） |

- `agc health` 全程 `dcb_full=0 ring_fail=0 tex_fail=0 timeouts=0 vo_rc=0`；页面峰值
  `direct_mem peak=78,160,640`（74.5MB），首页平台期 `peak=23,290,624`。
- 两轮退出后池字节**完全相同**（23,617,024），cache 项数都是 33、引用字节 21,136,528 ⇒ 不累积。
- 截图：`/tmp/xp-home0.png`（首页 22.2）、`/tmp/xp-detail-r1.png`（详情页 73.5，评论/相关正常）、
  `/tmp/xp-full-r1.png`（全屏 73.6，有画面）、`/tmp/xp-unfull-r1.png`（回小窗 73.5，00:52）、
  `/tmp/xp-back-r1.png`（返回首页 22.5，封面全部正常显示）。
- 回归：全屏有画面且 FPS 60；全屏往返时间连续（00:49→00:52）不 seek；播放结束定格（`eof=1`、时钟冻结、
  无自动重播）；无 crash/无 `terminate:`；AGC health 三项全程 0。

### 未完成/待复测

- **第 3 轮进出与"全屏进出"的完整截图组**未拍完：本轮测试中控制台被并行任务重装覆盖（20:04 包被替换，
  进程重启），第 3 轮与干净的全屏组需在 C 路构建合入后重跑（同一脚本，见 §4）。
- 第 1 轮测试片选中了 1:03 的短片，播放结束（EOF）后再操作触发了既有的"旧续播提示"路径：
  `seek target=739.000` → `demux EOF` → `FALLBACK_A reason=stream-count-mismatch rc=-9019`（干净回退，
  A 路继续、无 crash）。该事件发生在**退出播放器之后、重新打开视频时**，与本改动无关（本改动不碰
  加载/seek 路径），但复测时应选 ≥5 分钟的视频避免 EOF，以便给"无 FALLBACK_A"留下干净记录。

## 4. 复测步骤

```bash
# 构建（C 路完成后）
flock -w 3600 /tmp/ps5-dev.lock -c 'env -u PS5_NATIVE_OSMESA_DIR -u PS5_NATIVE_OSMESA_LLVM PS5_NATIVE_AGC=1 \
  PS5_NATIVE_SDL2_PREFIX=$PWD/../ps5-native/ps5-opengl/build/native-sdl2-audio/sdk \
  PS5_NATIVE_CDB=$PWD/build-ps5/compile_commands.json PS5_NATIVE_TITLE_ID=PPSA99233 \
  bash scripts/ps5/native/build-native.sh'
# 装机（/tmp/xp-install.sh 复用共享的 9999 listener，不额外起监听）
flock -w 3600 /tmp/ps5-dev.lock -c 'bash /tmp/xp-install.sh'
# 交互：right/down 选长视频 -> cross 进详情 -> cross 全屏 -> circle 退全屏 -> circle 退播放器；
#       每步 screenshot，3 轮
```

`assets/wiliwili-options.txt` 写 `WILIWILI_MEM_TRACE=1` 即得 `mem-page:` 行（5s 一行，含缓存/引用/快照/池）。
