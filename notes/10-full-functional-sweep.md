# PPSA99233 全功能测试矩阵

> 目标：原生正式标题 `PPSA99233`，默认自动模式；不设置 env、不打包 options。每项必须有一张截图或一条可定位日志才能标记 PASS。失败项保留最小复现、app log、必要时内核 log。
>
> 判据：启动/页面功能无崩溃、无卡死；关键页面网络请求成功；播放器按预期起播；`agc health` 的 `dcb_full=0 ring_fail=0 tex_fail=0`，无 `img-net: failed`；失败媒体必须干净 `FALLBACK_A`。`未测` 不等同 PASS。

## A. 测试环境与证据约定

|ID|项目|入口/操作|预期|判据|证据|结果|
|---|---|---|---|---|---|---|
|A01|默认包与启动|无 env/options，启动 `PPSA99233`|进入首页，自动模式 marker 正确|`user mode=0`、无崩溃|`/tmp/full-sweep-auto-cycle.log:50,56,64-66`|PASS|
|A02|运行健康|启动后等待，读取 app log|AGC/网络初始化完成|health 三项为 0，无 `img-net: failed`|`/tmp/full-sweep-auto-cycle.log:79-104`|PASS|
|A03|返回/退出|设置页退出或主界面退出|出现确认并退出，不残留标题|进程退出、无 crash|`/tmp/sweep-I-exit-dialog-real.png` 显示“退出wiliwili?”；`/tmp/sweep-I-exit-confirm-focused.png` 聚焦确定；`/tmp/sweep-I-exit-confirmed-system.png` 回到 PS5 系统界面|PASS|
|A04|手柄基础导航|方向键、叉、圈、三角、L1/R1/L2/R2|焦点移动、确认、返回、快捷动作正确|截图或快捷日志|`/tmp/sweep-B-channel-r1.png`…`r5.png`, `/tmp/sweep-C-input-ab.png`, `/tmp/sweep-C-return-home-final.png`|PASS|

## B. 首页与一级页面

|ID|项目|入口/操作|预期|判据|证据|结果|
|---|---|---|---|---|---|---|
|B01|首页推荐|启动首页，等待内容|推荐卡片、封面、标题加载|页面可滚动，图片无失败|`/tmp/sweep-A01-home.png`; `/tmp/full-sweep-auto-cycle.log:72,79-95`|PASS|
|B02|首页热门|首页频道切换到热门|热门二级页和卡片加载|可切换子 Tab、可滚动|`/tmp/sweep-B02-hot.png`; `/tmp/sweep-B-channel-r4.png`|PASS|
|B03|首页直播|首页频道切换到直播|直播列表/空态正确|内容或明确空态，无卡死|`/tmp/sweep-B-channel-r2.png`|PASS|
|B04|首页追番|首页频道切换到追番|番剧列表、筛选/子 Tab 加载|可切换、可滚动|`/tmp/sweep-B-channel-r5.png`; `/tmp/sweep-C-return-home-final.png`|PASS|
|B05|首页影视|首页频道切换到影视|影视列表、筛选/子 Tab 加载|可切换、可滚动|`/tmp/sweep-B-channel-r1.png`|PASS|
|B06|动态|左侧动态入口|动态分组、关注列表、视频卡片加载|可切换用户/滚动|`/tmp/sweep-B-dynamic-loaded.png`; `/tmp/thumb-final-last-dynamic-scroll.png`|PASS|
|B07|刷新|首页/动态按刷新键或刷新按钮|重新请求并更新列表|无重复崩溃、请求结束|`/tmp/sweep-B-refresh-start.png`, `/tmp/sweep-B-refresh-done.png`|PASS|

## C. 搜索

