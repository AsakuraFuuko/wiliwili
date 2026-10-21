# 11 — 音频移出 mpv：应用自有 FFmpeg + sceAudioOut 路径

> 日期：2026-10-21
> 范围：只把**音频输出**从 mpv 移走；mpv 仍负责 A 路视频、字幕渲染、软解兜底和播放时间线。
> 参考实现：`references/EVO-PLAYER-PS5/projects/evoplayer/media/src/evo_audio_out.c`（GPL-3.0，与 wiliwili 兼容）——block ring + 阻塞式 `sceAudioOutOutput` + 音频主时钟的思路。

## 1. 结论

- 新增 `scripts/ps5/native/native_audio.c`：libavformat/libavcodec 解复用+软解、libswresample 转 S16 stereo 48 kHz、block ring、`sceAudioOut` 输出（失败退回 SDL2 音频）。
- mpv 的 `ao` 在原生线改为 `null`（仍然解音频、仍然推进 A/V 时钟，但不再出声）。设备探测失败或新路径运行中失败时，通过 `ao=sdl` + `ao-reload` 把声音接回 mpv，播放不中断。
- 音频时钟是主时钟：`wiliwili_audio_clock()` 给出「当前可听样本」的媒体时间；`MPVCore::updateNativeAudioSync()` 用它与 mpv `playback-time` 的差微调 mpv `speed`（±1%），A 路视频与 C 路 `playback_time` 都跟随。
- 暂停/恢复/seek/切集/EOF 全部走音频模块自己的状态机；EOF 与 mpv 结束事件都会暂停并把端口关掉，退出播放器时同步 join 两个线程、关闭端口。

## 2. 结构

| 文件 | 作用 |
|---|---|
| `scripts/ps5/native/native_audio.c` | 新音频路径（解复用、解码、重采样、block ring、`sceAudioOut`/SDL 输出、时钟、暂停/seek/切轨/EOF、漂移日志） |
| `scripts/ps5/native/native_build.py` | `extra_sources` 增加 `native_audio.c` |
| `wiliwili/source/view/mpv_core.cpp` | `ao` 选择；pause/resume/seek/stop/volume/speed 挂接；`updateNativeAudioSync()`（漂移校正）；`fallbackToMpvAudio()`；EOF/END_FILE 静音；播放列表前进时回退 |
| `wiliwili/include/view/mpv_core.hpp` | 上述方法的声明与校正状态 |
| `wiliwili/source/view/video_view.cpp` | 起播时把音频 URL（DASH 独立音轨或单文件 URL）交给新模块；draw 里检查运行期失败并回退；`WILIWILI_AUDIO_AUTOTEST` 真机取证钩子 |
| `wiliwili/include/view/video_view.hpp` | autotest 状态 |

### 2.1 音频线程与数据流

```
decode thread: av_read_frame -> libavcodec -> swr(S16/48k*stereo) -> block(1024 frames)
             -> ring (最多提前 4 block ≈ 85 ms)
output thread: ring -> 音量 -> sceAudioOutOutput(阻塞) / SDL_QueueAudio + 队列深度限流
```

- block 带 `gen`：seek/切轨时 UI 线程立即清 ring、`seek_gen++`，旧代 block 在出队时丢弃，不会重放 seek 前的声音。
- seek：`avformat_seek_file(..., AVSEEK_FLAG_BACKWARD)` + `avcodec_flush_buffers` + swr 重建 + `discard_until = target`（丢弃关键帧到目标之间的 run-up）。
- 时钟：每块写入前记 `(block.pts, now)`，读取时按挂钟插值并夹在块跨度内，因此报告的是**正在出声**的媒体位置（块 21 ms）。
- 速度：swr 输出率 = 48000 × speed，媒体每输出帧前进 `1/(48000×speed)` 秒；倍速播放有音高变化（未做 time-stretch），漂移校正本身不改变音频速率。
- 音量：输出前按 0–100 线性缩放（mpv 的 `volume` 属性在 `ao=null` 下不再影响声音）。

### 2.2 回退链

1. 启动时 `wiliwili_audio_available()` 只探测一次 `sceAudioOutInit`（`sceAudioOutInit` 是进程级一次性初始化，
   重复调用会返回错误）；失败则 mpv 直接用 `ao=sdl`。真实端口在每次起播时同步打开，失败立刻回退。
2. 每次起播 `wiliwili_audio_start()`：网络/解码在线程里，但端口在调用线程同步打开；线程创建、URL 为空、
   端口被拒等同步失败 → 当场 `FALLBACK_MPV_AO reason=start-rejected`，此后本进程不再尝试新路径（避免双路出声）。
