# 13 — 续播越界 seek 夹紧 + 音频路径真机验证

> 日期：2026-10-22 夜。包 `PPSA99233`，主机 `192.168.102.118`。
> 提交：`0bc243c`（修复）+ 本文所属提交（验证钩子与证据）。
> 相关：`notes/10-full-functional-sweep.md`（LAST_TIME/全屏 seek 历史）、`notes/11-audio-own-path.md`（音频路径本体）。

## 1. 结论

- **越界 seek 的根因有两条**，两条都要处理：
  1. 目标**超过片长**（B 站历史续播点、片尾快进）→ C 路 `avformat_seek_file` 直接落到 EOF，首帧前就 `demux EOF inputs=0` → `FALLBACK_A reason=stream-count-mismatch rc=-9019`。现已在三处夹紧：C 路 API、C 路 worker、应用侧 LAST_TIME 判定（外加音频侧同界夹紧）。
  2. 目标**在片内但落在最后一个 GOP**（该 GOP 之后没有真 IDR）：C 路的 `sequential_seek` 只接受「pts ≥ 目标的关键帧」，等不到 → 同样 `demux EOF inputs=0`。这是既有行为，本次不做语义改动（见 §5 未验证/边界）。
- 真机确认夹紧生效并留痕：`vdec-play: seek clamped requested=24.166 target=10.523 duration=12.023`、`audio: seek clamped requested=24.166 target=11.667 duration=12.167`（12 s 短片，C 路强制）。**应用侧 `app: last-time clamp` 分支尚未在真机命中**（拿不到 ≥ 片长−2 s 的历史续播点，见 §5）。
- 音频路径：`backend=ps5` 起播、跨 35 s 暂停恢复、两次 seek、切换音轨（重建 demux/decode）、运行期失败自动回退 mpv `ao=sdl`、强制回退分支全部取证；`underrun=0`、`agc health` 全零、无 `FALLBACK_A`。

## 2. 改动

| 文件 | 改动 |
|---|---|
| `scripts/ps5/native/native_vdec_play.c` | `media_duration_us` 由 worker 在 `play_media_open()` 里原子发布（`demux ready` 增打 `duration=`）；`wiliwili_vdec_play_seek()` 用 `duration − VDEC_PLAY_SEEK_TAIL_MS(1.5 s)` 夹紧并打 `seek clamped`；`play_restart_after_seek()` 用 worker 自己的 `VdecPlayMedia` 再夹一次（覆盖 `start_seconds`、recreate resume、时钟跳变写入者）；会话重置时清掉过期时长，避免用上一支片长夹新片。 |
| `scripts/ps5/native/native_audio.c` | `wiliwili_audio_seek()` 与 `audio_open_input()` 用 `duration − AUDIO_SEEK_TAIL_SECONDS(0.5 s)` 夹紧并打 `audio: seek clamped` / `audio: start clamped`（越界目标的音频会直接解到 EOF、把端口停住 → 画面有声变无声）。 |
| `wiliwili/source/view/video_view.cpp` | `LAST_TIME` 分支：已知时长（`real_duration`，否则 `mpvCore->duration`）且 `p ≥ duration − 2 s` 时**丢弃**续播提示、`setLastPlayedPosition(0)`，打 `app: last-time clamp p=… duration=…`。这样 mpv 时间线与音频轨都不会被推到片尾之外。 |
| 验证钩子（本文提交） | `WILIWILI_AUDIO_FORCE_FALLBACK=1`（同步拒绝分支，可重复验证回退）；`wiliwili_audio_pause()` 状态迁移日志 `audio: pause=… blocks=… ring=… eof=…`；`audio_output_close_locked()` 的 `audio: output closed backend=…`；`WILIWILI_AUDIO_AUTOTEST_PAUSE_MS`（默认 3000，用于跨 30 s 暂停取证）。 |

夹紧语义：只在**已知时长**且目标越界时改写目标，未知时长（直播、无 duration 的容器）保持原值；正常片内 seek 完全不受影响（`seek requested target=20.000` / `target=5.000` 无 clamped 行，见 §4）。

## 3. 装置

```bash
flock -w 3600 /tmp/ps5-dev.lock -c 'bash scripts/ps5/native/test-cycle.sh PPSA99233 130 /tmp/seek/opt2.txt'
# options 示例（强制 C 路 + 直进视频）
WILIWILI_TEST_VDEC=1
WILIWILI_VDEC_PLAY=1
WILIWILI_TEST_BV=<bvid>
WILIWILI_TEST_BV_DELAY=25
```