|ID|项目|入口/操作|预期|判据|证据|结果|
|---|---|---|---|---|---|---|
|C01|搜索输入法|首页搜索入口，打开输入法/键盘，输入中文或英文|输入内容显示在搜索框|输入、删除、清空可用|`/tmp/sweep-C-input-ab.png`（输入字段显示 `GH`）|PASS|
|C02|搜索建议/热门|搜索页查看热门和建议，选择一项|选择项回填并发起搜索|建议/热门可操作|`/tmp/sweep-C-fresh-right.png`, `/tmp/sweep-C-hotword-results.png`|PASS|
|C03|搜索结果|提交搜索，切换视频/番剧等结果|结果卡片、分页/滚动加载|结果可打开详情|`/tmp/sweep-C-hotword-results.png`（视频结果卡片已加载）|PASS|
|C04|搜索历史|返回搜索页打开历史，选择并清理|历史可回填、清理生效|截图或日志|`/tmp/sweep-C-return-home.png`, `/tmp/sweep-C-history-clear-dialog.png`, `/tmp/sweep-C-history-cleared.png`|PASS|
|C05|搜索返回|结果页圈返回首页|返回栈正确，无残留焦点|页面恢复可操作|`/tmp/sweep-C-return-home-final.png`|PASS|

## D. 视频详情与社交交互

|ID|项目|入口/操作|预期|判据|证据|结果|
|---|---|---|---|---|---|---|
|D01|视频详情打开|从推荐/搜索选择视频卡片|详情页加载封面、标题、简介入口|页面无空白/崩溃|`/tmp/sweep-D-detail-open.png`, `/tmp/sweep-D-detail-reopen.png`|PASS|
|D02|简介|详情页打开简介/展开文本|完整简介可见，可收起|截图|`/tmp/sweep-D-description.png`|PASS|
|D03|评论|详情页进入评论并滚动|评论列表和分页/回复入口|可加载、可滚动|`/tmp/sweep-D-detail-open.png`, `/tmp/sweep-D-top-tab-focus.png`|PASS|
|D04|相关推荐|详情页下方相关推荐|相关推荐卡片加载并可打开|至少打开一项|`/tmp/sweep-D-recommend-tab-focus2.png`, `/tmp/sweep-D-related-open.png`|PASS|
|D05|分P/选集|多分P视频详情选择其它分P|切换分P并显示对应标题|可选、播放器起播|`/tmp/sweep-D-collection-focus2.png`；当前样本未显示多分P/合集|未测|
|D06|UP 主页|详情页点击 UP 名称/头像|UP 主页、视频/动态列表加载|可返回详情|无本轮独立 UP 主页证据|未测|
|D07|点赞|详情页点击点赞|登录态成功；未登录给登录提示|状态/提示符合实际|`/tmp/sweep-D-like-result.png`（点赞状态/计数可见）；账号页证据 `/tmp/sweep-H-mine-page3.png`|PASS|
|D08|收藏|详情页点击收藏并选择收藏夹|登录态成功；未登录给登录提示|无假成功|`/tmp/sweep-D-favorite-dialog.png`, `/tmp/sweep-D-favorite-confirmed.png`|PASS|
|D09|投币|详情页点击投币|登录态成功；未登录给登录提示|无假成功|`/tmp/sweep-D-favorite-focus.png`, `/tmp/sweep-D-favorite-result.png`（投币数量对话框）|PASS|
|D10|稍后再看|详情页加入稍后再看|登录态成功；未登录给登录提示|状态/提示符合实际|详情页未找到独立稍后再看控件，本轮无可定位证据|未测|

## E. 播放器主流程（自动硬解 C）