3. 运行中打开/解码/端口失败由 `wiliwili_audio_take_failure()` 上报（在 `VideoView::draw` 轮询）→ 停新路径 + `ao=sdl`/`ao-reload`，mpv 的音频链在当前位置继续出声。
4. mpv 播放列表自动前进（备用视频 URL 接管）时，`MPV_EVENT_FILE_LOADED` 发现 `path` 与起播 URL 不同 → 同样交还 mpv。

## 3. 真机验收步骤

```bash
# 装置（flock 串行）
flock -w 3600 /tmp/ps5-dev.lock -c 'bash scripts/ps5/native/test-cycle.sh PPSA99233 <listen_seconds>'
# 导航：历史页 = up×8 → down×2 → cross → down → cross
# 日志：共享 UDP 9999 listener（不要重启）
```

观察点：
- `audio: mpv ao=null (app audio path owns output)`、`audio: probe ps5 audio available`
- `audio: demux ready codec=... rate=... streams=...`、`audio: output backend=ps5 handle=...`
- `audio: clock=... mpv=... drift_ms=... max_ms=...`（2 s 一条）
- `audio-test: seek/pause/resume/switch-track`（`WILIWILI_AUDIO_AUTOTEST=1`）
- EOF：`audio: eof decoded_frames=...` + `audio: output drained ...` + `audio: summary ...`
- 回归：`agc health` 三项 0、无 `FALLBACK_A`、无 crash；`mpv: audio active ... ao=null`

## 4. 证据

### 4.1 旁证（**来自含 C 路半成品代码的别人构建**，只能证明音频链路在跑，不作为最终验收）

构建 marker `Oct 21 2026 19:52:02`，视频 `BV`/cid `42689235040`（63 s，HEVC DASH，A 路），日志：

- `audio: probe ps5 audio available`、`audio: mpv ao=null (app audio path owns output)`
- `audio: start tracks=3 start=-1.000`（3 个 DASH 音频镜像 URL）
- `audio: demux ready codec=aac rate=44100 ch=2 tb=0.000023 duration=63.065 streams=1`
- `audio: output backend=sdl`（**直连 sceAudioOut 未成功**；当时 `audio_ps5_open` 与探测各调用了一次
  `sceAudioOutInit()`，第二次返回错误被当成失败。SDL 驱动因此接管，`blocks` 持续增长、`underrun=0`）
- 2 s 采样的 `audio: clock=… mpv=… drift_ms=…`，`blocks≈47/s`；`max_ms=137.3` 出现在起播后 2 s 内，
  之后长期在 ±20–70 ms 摆动。摆动主要来自 `playback-time` 属性 ~100 ms 的更新粒度（本次改动已改为
  用挂钟外推后再比较）。

## 4.2 待补（最终构建）

- 短/长视频有声（`backend=ps5` 或 `backend=sdl driver=ps5`）
- `WILIWILI_AUDIO_AUTOTEST`：seek 20 → pause → resume → seek 5 → switch-track 的日志与漂移
- EOF：`audio: eof` + `audio: output drained`，画面定格、无后续 `audio: clock` 增长
- 退出：`audio: summary`，之后无音频日志
- 长视频 10 分钟：`drift_ms` / `max_ms` < 100 ms
- 回归：health 三项 0、无 `FALLBACK_A`、无 crash、封面/HUD/全屏/EOF 行为不变

## 5. 已知边界 / 未验证

- 倍速播放是重采样（音高随速度变化），没有做 `atempo`/time-stretch。
- FLV/单文件走两条 FFmpeg 连接（视频一条、音频一条），网络开销按音轨码率增加；DASH 本来就是分轨。
- 音频设备的实际吞吐若与标称 48 kHz 有偏差，漂移会体现在 `drift_ms`；校正只作用于 mpv 时钟，不改音频时钟。
- 漂移指标同时记录 `max_ms`（含起播瞬态）与 `settle_max_ms`（起播 10 s 之后、且跳过每次 seek/切轨后 4 s 的取流期），
  验收以 `settle_max_ms` 为准。
- seek/切轨后音频要重开 demuxer，声音会短暂落后；此时把 mpv `speed` 压到 0.25x 等声音追上（`audio: sync hold/release`），
  不碰 pause 状态。A 路画面在这 1 s 左右会接近定格，属预期。
- 未能直接录制主机声音：EOF/退出的“无残留”以端口关闭日志 + 线程 join 为证据，主观听感未验证。