日志：`wiliwili_boot_log` 同时落 `/download0/wiliwili-boot.log` 与 UDP 9999，`python3 scripts/ps5/native/log-listen.py 9999` 实时收（`app-log.sh` 要整盘下载 download0，几分钟量级，通常不需要）。
样本：`BV1kK4y167e6`（12.0 s，480×640 HEVC，DASH，3 个音频镜像）、`BV1MSHY6eEq9`（94.9 s，852×480 HEVC，DASH）。

## 4. 证据（真机）

### 4.1 C 路 / 音频夹紧（build `Oct 22 2026 16:53:19`，`BV1kK4y167e6`）

片尾停帧时连按 R1（`seekRelative`），请求目标 24.166 s：

```
[17:15:07] vdec-play: seek clamped requested=24.166 target=10.523 duration=12.023
[17:15:07] vdec-play: EOF parking released action=seek
[17:15:07] audio: seek clamped requested=24.166 target=11.667 duration=12.167
[17:15:07] audio: seek target=11.667 gen=1
[17:15:07] vdec-play: seek reset target=10.523 rc=0 reopen=1
[17:15:07] vdec-play: seek ready generation=1 target=10.523 target90k=947070 superseded=0; awaiting true IDR
```

- `demux ready … duration=12.023` 与音频 `demux ready … duration=12.167` 是夹紧用的同一个时长来源。
- 同一动作在 16:56 那一轮（夹紧已生效）画面从 `00:12/00:12` 回到 `00:10/00:12` 继续播放（截图 `/tmp/seek/r1-seek1.png`），会话未回退。
- **残留边界**：本轮 17:15 的同一目标落在最后一个 GOP 之后（该片尾之后没有真 IDR），C 路仍以 `demux EOF inputs=0` 收场（§5）。这是「片内但终点后无 IDR」的既有约束，与越界目标无关；应用侧已不让续播点发出这种 seek。

### 4.2 LAST_TIME 续播点（`BV1MSHY6eEq9`，94.9 s）

```
[17:35:15] vdec-play: seek requested target=85.000
[17:35:15] audio: seek target=85.000 gen=1
```

历史续播点 85 s < 92.87 s（= 时长 − 2 s），按设计**不夹紧、不丢弃**，正常恢复；同一日志里 `audio: clock=85.074 mpv=85.334` 起播后 4 s 内 drift 收敛到 −44.8 ms。

### 4.3 音频：seek / 暂停恢复 / 切音轨（`WILIWILI_AUDIO_AUTOTEST=1` + `PAUSE_MS=35000`）

```
[17:35:15] audio: output backend=ps5 handle=536870912
[17:35:15] audio: demux ready codec=aac rate=44100 ch=2 tb=0.000023 duration=94.900 streams=1
[17:35:21] audio-test: seek target=20
[17:35:21] vdec-play: seek requested target=20.000        ← 片内 seek 未夹紧
[17:35:21] audio: seek target=20.000 gen=2
[17:35:22] audio: sync release drift_ms=-74.7
[17:35:22] audio: clock=20.454 mpv=20.804 drift_ms=-349.5
[17:35:22] audio: sync drift_ms=-35.0 correction=-0.0025 speed=0.9975
[17:35:24] audio: clock=22.456 mpv=22.221 drift_ms=235.0 (同步修正 speed=1.0774)
[17:35:25] audio-test: pause
[17:35:25] audio: pause=1 blocks=433 ring=4 eof=0
[17:36:00] audio-test: resume                      ← 暂停 35 s
[17:36:00] audio: pause=0 blocks=434 ring=4 eof=0
[17:36:00] audio: clock=24.154 mpv=23.994 drift_ms=160.3
[17:36:00] audio: sync drift_ms=64.1 correction=0.0100 speed=1.0100
[17:36:02] audio: clock=26.155 mpv=26.109 drift_ms=46.5
[17:36:03] audio: sync drift_ms=41.0 correction=0.0122 speed=1.0122
[17:36:04] audio-test: seek target=5
[17:36:04] vdec-play: seek requested target=5.000         ← 片内 seek 未夹紧
[17:36:04] audio: seek target=5.000 gen=3
[17:36:05] audio: clock=5.070 mpv=5.260 drift_ms=-190.5
[17:36:07] audio: clock=7.071 mpv=7.220 drift_ms=-149.3 (correction=-0.0470)
[17:36:08] audio-test: switch-track 1/3
[17:36:08] audio: track switch 0 -> 1 at 8.733
[17:36:08] audio: demux ready codec=aac rate=44100 ch=2 tb=0.000023 duration=94.900 streams=1
```