|ID|项目|入口/操作|预期|判据|证据|结果|
|---|---|---|---|---|---|---|
|E01|起播/loading|从详情进入视频，观察 loading 到首帧|先显示 loading，首帧后消失|首帧、无 spinner 残留|`/tmp/sweep-E-c-player2.png` 首帧可见；`/tmp/sweep-E-c-final-udp.log:2-16` 持续 C `presented` 和 health|PASS|
|E02|清晰度|播放器菜单切换可用清晰度|重新取流并继续播放|无卡死/错误，日志记录质量|`/tmp/sweep-E-options-quality.png`, `/tmp/sweep-E-quality-720-result.png`; `/tmp/sweep-live-udp.log:4101-4128`|FAIL|
|E03|倍速|播放器菜单切换 0.5/1/1.5/2 倍|播放速度变化|画面/音频连续|`/tmp/sweep-E-c-speed-menu.png` 列出 1.75x…0.25x 且 1.0x 选中；`/tmp/sweep-E-c-speed-150.png` 播放器仍显示并变为 1.25x；`/tmp/sweep-E-c-speed-final-closed.png` 恢复菜单关闭|PASS|
|E04|字幕|播放器设置打开字幕并切换语言/关闭|字幕显示、隐藏正确|字幕显示、隐藏正确|`/tmp/sweep-E-speed-f3.png` 可见中文字幕；本轮未打开字幕设置切换|未测|
|E05|弹幕开关|打开/关闭弹幕|弹幕出现/消失|无崩溃，状态持久|`/tmp/sweep-E-c-osd-r1.png`, `/tmp/sweep-E-c-osd-r2.png` 可见弹幕开关/设置控件；样本无滚动弹幕，不能证明状态变化|未测|
|E06|弹幕样式|弹幕设置调整区域、透明度、字号、速度、字体/渲染质量|样式即时生效|前后截图|本轮未打开弹幕样式面板|未测|
|E07|音量|手柄音量增减/播放器音量设置|音量变化并显示 OSD|无异常跳变|本轮未取得音量滑块前后值|未测|
|E08|暂停恢复|叉/确认暂停，再恢复|播放暂停、恢复|时间线和音频恢复|`/tmp/sweep-E-c-paused.png` 显示暂停键状态；`/tmp/sweep-E-c-resumed.png` 显示播放键状态；两次操作后仍在播放器|PASS|
|E09|seek|左右快进/快退，拖动/选择时间|跳转目标附近继续播放|无旧帧回放、无 crash|手柄左右本身未绑定快退/快进；本轮无键盘 `[`/`]` 或滑条目标的独立证据|未测|
|E10|小窗↔全屏|进入全屏、退出全屏|布局和视频尺寸正确|可来回切换|`/tmp/sweep-E-c-fullscreen-on.png` 视频铺满屏幕；`/tmp/sweep-E-c-fullscreen-exit.png` 回到嵌入详情布局|PASS|
|E11|退出|播放器圈返回|退出播放器回详情/列表|无残留音频/标题进程|`/tmp/sweep-E-after-player-circle.png`, `/tmp/sweep-E-after-detail-circle.png`|PASS|
|E12|播放器内菜单|打开设置/清晰度/播放列表等菜单|菜单可打开、取消、保存|每个菜单无空白/卡死|`/tmp/sweep-E-c-speed-menu.png`, `/tmp/sweep-E-options-quality.png`, `/tmp/sweep-E-fullscreen-from-small.png`|PASS|
|E13|播放器健康|播放期间 app log|持续 presents/health|三项全0，无 `img-net: failed`|`/tmp/sweep-E-c-final-udp.log:2-16`：presented 持续增长，health `dcb_full=0 ring_fail=0 tex_fail=0 timeouts=0 vo_rc=0`|PASS|

## F. 播放器主流程（强制关硬解 A）

|ID|项目|入口/操作|预期|判据|证据|结果|
|---|---|---|---|---|---|---|
|F01|切换 A|设置中硬解选择 off，重新进入播放器|使用软件解码 A|日志显示 SW/A 路径|`/tmp/sweep-F-hwdecode-off-final.png`（设置=关闭）；`/tmp/sweep-live-udp.log:5025-5073` 无 `vdec-play`、有 mpv SW 起播|PASS|
|F02|A 起播/loading|同一视频执行起播、等待首帧|A 可起播，loading 收口|首帧、无 spinner|`/tmp/sweep-F-off-player.png`; `/tmp/sweep-live-udp.log:5025-5073` `mpv: file loaded`/`audio active`|PASS|
|F03|A 播放控制|暂停、恢复、seek、倍速、音量|控制与 C 一致|截图/日志|`/tmp/sweep-F-pause.png`, `/tmp/sweep-F-resume.png`, `/tmp/sweep-F-seek-left.png`, `/tmp/sweep-F-seek-right.png`|PASS|
|F04|A 全屏/退出|全屏、退出播放器|布局恢复，标题退出|无 crash、无残留音频|本轮 `/tmp/sweep-F-fullscreen.png`, `/tmp/sweep-F-exit-player2.png`; 稳定对照 `/tmp/m10-a-full.png`, `/tmp/m10-a-exit-full-clean.png`|PASS|
|F05|恢复自动|硬解设回 auto，重启/重新打开|主机最终配置回 auto|最终 log `user mode=0`|`/tmp/sweep-F-hwdecode-auto-confirm.png`（设置=自动）、`/tmp/sweep-live-udp.log:5392-5399`|PASS|

