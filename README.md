<p align="center">
    <img src="resources/svg/cn.xfangfang.wiliwili.svg" alt="logo" height="128" width="128"/>
</p>
<p align="center">
  一个专为手柄用户设计的第三方 <a href="https://www.bilibili.com">B站</a> 客户端
</p>
<p align="center">
<b><a href="#特点">特点</a></b>
|
<b><a href="#安装">安装</a></b>
|
<b><a href="#文档">文档</a></b>
|
<b><a href="#开发">开发</a></b>
</p>

- - -
![GitHub forks](https://img.shields.io/github/forks/xfangfang/wiliwili)
[![Crowdin](https://badges.crowdin.net/wiliwili/localized.svg)](https://crowdin.com/project/wiliwili)
![NS](https://img.shields.io/badge/-Nintendo%20Switch-e4000f?style=flat&logo=Nintendo%20Switch)
![PSV](https://img.shields.io/badge/-PSVita-003791?style=flat&logo=PlayStation)
![PS4](https://img.shields.io/badge/-PS4-003791?style=flat&logo=PlayStation)
![PS5](https://img.shields.io/badge/-PS5-003791?style=flat&logo=PlayStation)
![MS](https://img.shields.io/badge/-Windows%207+-357ec7?style=flat&logo=Windows)
![mac](https://img.shields.io/badge/-macOS%2010.11+-black?style=flat&logo=Apple)
![Linux](https://img.shields.io/badge/-Linux-lightgrey?style=flat&logo=Linux&logoColor=white)
[![fedora](https://img.shields.io/badge/fedora-copr-blue?logo=fedora)](https://copr.fedorainfracloud.org/coprs/mochaa/wiliwili/)
[![Scoop Version (extras bucket)](https://img.shields.io/scoop/v/wiliwili?bucket=extras)](https://scoop.sh/#/apps?q=wiliwili)
[![aur](https://img.shields.io/aur/version/wiliwili?color=blue&logo=archlinux)](https://aur.archlinux.org/packages/wiliwili/)
[![Flathub](https://img.shields.io/flathub/v/cn.xfangfang.wiliwili)](https://flathub.org/apps/cn.xfangfang.wiliwili)
[![nightly.link](https://img.shields.io/badge/nightly.link-%E6%B5%8B%E8%AF%95%E7%89%88-green)](https://nightly.link/xfangfang/wiliwili/workflows/build.yaml/dev)
[![layout](https://img.shields.io/badge/wiliwili-自定义布局-yellow)](https://github.com/xfangfang/wiliwili_theme)
<br>

# 特点

wiliwili 拥有非常接近官方PC客户端的B站浏览体验  
同时支持**触屏**、**鼠标**、**键盘** 与 **手柄**操控  
无论是电脑还是游戏掌机都能获得全新的使用体验

多语言：简、繁、日、韩、英 ...   
搜索页：热搜 视频 番剧 影视  
筛选页：快速找到想看的影视内容  
动态页：关注的UP主最近视频动态  
直播页：关注的主播与其他系统推荐  
播放页：视频 番剧 电影 纪录片 综艺，支持弹幕与评论  
个人页：扫码登录 历史记录 个人收藏 我的追番 我的追剧  
主题色：拥有深浅两色主题，跟随系统自动切换

<br>

# 安装

### Nintendo Switch

1. 下载 `wiliwili-NintendoSwitch.zip`：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)
2. 将 wiliwili.nro 放置在**内存卡** `switch` 目录下。
3. 在主页 `按住` R键打开任意游戏进入 hbmenu，在列表中选择 wiliwili 点击打开即可。
4. [可选] 在应用内安装桌面图标，入口：设置/实用工具/使用教程

<details>

<br>

桌面图标会优先尝试打开 `switch/wiliwili.nro`，如果其不存在，则尝试打开 `switch/wiliwili/wiliwili.nro`，如果这两个路径都不存在，则打开
hbmenu 自行选择路径。

默认提供的为 OpenGL 版本，最高只能播放 4k@30，你也可以下载到支持原生图形 api
的 [deko3d 版本](https://nightly.link/xfangfang/wiliwili/workflows/build.yaml/dev)，可以流畅播放 4k@60，不过可能会偶尔崩溃。

</details>

### PSVita

下载 `wiliwili-PSVita.vpk` 安装即可：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)

开启硬解后可以流畅播放 720P 横屏视频，480P 竖屏视频，部分直播 1080P 原画。

### PS4

下载 `wiliwili-PS4.pkg` 安装即可：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)

只支持软解，如果想播放 4k@60 需要在设置中开启低画质解码。

### PS5

通过 `ps5-payload-sdk` 和 `ps5-payload-websrv` 以 payload 方式运行。资源会嵌入 ELF；运行配置、OSMesa 和 CA 证书位于 `/data/homebrew/wiliwili/`。

构建、安装 OSMesa 与 CA 证书并加载的命令见下方“交叉编译 PS5 payload”。

### PC

PC客户端支持切换硬件解码、秒开流畅适合老电脑、支持鼠标操控（左键点击 右键返回 中键刷新）

下载对应系统的安装包运行即可：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)

> [!TIP]
> 现在 Linux & Steam Deck 用户可以通过系统自带的软件商店（如Discover、GNOME Software）搜索 `wiliwili` 进行下载。  
> 更多使用技巧请参考 [项目 WIKI](https://github.com/xfangfang/wiliwili/wiki)  

<br>

# 文档

在各位开发者的帮助下，wiliwili 支持了一系列包管理器，同时 wiliwili 还拥有丰富的自定义选项，包括：使用 Anime4K
提升观感，自定义字体及图标等等  
前往 [项目 WIKI](https://github.com/xfangfang/wiliwili/wiki) 查看更多使用技巧

<br>

# TODO list

如果你有其他改进的想法或创意，欢迎在讨论区交流：[Discussions](https://github.com/xfangfang/wiliwili/discussions/categories/ideas)

<details>

- [x] 初步完成底层基础组件、首页各类推荐视频、用户视频播放页
- [x] 微调页面、解决播放器启动速度慢、解决播放页面退出卡顿
- [x] 临时解决异步加载导致的空指针问题（图片异步加载某些情况还会出现问题，待修复）
- [x] 添加番剧/影视播放、添加扫码登录、播放历史、用户收藏夹（收藏夹相关部分工作不稳定）
- [x] 初步添加搜索
- [x] 播放页新增分集与UP主最新投稿
- [x] 完善视频播放页用户评论内容
- [x] 重构图片异步加载逻辑
- [x] 解决收藏夹、搜索页某些情况导致闪退的问题
- [x] 完善搜索页：番剧、影视 转为竖图
- [x] 完善播放页投稿列表：调整结构、自动加载下一页
- [x] 播放页展示合集与推荐
- [x] 添加动态页
- [x] 添加视频检索页
- [x] 完善设置页
- [x] 弹幕相关设置
- [x] 点赞、投币、收藏
- [x] 拖拽调节进度
- [x] 增加单手模式使用一个手柄来控制播放器
- [x] NSP forwarder自动检查多个位置的nro文件，避免无法打开
- [x] 增加设置使首页无法通过返回退出，避免误触
- [x] 使用教程添加未指明的快捷键说明
- [x] 重压摇杆临时快进
- [x] 支持切换按键图标
- [x] 应用内多语言切换
- [x] 重构搜索页面
- [x] 评论@显示不同颜色
- [x] 完善评论图片
- [x] 评论大表情包所在行增加行高
- [x] 支持webp图片
- [ ] 搜索支持搜索用户
- [ ] 长按一键三连
- [ ] 支持个人主页
- [ ] 评论跳转进度
- [ ] 评论跳转搜索
- [ ] 评论下方的更多信息 (up主点赞等内容)
- [ ] 投票评论
- [ ] 互动视频

</details>

<br>

# 反馈问题前要做的事

1. 网络相关的问题附加 `网络诊断截图`，入口：应用内设置/实用工具/网络诊断
2. [Switch用户] 要确保 `大气层`和`系统固件` 更新到 **最新** ，`内存卡`为 **FAT32**
3. [Switch用户] 如果打开应用黑屏时间过长，可以尝试删除内存卡目录 `config/wiliwili` 重新进入
4. 确保 `系统时间`正确、系统`网络设置`正确（主要是DNS）、如果使用了`网络代理`请在反馈前关闭并重新测试
5. 查找有没有其他人出现过类似的问题：[Issues](https://github.com/xfangfang/wiliwili/issues?q=is%3Aissue)
6. **完整且详细地** 描述你的问题，最好附加演示视频、截图。
7. 尝试复现问题，尽力找到BUG出现的规律

<br>

# 贡献

### 软件移植

本应用基于 nanovg 绘制界面，nanovg 底层可移植切换到任意图形库，已有 OpenGL/Vulkan/Metal 等支持。   
视频播放部分则使用 FFMPEG + MPV 绘制，默认使用 OpenGL，有 D3D11/Deko3d/Gxm 或软件渲染支持。  
触摸/按键/输入法等平台相关功能通过 GLFW 或 SDL 来支持，也可以脱离二者直接实现，比如 Gxm 版 PSV。

如果你要移植的设备支持 OpenGL(ES) 那么一般来说，直接编译就能正常运行。  
如果你要移植的设备使用其他底层图形库，那么首先需要移植 nanovg，这可以确保应用主要界面正常，
其次为了更好的性能表现需要 ffmpeg 的硬解和 mpv 的渲染支持。  

如果你有想要移植的设备欢迎发一条 issue 讨论，Android / iOS 不在讨论之内。

### 新功能

如果你有想完成的创意，请在开发前发布一个 issue 讨论，避免和别人的创意撞车浪费了时间

### 多语言支持

如果你想为软件添加多语言的翻译支持，或者发现了某些翻译存在问题需要订正，请查看 [#52](https://github.com/xfangfang/wiliwili/issues/52)
了解如何贡献翻译

### 代码分支

主分支 yoga 为最新版本的代码  
开发分支 dev 为正在开发中的代码，任何新的 PR 都需要向 dev 分支提交

<br>

# 开发

```shell
# 拉取代码
git clone --recursive https://github.com/xfangfang/wiliwili.git
cd wiliwili
```

### PC本地运行

目前 wiliwili 支持运行在 Linux macOS 和 Windows上

<details>

#### macOS

```shell
# macOS: install dependencies
brew install mpv webp

cmake -B build -DPLATFORM_DESKTOP=ON
make -C build wiliwili -j$(sysctl -n hw.ncpu)
```

#### Linux

不同 Linux 的编译过程或依赖可能不同，这里是一份总结：[#89](https://github.com/xfangfang/wiliwili/discussions/89)

欢迎在上面的链接中写出你所使用系统的编译过程供大家参考。

```shell
# Ubuntu: install dependencies
sudo apt install libssl-dev libmpv-dev libwebp-dev

cmake -B build -DPLATFORM_DESKTOP=ON
make -C build wiliwili -j$(nproc)
```

```shell
# 如果你想安装在系统路径，并生成一个桌面图标，请使用如下内容编译
cmake -B build -DPLATFORM_DESKTOP=ON -DINSTALL=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH=/usr
make -C build wiliwili -j$(nproc)
sudo make -C build install

# uninstall (run after install)
sudo xargs -a build/install_manifest.txt rm
```

#### Windows

```shell
# Windows: install dependencies (MSYS2 MinGW64)
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make \
  git mingw-w64-x86_64-mpv mingw-w64-x86_64-libwebp

cmake -B build -G "MinGW Makefiles" -DPLATFORM_DESKTOP=ON
mingw32-make -C build wiliwili -j$(nproc)
```


</details>

### 交叉编译 Switch 可执行文件 (wiliwili.nro)

推荐使用docker构建，本地构建配置环境略微繁琐不过可用来切换底层的ffmpeg或mpv等其他依赖库更灵活地进行调试。

<details>

以下介绍 OpenGL 下的构建方法，deko3d (更好的硬解支持)请参考：`scripts/build_switch_deko3d.sh`

#### Docker

```shell
docker run --rm -v $(pwd):/data devkitpro/devkita64:20251117 \
  bash -c "/data/scripts/build_switch.sh"
```

#### 本地编译

```shell
# 1. 安装devkitpro环境: https://github.com/devkitPro/pacman/releases

# 2. 安装依赖
sudo dkp-pacman -S switch-glfw switch-libwebp switch-cmake switch-curl devkitA64

# 3. 安装自定义依赖
# devkitpro提供的提供的 ffmpeg/mpv 无法播放网络视频
# 手动编译方法请参考：scripts/README.md
base_url="https://github.com/xfangfang/wiliwili/releases/download/v0.1.0"
sudo dkp-pacman -U \
    $base_url/switch-ffmpeg-7.1-1-any.pkg.tar.zst \
    $base_url/switch-libmpv-0.36.0-3-any.pkg.tar.zst

# 4. build
cmake -B cmake-build-switch -DPLATFORM_SWITCH=ON
make -C cmake-build-switch wiliwili.nro -j$(nproc)
```

</details>

### 交叉编译 PSV 可执行文件

使用本地环境来编译可以参考：
 - [borealis 编译指南](https://github.com/xfangfang/borealis/wiki/PS-Vita)
 - [wiliwili vita 编译指南](https://gist.github.com/xfangfang/305da139721ad4e96d7a9d9a1a550a9d)

注意: 我们使用自定义的 mbedtls, curl 和 ffmpeg 作为依赖, 在使用本地环境编译时，请先卸载 vitasdk 中的相关库，再安装[指定的](https://github.com/xfangfang/wiliwili/tree/yoga/scripts/psv)依赖.

<details>

```shell
# 构建 OpenGL ES 2.0 版
docker run --rm -v $(pwd):/src/ xfangfang/wiliwili_psv_builder:latest \
    "cmake -B cmake-build-psv -G Ninja -DPLATFORM_PSV=ON \
        -DMPV_NO_FB=ON -DUSE_SYSTEM_CURL=ON -DUSE_SYSTEM_SDL2=ON \
        -DCMAKE_BUILD_TYPE=Release && \
        cmake --build cmake-build-psv"

# 构建 Gxm 版 (推荐)
docker run --rm -v $(pwd):/src/ xfangfang/wiliwili_psv_builder:latest-gxm \
    "cmake -B cmake-build-psv -G Ninja -DPLATFORM_PSV=ON \
        -DUSE_SYSTEM_CURL=ON -DUSE_GXM=ON -DUSE_VITA_SHARK=OFF \
        -DCMAKE_BUILD_TYPE=Release && \
        cmake --build cmake-build-psv"
```

</details>

### 交叉编译 PS4 可执行文件

使用本地环境来编译可以参考: 
 - [PacBrew 环境安装](https://github.com/PacBrew/pacbrew-packages)
 - [borealis 编译指南](https://github.com/xfangfang/borealis/wiki/PS4)
 - [编译 wiliwili 依赖的第三方库](https://github.com/xfangfang/wiliwili/blob/dev/scripts/ps4/Dockerfile)

<details>

```shell
docker run --rm -v $(pwd):/src/ xfangfang/wiliwili_ps4_builder:latest \
    "cmake -B cmake-build-ps4 -DPLATFORM_PS4=ON \
        -DMPV_NO_FB=ON \
        -DUSE_SYSTEM_CPR=ON && \
        make -C cmake-build-ps4 -j$(nproc)"
```

</details>

### 交叉编译 PS5 payload

需要安装 [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)、`cmake`、`ninja`、`git`、`curl` 和 `python3`。构建脚本固定使用 `ps5-payload-dev/SDL` 的 `37acbcac579944f196c1620c44b108cf52985749`，并修复 PS5 视频驱动的直接显存分配、OSMesa 默认路径、VideoOut 错误诊断和 OSMesa 退出生命周期。

```shell
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
./scripts/ps5/build.sh
```

产物在 `build-ps5/ps5/`：默认包含 `wiliwili.elf`（全部资源内置）、`osmesa-installer.elf`、`libOSMesa.so.8`、`ca-bundle.crt`、`icon0.png` 和 `homebrew.js`。PS5 上需要先运行 [ps5-payload-websrv](https://github.com/ps5-payload-dev/websrv)；随后从构建主机执行：

```shell
./scripts/ps5/deploy.sh <PS5_IP>
```

部署会把应用安装为 `/data/homebrew/wiliwili/wiliwili.elf`，并写入 websrv 自动发现所需的图标和启动扩展。

手柄启动需要在 PS5 上一次性安装 [Homebrew Launcher PKG](https://github.com/ps5-payload-dev/websrv/raw/refs/heads/master/homebrew/IV9999-FAKE00000_00-HOMEBREWLOADER01.pkg)。每次 PS5 重启后重新越狱并加载 `websrv`，从 PS5 主界面打开 Homebrew Launcher；`wiliwili` 会自动出现在列表中，用方向键选择、按 × 启动，按 ○ 返回。首次部署旧版本后重新执行一次 `deploy.sh`，即可生成该列表项。

首次部署完成后，应用文件已保存在 PS5 的 `/data/homebrew/wiliwili/`。PS5 重启并重新加载 `websrv` 后，只需启动应用，不需要再次上传：

```shell
./scripts/ps5/start.sh <PS5_IP>
```

该脚本通过 websrv 的 `/hbldr` 以前台 BigApp 启动 `/data/homebrew/wiliwili/wiliwili.elf`。如果应用文件尚未部署，先运行上面的 `deploy.sh`；`/elfldr` 仅用于首次安装 OSMesa、CA bundle、应用文件和手柄启动元数据，不用于日常启动。

如需将资源从 ELF 中拆出，构建时设置 `WILIWILI_PS5_EXTERNAL_RESOURCES=1`。此时构建包会生成 `build-ps5/ps5/resources/`，应用运行时从 `/data/homebrew/wiliwili/resources/` 读取资源。外置资源模式必须使用 FTP 部署，脚本会递归上传整个 `resources/` 目录。

PS5 外置资源部署前，先在 Payload Manager 中加载 `websrv` 和 `zftpd`。zftpd 的 PS5 默认 FTP 端口是 `2120`；如果端口被占用，它会顺延到后续端口，按主机屏幕提示填写实际端口。PS5 重启后这些 payload 需要重新加载。

```shell
WILIWILI_PS5_EXTERNAL_RESOURCES=1 ./scripts/ps5/build.sh
WILIWILI_PS5_FTP_PORT=2120 ./scripts/ps5/deploy.sh <PS5_IP>
```

如果使用非默认构建目录，另设 `WILIWILI_PS5_BUNDLE=/绝对路径/ps5`。

部署脚本会同时安装 CA bundle 到 `/data/homebrew/wiliwili/ca-bundle.crt`，HTTPS 请求保持证书校验；不要通过关闭 TLS 校验来规避证书错误。
- PS5 的 OSMesa 会注册进程退出清理回调；SDL 退出时只释放函数表，不调用 `dlclose`，让动态库保持映射到进程结束，避免 payload loader 的卸载实现触发退出阶段跳转到失效代码。

默认内置资源模式通过临时 HTTP 服务安装 OSMesa、CA bundle、应用 ELF 和手柄启动元数据，再通过 websrv 的 `/hbldr` 以前台 BigApp 启动 `wiliwili.elf`。外置资源模式改用 FTP 上传所有文件后再通过 `/hbldr` 启动；若 PS5 无法访问自动探测出的主机地址，设置 `WILIWILI_LOCAL_HOST=<主机局域网 IP>` 后重试。应用配置保存在 `/data/homebrew/wiliwili/config/`。

### GLFW or SDL

wiliwili 使用 nanovg 绘制图形和文字，对于创建窗口、按键触摸、输入法等支持是通过 GLFW(默认) 或 SDL 完成的。

因为 GLFW 支持平台有限，在移植到新平台时可以使用 SDL 或者自行实现上述对应接口。

```shell
# 示例
cmake -B build -DPLATFORM_DESKTOP=ON -DUSE_SDL2=ON
cmake --build build
```

<br>

# 应用截图

<p align="center">
<img src="docs/images/screenshot-3.jpg" alt="screenshot">
<img src="docs/images/screenshot-4.jpg" alt="screenshot">
</p>

# Acknowledgement

The development of wiliwili cannot do without the support of the following organization and open source projects.

- Toolchain: devkitpro, switchbrew, vitasdk OpenOrbis and PacBrew
    - https://github.com/devkitPro
    - https://github.com/switchbrew/libnx
    - https://github.com/vitasdk
    - https://github.com/OpenOrbis
    - https://github.com/PacBrew
- UI Library: natinusala and XITRIX
    - https://github.com/natinusala/borealis
    - https://github.com/XITRIX/borealis
- Video Player: Cpasjuste, proconsule, fish47 and averne
    - https://github.com/Cpasjuste/pplay
    - https://github.com/proconsule/nxmp
    - https://github.com/fish47/FFmpeg-vita
    - https://github.com/fish47/mpv-vita
    - http://github.com/averne/FFmpeg
    - http://github.com/averne/mpv
- Misc
    - https://github.com/libcpr/cpr
    - https://github.com/nlohmann/json
    - https://github.com/nayuki/QR-Code-generator
    - https://github.com/BYVoid/OpenCC
    - https://github.com/imageworks/pystring
    - https://github.com/sammycage/lunasvg
    - https://github.com/cesanta/mongoose
    - https://chromium.googlesource.com/webm/libwebp
    - https://github.com/fancycode/MemoryModule

# Special thanks

- Thanks to Crowdin for supporting [open-source projects](https://crowdin.com/page/open-source-project-setup-request).