- `underrun=0`、`blocks` 单调增长；`agc health` 全零（`dcb_full/ring_fail/tex_fail/timeouts/vo_rc`），无 `FALLBACK_A`。
- 稳态 `|drift|`：−44.8 / −35.0 / 46.5 / 41.0 ms（< 100 ms）。
- **瞬态 >100 ms**：每次 seek/暂停恢复后的 1–2 s 内（−349.5、+235.0、−190.5、恢复后 160.3）——seek 后要重开 demux，属 `notes/11` §5 记录的取流期；恢复后的 160.3 ms 是**暂停前未收敛的 offset** 被冻结带入，2 s 内被修正回 < 100 ms。切轨后音频从 8.733 s 重建，时间线对齐。
- 不确定项：`audio: summary` 行落在这轮 130 s 监听窗口之外，未取到；`switch-track` 切的是 DASH 的 3 个音频**镜像 URL**（B 站多音轨视频未覆盖）。

### 4.4 回退 mpv 音频

**运行期失败（真实发生）**，`BV1MSHY6eEq9`：

```
[17:28:35] audio: open-input failed rc=-1414092869 url=https://xy1x58x218x156xy.mcdn.bilivideo.cn:8082/…
[17:28:35] audio: failed reason=open rc=-1
[17:28:35] audio: output closed backend=1 blocks=0      ← 本轮为验证补的关端口日志
[17:28:35] audio: FALLBACK_MPV_AO reason=runtime-failure
[17:28:36] mpv: audio aid=1 codec=aac … ao=sdl
[17:28:36] mpv: audio active aid=1(rc=0) tracks=2(rc=0) … ao=sdl
```

视频继续播（`vdec-play: presented` 正常增长），声音交回 mpv。**注意**：首个音频 URL 是 `mcdn:8082` 镜像，打不开时应用音频路径不做镜像重试，直接用 mpv 回退（本次未改，属既有设计）。

**强制同步拒绝分支**（`WILIWILI_AUDIO_FORCE_FALLBACK=1`，`BV1kK4y167e6`）：

```
[17:32:27] audio: start rejected by WILIWILI_AUDIO_FORCE_FALLBACK
[17:32:27] audio: FALLBACK_MPV_AO reason=start-rejected
[17:32:27] mpv: audio active aid=1(rc=0) tracks=2(rc=0) codec=aac … ao=sdl
[17:32:27] vdec-play: first-presented elapsed_ms=306
[17:32:33] vdec-play: presented=360 pts90k=533970 …
```

两条回退都满足「播放不中断」。

## 5. 未验证 / 边界（不要当作已修复）

1. **`app: last-time clamp` 真机未命中**。两次尝试都被 B 站历史「已完成」规则挡住：12 s 短片播到片尾再退出，重开时 `last_play_time=0`（`audio: start … start=0.000`、无 `LAST_TIME`）；95 s 片重开时拿到的是 85 s（未被夹紧，符合设计）。要命中需要「在片子最后 2 s 内退出」的历史点，器件窗口极窄，本轮未做到。代码分支已在，但**无真机日志**。
2. **片内但落在最后一个 GOP 的 seek 仍会 `demux EOF → FALLBACK_A`**（12 s 片 seek 到 10.523 s 时复现；该片片尾后无真 IDR）。曾尝试「失败后 AVSEEK_FLAG_BACKWARD 重试到前一个关键帧」的原型：能避免回退，但 C 路时间线落在关键帧（10.0 s）而 mpv 已停在片尾（12.0 s），画面冻结在 1 帧 —— 比回退更糟，已回滚（不在本次提交）。
3. 播完「无残留声音」只有间接证据：EOF 时 `mpv_core` 调 `wiliwili_audio_pause(1)`（本轮补了 `audio: pause=1` 日志，但对应那轮的日志抓取在 17:24 之后未再收到数据，未取到该行）；退出播放器的 `audio: summary` / `audio: output closed` 同样未抓到。**主观听感与退出静音未验证。**
4. 多音轨（真正 2 条不同语言的音轨）未覆盖；只验证了 DASH 的 3 个镜像 URL 之间的切换。
5. 跨关键帧的前进/后退各 1 次（20 s、5 s）已验证；未做连续长序列 seek（`WILIWILI_VDEC_STRESS` 路径）与 10 分钟长测。
6. 已验收行为未回归：C 路码流矩阵本轮未重跑（改动只在 seek 目标与时长发布，`demux ready` 多一个 `duration=` 字段）；`agc health`、`FALLBACK_A=0`、EOF 停帧、HUD/全屏路径在上述运行中保持。