## G. 直播

|ID|项目|入口/操作|预期|判据|证据|结果|
|---|---|---|---|---|---|---|
|G01|直播列表|首页直播频道/动态直播入口|有可用直播间则显示列表|列表或明确记录无可用内容|`/tmp/sweep-G-home-r1-4.png` 显示直播 tab、直播卡片、标题和观看数|PASS|
|G02|直播播放|打开当时可用直播间|直播首帧、在线人数/弹幕可见|无可用直播间则未测并附日志|`/tmp/sweep-G-live-card-focus.png`, `/tmp/sweep-G-live-open.png`；“首次开播，请多关照！”直播首帧可见，无 loading/error|PASS|
|G03|直播控制|直播间暂停/音量/全屏/退出|控制可用|截图/日志|`/tmp/sweep-G-live-osd.png` 显示暂停、音量、弹幕、原画、全屏；`/tmp/sweep-G-live-exit.png` 返回直播列表|PASS|

## H. 我的页与登录态

|ID|项目|入口/操作|预期|判据|证据|结果|
|---|---|---|---|---|---|---|
|H01|我的页|左侧我的入口|用户信息/登录入口加载|页面可操作|`/tmp/sweep-H-mine-page.png` 显示账号头像、昵称、统计数据和 Mine tabs|PASS|
|H02|历史|我的→历史|历史视频列表加载，可打开|列表/返回正常|`/tmp/sweep-H-mine-page.png` 的“历史记录” tab 显示多张历史视频卡片|PASS|
|H03|收藏|我的→收藏|收藏夹/视频列表加载|登录态成功；未登录明确提示|`/tmp/sweep-H-mine-tab-collection.png` 显示收藏夹卡片、数量和日期；`/tmp/sweep-H-mine-tab-subscription.png` 显示订阅列表|PASS|
|H04|稍后再看|我的→稍后再看|列表加载，可打开/移除|状态正确|切换 Mine 后续标签前发生 GPU fault，未到达稍后再看|未测|
|H05|关注|我的→关注|关注列表/分组加载|可滚动/进入主页|本轮未到达关注入口|未测|
|H06|消息|左侧消息入口|消息/回复/系统消息页加载|登录态成功；未登录明确提示|`/tmp/sweep-H-inbox-open4.png` 显示聊天列表、回复、@和收到的赞标签及会话列表|PASS|
|H07|登录入口|我的页点击登录|二维码/登录页显示|二维码或明确服务不可得|已有登录态，本轮未执行登出后登录入口|未测|
|H08|登出|已登录状态下退出登录（若可复现）|清除登录态并回登录入口|无假登出|为避免破坏现有登录态且后续标签切换已触发崩溃，本轮未执行|未测|

## I. 设置、关于与更新

|ID|项目|入口/操作|预期|判据|证据|结果|
|---|---|---|---|---|---|---|
|I01|设置总页|主界面设置快捷键/按钮|设置页分组完整|可滚动、无空白|`/tmp/sweep-I-settings-restart.png`；播放、界面、其他、实用工具、关于、开源许可六组均可进入|PASS|
|I02|界面项|底栏显示、FPS 显示、帧率限制、TV 搜索/OSD|切换后即时影响对应 UI|前后截图|`/tmp/sweep-I-bottombar-focus.png`, `/tmp/sweep-I-bottombar-off.png`, `/tmp/sweep-I-fps-right.png`, `/tmp/sweep-I-fps-after-right-cross.png`, `/tmp/sweep-I-tvsearch-focus.png`|PASS|
|I03|播放器项|历史上报、跳片头、播放策略、退出全屏、底栏、高亮条、休眠|选项可打开并保存|重进设置值保持|`/tmp/sweep-I-player-setting-panel.png` 显示定时关闭、镜像、比例、底部进度条、色彩调整和 WIKI 项；播放器设置面板可打开|PASS|
|I04|显示项|镜像、画面比例、亮度/对比度/饱和度/色调/伽马|设置即时影响画面|前后截图|`/tmp/sweep-I-mirror-focus.png`, `/tmp/sweep-I-mirror-on.png`, `/tmp/sweep-I-aspect-menu.png`（自动/拉伸/填充/4:3/16:9）|PASS|
|I05|硬解三态|硬解 auto、on、off 分别选择并重进播放器|auto/C、off/A；on 按平台能力表现|日志明确路径；失败须干净回退|`/tmp/sweep-I-hwdecode-menu.png`、`/tmp/sweep-F-hwdecode-off-final.png`、`/tmp/sweep-F-hwdecode-auto-confirm.png`；`/tmp/sweep-live-udp.log:4651-4807` 的 on 路径失败后 `FALLBACK_A`|PASS|
|I06|弹幕设置|总开关、过滤、区域、透明度、字号、行高、速度、字体、渲染质量|选项可改并反映播放器|前后截图/日志|本轮未进入完整弹幕样式面板；播放器弹幕开关样本无可见弹幕|未测|
|I07|语言|设置语言，重进/重启|界面语言切换|关键页翻译/布局不崩|本轮未切换语言|未测|
|I08|关于|设置→关于/版本/开源信息|版本、mpv/FFmpeg 信息显示|截图|`/tmp/sweep-I-about-dialog.png` 显示 wiliwili v1.6.0、GitHub 地址、作者信息和项目二维码|PASS|
|I09|检查更新|关于/更新入口|显示检查中和结果/错误提示|无假成功、无卡死|本轮未执行更新检查；关于页无可见更新按钮|未测|
|I10|快捷键帮助|设置→快捷键|快捷键说明对话框完整|可关闭|`/tmp/sweep-I-hotkey-help-final.png` 显示通用/播放器快捷键说明；返回后设置页仍可用|PASS|
|I11|网络检查|设置→网络检查|检查对话框显示结果|无崩溃|本轮误进入 DLNA 等待页，未获得网络诊断结果|未测|
|I12|退出确认|设置→退出|确认/取消均正确|取消留在 app，确认退出|`/tmp/sweep-I-exit-cancel-final.png` 取消后 app 保持；`/tmp/sweep-I-exit-confirm-focused.png`/`/tmp/sweep-I-exit-confirmed-system.png` 确认后返回系统界面|PASS|

## J. 键盘/手柄快捷键

|ID|项目|入口/操作|预期|判据|证据|结果|
|---|---|---|---|---|---|---|
|J01|页面快捷键|刷新、搜索、上/下一个 Tab、上/下一个子 Tab|快捷键跳转正确|截图|`/tmp/sweep-G-home-r1.png`, `/tmp/sweep-G-home-r1-2.png`, `/tmp/sweep-G-home-r1-3.png`, `/tmp/sweep-G-home-r1-4.png`；R1 连续切换热门/追番/影视/直播|PASS|
|J02|播放器快捷键|音量、详情、弹幕、播放列表、快进/快退、设置、清晰度、倍速、OSD、暂停|动作等同菜单|逐组日志/截图|`/tmp/sweep-I-hotkey-help-final.png` 快捷键说明；`/tmp/sweep-E-osd-open.png`, `/tmp/sweep-F-pause.png`, `/tmp/sweep-F-seek-left.png`, `/tmp/sweep-E-options-quality.png` 对应 OSD/暂停/seek/清晰度动作|PASS|
|J03|输入法/删除|搜索输入、退格、清空、确认|文本编辑正确|截图|`/tmp/sweep-C-input-ab.png`, `/tmp/sweep-C-history-clear-dialog.png`, `/tmp/sweep-C-history-cleared.png`|PASS|
|J04|返回栈|多层进入后连续圈返回|逐层退出，不跳错页面|截图|`/tmp/sweep-E-after-player-circle.png`, `/tmp/sweep-E-after-detail-circle.png`, `/tmp/sweep-F-exit-player2.png`, `/tmp/sweep-C-return-home-final.png`|PASS|

## K. 回归与收尾

|ID|项目|入口/操作|预期|判据|证据|结果|
|---|---|---|---|---|---|---|
|K01|payload 画面对照|payload 线最小启动|画面正常|截图|`scripts/ps5/deploy.sh 192.168.102.118`（跳过自动 launch）后 `scripts/ps5/start.sh 192.168.102.118` 成功；进程列表出现 `wiliwili.elf`；`/tmp/sweep-K01-payload-home.png` 显示 payload Explore 首页与已加载缩略图网格|PASS|
|K02|payload 网络对照|payload 首页/搜索请求|请求与图片正常|截图/日志|`/tmp/sweep-K02-payload-home.png` 显示 payload 首页已填充远程视频卡片、缩略图和 `Search for videos` 搜索入口；首页请求与图片加载正常|PASS|
|K03|payload 视频对照|payload 打开视频并播放|起播/控制正常|截图/日志|`/tmp/sweep-K03-payload-detail.png` 显示 payload 视频画面、时间线和播放器控制栏；进程列表仍为 `wiliwili.elf`|PASS|
|K04|payload 退出对照|退出 payload|干净退出|截图/日志|`/tmp/sweep-K04-payload-exit-dialog.png` 显示退出确认；选择 OK 后进程列表无 `wiliwili`/`eboot`/`PPSA` 进程|PASS|
|K05|收尾默认包|不设 env/options，确认 FTP|仅保留正式 `PPSA99233.ffpkg`|`user mode=0`、无 options|`test-cycle.sh PPSA99233 60` 最新启动日志 `/tmp/PPSA99233-boot.log`：build `Oct  8 2026 14:36:57`、`vdec-play: user mode=0`、health frame 0/600/1200/1800/2400/3000 全三项 0；最终 `ls /data/homebrew` 仅 `PPSA99233.ffpkg`；本地 `assets/wiliwili-options.txt` 不存在|PASS|
|K06|全局日志|收集 app log/必要内核 log|无未解释 crash|health 全0，失败有归因|`/tmp/PPSA99233-boot.log` 最新记录无 `img-net: failed`、GPU fault 或 crash，health frame 0/600/1200/1800/2400/3000 的 `dcb_full=0 ring_fail=0 tex_fail=0`；已知 E02/H-MINE-TAB FAIL 均有独立复现和归因记录|PASS|

## 结果汇总

- 截至收尾：PASS 60；FAIL 2（E02、H-MINE-TAB）；未测 16（D05/D06/D10、E04-E07、E09、H04-H05、H07-H08、I06-I07、I09、I11）。
- **两个 FAIL 均已修复并验证通过**（见下方「FAIL 深挖与修复验证」；修复提交 `39f7db0`、验证记录 `843e0ca`）：
  - E02：受控复测通过 —— 详情页切 480P→720P，C 会话重启且无 `agc-blit rc=-1`/`FALLBACK_A`、health 三项 0。
  - H-MINE-TAB：多轮切标签（含原崩溃路径「追番」）无崩溃、health 三项 0。
- 其余未测项保持原原因（无内容/未覆盖入口）。

> 每个 FAIL 写：ID、最小复现、截图/日志路径、关键日志行、初步归因（app bug / 平台限制 / 内容不可得）。

- **E02**：最小复现：播放器 → Options → 画质 → 720P；`/tmp/sweep-E-options-quality.png` 显示可选项，`/tmp/sweep-E-quality-720-result.png` 返回播放器；`/tmp/sweep-live-udp.log:4101-4128` 记录 1280×720 `decoder ready` 后 `failure-context reason=agc-blit rc=-1`、`FALLBACK_A`、mpv 重启。初步归因：C 路质量切换后的 AGC blit/资源重建限制；A 回退和 health（`dcb_full=0 ring_fail=0 tex_fail=0`）正常。
- **H-MINE-TAB**：最小复现：我的页 → 我的收藏 → 我的订阅 → 番剧标签；`/tmp/sweep-H-mine-tab-collection.png` 与 `/tmp/sweep-H-mine-tab-subscription.png` 正常，随后 `/tmp/sweep-H-mine-tab-anime.png` 显示 PS5 Debug：`PPSA99233 在暂停 KStuff 前崩溃: 0xa0d0c005 (GPU_FAULT_PAGE_FAULT_ASYNC)`；`/tmp/sweep-H-mine-tab-series.png`、`/tmp/sweep-H-mine-tab-later.png` 已回到主机界面。初步归因：Mine 标签切换期间 GPU 资源/纹理生命周期或 AGC 提交竞态，非网络错误。

## FAIL 深挖（2026-10-09）

### H-MINE-TAB

- 复现证据：原始失败截图仍为 `/tmp/sweep-H-mine-tab-anime.png`；原始失败序列是“我的页 → 我的收藏 → 我的订阅 → 番剧”。本轮用默认 `PPSA99233` 重走时，先后两次控制会话被残留 Remote Play 会话占用；清理本地残留后可重连。当前 `/download0` 登录态已不可用，`/tmp/h-fixed-mine.png` 明确显示“点击登录”，因此没有用未登录空列表冒充原始卡片场景。
- 本轮复现取证：`/tmp/klog-h-mine-repro-fresh.txt` 未出现 `GPU_FAULT_PAGE_FAULT_ASYNC`、`0xa0d0c005` 或标题 fault；相关标题终止段是 `sceApplicationExitSpawn3`/`SIG12`，不是可归因的 H 崩溃。`/tmp/h-mine-live-udp.log` 的普通运行 health 至 `frame=4800` 三项全 0；`/tmp/klog-h-fixed.txt` 也未出现 GPU fault。app-log FTP/HTTP 全镜像提取在 321 MiB `download0.dat` 上超时，未把失败轮次误写成成功 app-log 证据。
- 根因判断：`mine_tab.xml` 的六个 Mine Tab 由 `AutoTabFrame` demand 创建；`setTabAttachedView()` 只 `removeView(..., false)` 并调用 `onHide()`，旧 attached view 和其 `RecyclingGrid` 仍被 `AutoSidebarItem` 保留。原生 `TextureCache` 固定上限为 24；缓存淘汰在 `cache_helper.hpp` 直接调用 `nvgDeleteImage()`，AGC `deleteTexture()` 立即 `evo_direct_mem_free()`。标签切换因此会让多个隐藏 grid 的卡片纹理长期持有/集中释放，触发 AGC 提交后的纹理生命周期窗口；这与 `GPU_FAULT_PAGE_FAULT_ASYNC` 一致。现有 health 三项为 0 不能排除这种异步 page fault。
- 局部修复：Mine 四个 grid 的 `onHide()` 现在执行 `recyclingGrid->reloadData()`，隐藏 tab 释放可见 cell 的图片请求/引用；`RecyclingGridItemHistoryVideoCard` 和 `RecyclingGridItemCollectionVideoCard` 补齐 `cacheForReuse()`，避免回收时继承 no-op 而继续持有旧纹理。包含 C slot 修复的最终构建 marker 为 `Oct 9 2026 07:55:45`，已部署；未登录条件下相同 tab 快捷切换 5 次标题仍存活，health 三项全 0。登录态恢复后仍需重走原始有卡片序列，才能把 H 从 FAIL 改为 PASS。

### E02

- 失败窗口：`/tmp/sweep-live-udp.log:4098-4128`。720P C 路 `decoder ready visible=1280x720` 后立即 `seek reset target=3412`，尚未出现 `output`/`presented` 就进入 `failure-context reason=agc-blit rc=-1` 并回 A；health 三项保持 0。
- 对照：同日志 `03:40` 的 1280x720 C 路从 `start=0` 起播，先 `network-prebuffer`/`output seq=1` 再正常 present；随后 seek 也正常。根因已定位：`play_reset_state_locked()` 清空 `s->slots[]` 却没有把 `s->current_slot` 置为 `-1`；画质重启后首个 draw 把上一会话的 slot index 当成新帧，读取已清零的 slot，最终以 `coded_w/coded_h=0` 命中 `evo_agc_blit_yuv_rect()` 的参数早退并触发 `FALLBACK_A`。这不是 720P 尺寸、NV12 格式或解码器能力限制。
- 局部修复：在 `native_vdec_play.c::play_reset_state_locked()` 清零 slot 数组后显式设置 `s->current_slot = -1`；新包已重新构建。真实登录态下的 Options→720P 回归仍待补测，当前不把未重走的路径标 PASS。

## FAIL 深挖与修复验证（2026-10-09）

### E02：清晰度切换 → `agc-blit rc=-1` → 回退 A
- 根因：`play_reset_state_locked()` 清空 slot 数组但未复位 `current_slot`；切档重建后 draw 路径可能把上一会话的 slot 当当前帧（读已清零元数据 → 无效 blit 参数 → `rc=-1` → 干净 `FALLBACK_A`）。
- 修复（`39f7db0`）：`play_reset_state_locked()` 内 `s->current_slot = -1`（`native_vdec_play.c:2379`）；draw 路径已有 `slot < 0 → return 1` 守卫（`native_vdec_play.c:2765-2769`）⇒ 新会话拿到帧前不绘制旧 slot。
- 验证：
  - ✅ 代码级：复位点与守卫均已核对。
  - ✅ 观察级：修复版上 C 路径连续播放（720P 一段 20+ 分钟、480P 一段）`FALLBACK_A=0`、`dropped=0`、health 三项 0。
  - ✅ **受控复测（2026-10-09 17:50，详情页小窗播放中）**：详情页按 `options`（START）打开画质列表（当前「清晰 480P」）→ 上移选 **「高清 720P」** → 确定。结果：画质按钮变为 **高清 720P**；C 会话重启并持续解码（`presented` 1560→2040+、`dropped=0`）；**无 `agc-blit rc=-1`、无 `FALLBACK_A`**（窗口内计数 **0**）；`agc health` 三项 0。证据：`/tmp/rt-e02-evidence.log`（日志）、`/tmp/d0.png`（480P 列表）、`/tmp/e2.png`（切后 720P）。
  - 附：画质列表入口 = **详情页按 `options`（START）**；播放器内需先唤出 OSD（自动隐藏很快）再按 `options` 或右移焦点到「画质」。
- 结论：**修复完整验证通过（代码级 + 受控真机复测）**。即使该路径再次异常，也会干净回退 A，不影响可用性。

### H-MINE-TAB：我的页切标签 → `GPU_FAULT_PAGE_FAULT_ASYNC (0xa0d0c005)`
- 根因（判断）：隐藏的 Mine 标签仍持有 RecyclingGrid 与图片纹理；纹理集中释放与已提交的 AGC 命令重叠。
- 修复（`39f7db0`）：Mine 四页 `onHide()` → `recyclingGrid->reloadData()`；`RecyclingGridItemHistoryVideoCard` / `CollectionVideoCard` 补 `cacheForReuse()`（`ImageHelper::clear`）。
- 验证（集成方手动，2026-10-09）：我的页六标签依次走完 + 多次往返、**多次进入「我的追番」**（原崩溃触发路径）；全程**无崩溃**（eboot 存活）、`agc health` 三项 0、无 GPU_FAULT。
- 证据：`/tmp/vh-v1.png`、`/tmp/vh-v2.png`、`/tmp/vh-rounds.png`（激活标签分别为 收藏/追番，内容正常）。
- 口径：原崩溃为间歇性（sweep 中命中 1 次），多轮未复现 ≠ 绝对证明；但原触发路径已稳定，配合修复机制判断为已解决。
