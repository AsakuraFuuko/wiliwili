# 03 — 原生标题线现状盘点 + 新树（`wiliwili-native/`）迁移清单

> 调研笔记。**未改动任何源码、未构建、未跑测试。** 全部路径以 `/root/workspace/ps5wiliwili/` 为根。
> 相关工作：`notes/01-agc-bringup.md`（AGC 最小上屏：DCB/寄存器/shader 工具链）——本文件只做**现状盘点 + 迁移**，不重复那份的 AGC 细节。
> 引用的外部实现（`blackbearreloaded/ps5-native-app-boilerplate`、`blackbearreloaded/ps5-opengl`、`ps5-payload-dev/SDL`）均为 **GPL-3.0**，与 wiliwili（GPL-3.0）兼容；逐项出处见附录 A。

---

## 0. 结论（先看这个）

1. **老原生线（`wiliwili/`）是一条功能完整的交付线**：AGC/GL 3.3 变体真机验证过「主界面渲染 ✓、颜色正确 ✓、无撕裂 ✓、DNS/HTTPS ✓、首页真实数据 ✓、封面按帧队列渐进加载 ✓、可 ffpkg 安装可启动 ✓」，唯一缺陷是 **≈4.7 fps**（`run-continuation/ps5-port-status.md`「收尾状态（原生标题）」「批处理优化实验（2026-09-23）与最终结论」）。软渲染变体（OSMesa）**未完成**，卡在 SDL 初始化。
2. **原生线的实现方式不是 CMake 分支。** 两棵树的 `CMakeLists.txt` **逐字节相同**（`diff -q wiliwili/CMakeLists.txt wiliwili-native/CMakeLists.txt` 无输出）：原生构建以 **payload 构建产出的 `compile_commands.json` 为唯一源清单**，由 `scripts/ps5/native/native_build.py:64-105` 重定向编译标志后离线编译/链接/转换/打包。⇒ **迁移时不要给 CMake 加 `PS5_NATIVE_APP` 分支**（那会让 CDB 与原生构建分叉，正是老线刻意避开的）。
3. **`scripts/ps5/native/` 已在新树就位，且与老树逐字节一致**（`diff -rq wiliwili/scripts/ps5/native wiliwili-native/scripts/ps5/native` → IDENTICAL，22 个文件）。⇒ 迁移工作量全部集中在**应用层 8 个文件 + borealis 5 个文件**（§2/§3）。
4. **新树缺的只有"构建产物 + 主机工具"**：payload 的 CDB/libromfs 归档（Step 1 生成）、UFS2Tool（复制旧树）。三个工具链输入（boilerplate、ps5-opengl 的 SDL2/GL 前缀、AGC stub 目录）已在 `ps5-native/` 中存在，且 `build-native.sh` 的默认值就是从 `<repo>/..` 推导的，天然可达（§3 Step 0 已逐条实测存在）。
5. **渲染路线必须二选一**（互斥的 VideoOut 归属，见 `notes/01-agc-bringup.md` §4.4）：GL/AGC 变体（照抄老树，可交付、~4.7 fps）或软渲染变体（未完成，且 llvmpipe 被标题 JIT 权限挡住、softpipe 建 context 又被 Mesa 版本判定拒绝）。建议"原生线 2.0"第一阶段**只保留 GL 变体**作为可运行基线，软渲染另立任务。
6. **payload 线代码可直接复用**，需要回改的只有 13 处宏分支；其中**最容易漏、后果最重的一处是 `HTTP::runAsync` vs cpr `GetCallback`**——它在头文件里（`wiliwili/include/api/bilibili/util/http.hpp:171-183`），标题沙箱里走 cpr multi 路径会崩在首个请求处（`ps5-port-status.md`「原生标题运行期排障记录」）。
7. 标题内两条硬边界，所有移植决定都由它们推出：**不能 `dlopen`**（连自己镜像里的 50 KB 库都失败 ⇒ 一切静态链接）、**不能 klog**（`syscall 0x259` 在沙箱内直接杀进程 ⇒ 日志改走 `/download0` + UDP）。两者已由 `native_shims.c` / `native_libc_compat.c` 兜住（§1.4）。
8. §4 列了 **13 条真机已证伪的坑**（含 AGC 空桩、BGRA 互换、`/system_ex` 空间、JIT 权限、键盘模块缺失等），**不要重踩**。
9. 工作量诚实估计（§5）：新树产出**可安装/可启动/能联网/能画界面的 GL 原生标题** ≈ **1.5–2 人天**（8–14 人时，其中一半是首次构建与真机往返）；把软渲染恢复到可用 **+3–5 人天**；AGC 渲染层重写是另一量级（见 `notes/01-agc-bringup.md`）。

---

## 1. 原生线**现在已经有什么**（能力清单）

### 1.1 总表（能力 → 脚本 → 依据 → 用法）

| 能力 | 脚本 / 文件 | 关键依据（文件:行） | 用法 |
|---|---|---|---|
| 一键构建原生标题 | `scripts/ps5/native/build-native.sh` | 校验 5 个输入 + `ps5-native-tool` + `libc.prx` sha256，末尾 `exec native_build.py` | `[环境变量…] bash scripts/ps5/native/build-native.sh` |
| 编译/链接/转换/打包核心 | `scripts/ps5/native/native_build.py` | 见 §1.2 | 由上面脚本调用 |
| 静态软件渲染库（Mesa 22.1.7） | `scripts/ps5/native/build-osmesa.sh` | 5 处 Mesa 源码补丁 + thin archive 展开 | `bash scripts/ps5/native/build-osmesa.sh` |
| ffpkg 打包 + FTP 安装 | `scripts/ps5/native/install-ffpkg.sh` | `install-ffpkg.sh:33`（UFS2Tool 路径）、`:43`（`newfs -D dist out wiliwili`）、`:51-59`（远端大小校验） | `bash scripts/ps5/native/install-ffpkg.sh <PS5_IP> <TITLE_ID>` |
| 真实目录安装（`/system_ex/app`+`/user/app`） | `scripts/ps5/native/deploy-native.sh` | `deploy-native.sh`（先传其余文件，最后传 `eboot.bin`；`param.json`/`icon0.png` 进 `/user/app`） | `[WILIWILI_PS5_FTP_PORT=2120] bash scripts/ps5/native/deploy-native.sh <IP> <ID>` |
| 标题注册（目录形态） | `scripts/ps5/native/register-native.sh` + `register_title.c` | `register_title.c:27`（要求 9 字符 ID）、`:38`（`sceAppInstUtilAppInstallTitleDir`） | `/elfldr` 跑 `register-native.elf <TITLE_ID>` |
| 远程启动 + 进程确认 | `scripts/ps5/native/launch-native.sh` | `/launch?titleId=`；`launch-native.sh:30,32,35-45`（503 也算成功，改用 `/processes_list` 判定） | `bash scripts/ps5/native/launch-native.sh <IP> <ID>` |
| 打包→上传→启动→收日志 一条龙 | `scripts/ps5/native/run-title.sh` | `run-title.sh:23,26`（UFS2Tool 必须存在）、`:35`（上传 `/data/homebrew`）、`:39`（等 25 s）、`:41`（UDP 监听）、`:46`（`/launch`） | `bash scripts/ps5/native/run-title.sh <TITLE_ID> [秒]` |
| 等主机唤醒后自动跑一轮 | `scripts/ps5/native/wait-and-run.sh` | 轮询 8084 → 起载荷 → 打包/上传/启动/收日志 | `bash scripts/ps5/native/wait-and-run.sh <dist> <ID> [分钟]` |
| 实时日志（UDP） | `scripts/ps5/native/log-listen.py` + `native_shims.c:218-238` | 默认 `WILIWILI_LOG_HOST "192.168.100.7"`、端口 9999（`native_shims.c:186-189`） | `( timeout 80 python3 scripts/ps5/native/log-listen.py 9999 > /tmp/title.log 2>&1 & )` |
| 启动日志（镜像内） | `scripts/ps5/native/log-tail.sh`（FTP）、`app-log.sh`（HTTP） | 取 `download0.dat`（UFS2）→ `UFS2Tool extract` → `wiliwili-boot.log` | `bash scripts/ps5/native/log-tail.sh <IP> <ID> 25` |
| 崩溃地址符号化 | `scripts/ps5/native/resolve-crash.py` | 依赖 `native_shims.c:608-706` 崩溃处理器写的 `crash: addr/rip/base/rsp/rb` + `bt:` + `stk:` | `python3 scripts/ps5/native/resolve-crash.py 0x<rip>` |
| 主机侧真 shell | `scripts/ps5/native/console-shell.sh` | shsrv（telnet 2323），命令集极小（无 `head/tail/wc`） | `bash scripts/ps5/native/console-shell.sh <IP> "df -h /system_ex"` |
| 大文件绕过 FTP 断流 | `scripts/ps5/native/fetch_payload.c` | 主机侧 HTTP 拉取后本地落盘（`:1-11` 说明与用法） | `prospero-clang -O2 fetch_payload.c -o fetch-payload.elf` → `pldmgr /loadpayload:fetch-payload.elf` |
| 诊断：NULL 参数追踪 | `scripts/ps5/native/native_libc_trace.c` | 仅在 `PS5_NATIVE_LIBC_TRACE=1` 时编入（`native_build.py:201-206`） | 见 §1.2 环境变量 |

### 1.2 构建链（`native_build.py` 到底做了什么）

**输入**（`build-native.sh:38-48` 的默认值，全部可用环境变量覆盖）：

| 变量 | 默认值 | 用途 |
|---|---|---|
| `PS5_NATIVE_TOOLCHAIN` | `<ws>/ps5-native/ps5-native-app-boilerplate` | PIE 链接脚本、`app_crt.cpp`、`prospero-clang18` 包装器、`runtime/libc.prx` |
| `PS5_NATIVE_SDK` | `/opt/ps5-payload-sdk` | `prospero-lld`/`prospero-strip`、`target/lib`、homebrew 端口库（mpv/ffmpeg/curl/…） |
| `PS5_NATIVE_SDL2_PREFIX` | `<ws>/ps5-native/ps5-opengl/build/native-sdl2/sdk` | 原生 SDL2（`libSDL2.a` + 头） |
| `PS5_OPENGL_PREFIX` | `<ws>/ps5-native/ps5-opengl/build/sdk/ps5-opengl-sdk-0.3.0/sdk` | `libPS5OpenGLCore33.a`、`libSceAgc.so`/`libSceAgcDriver.so` |
| `PS5_NATIVE_OUT` | `<repo>/build-ps5/native` | 对象/中间产物/`dist/` |
| `PS5_NATIVE_CDB` | `<repo>/build-ps5/compile_commands.json` | payload 构建的编译数据库（**唯一源清单**） |
| `PS5_NATIVE_TITLE_ID` | `PPSA99010` | 输出 `dist/<ID>/` |
| `PS5_NATIVE_JOBS` | `nproc` | 并行编译 |

**附加开关**（`native_build.py` 读环境）：`WILIWILI_NATIVE_PROBE`（`:41-42`，只编启动探针）、`PS5_NATIVE_OSMESA_DIR`（`:47-48`，软渲染）、`PS5_NATIVE_OSMESA_LLVM=0`（`:265,279`，去掉 llvmpipe/LLVM）、`WILIWILI_SKIP_HOME_REQUEST=1`（`:49-50`）、`PS5_NATIVE_LIBC_TRACE=1`（`:205-206`）、`PS5_NATIVE_ROMFS_ARCHIVE`（`:239-242`）、`PS5_NATIVE_STUB_DIR`（`:369`）、`PS5_OPENGL_NATIVE_APP`（`:186-187`）。

**五阶段**：

1. **CDB → 编译计划**（`compile_plan`：`native_build.py:64-105`）：逐条取 CDB 的 `command`，丢掉 `prospero-clang*`、`-o`、`-c`、`--sysroot=`、`-DUSE_GL2`/`-DUSE_GLES*`、`-I…ps5-sdl-prefix`（`:94`），改加 `NATIVE_DEFINES`（`:32-37`）= `-DUSE_GL3 -DPS5_NATIVE_APP -DBRLS_RESOURCES="/app0/assets/" -fexceptions -frtti`，并补 `-I{SDL2}/include`、`-I{SDL2}/include/SDL2`、`-I{GL}/include`（`:100`）；对象落在 `build-ps5/native/obj/<相对路径点号化>.o`（`:102`）。
   - ⚠️ CDB 里本来就带 `-DBRLS_RESOURCES=\"/data/homebrew/wiliwili/resources/\"`，因此 `:97-99` 的条件会**跳过**上面那条 `-DBRLS_RESOURCES`。只要仍用 libromfs 内嵌资源，这个定义是惰性的；**若改成外置资源，标题会去读 `/data/homebrew`（沙箱读不到）**。
2. **增量编译**（`object_is_current` `:107-134` + `compile_sources` `:136-169`）：用 `-MMD -MF obj.d -MT obj`（`:147`）判断头文件变更；`compile-flags.txt`（`:476-486`）记录 `NATIVE_DEFINES`，变了就清空 `obj/` 全量重编。
3. **运行时对象**（`compile_runtime_objects` `:171-220`）：`tooling/native/{app_crt.cpp,app_cpp_runtime.cpp}`（`-std=c++20 -fno-exceptions -fno-rtti`）+ **`app_heap.c`**（`--wrap` 分配族；解析顺序 `PS5_OPENGL_PREFIX/../native-app`（实测命中 SDK 包内副本 `ps5-opengl-sdk-0.3.0/native-app/app_heap.c`）→ `PS5_OPENGL_NATIVE_APP` → `<toolchain>/../ps5-opengl/native-app`，`:184-197`）+ 本线自己的 `native_shims.c`、`native_libc_compat.c`、`native_regex.c`（`-std=gnu11`，带 `-DWILIWILI_*` 功能宏）。
   - **`runtime_shims.c`/`agc_link_stub.c`/`agc_driver_link_stub.c` 刻意不编**（`:188-190` 注释）：本地定义 AGC 入口会让每次 GPU 提交变 no-op（历史事故，见 §4）。
4. **链接**（`link` `:227-386`）：`prospero-lld -T <boilerplate ps5-pie.ld + 4 条 `PROVIDE(__eh_frame*)`>`（`:305-317`）`--eh-frame-hdr` + 6 个 `--wrap=`（`:319-322`）+ `--version-script scripts/ps5/native/app-symbols.map`（`{ local: *; }`，`:339`）+ `-e _start`；归档 = libromfs + `libSDL2.a` + `libunwind/libc++abi/libc++` + `libcurl.a` + （`PS5_OPENGL_PREFIX` 的 `-lPS5OpenGLCore33` 或 OSMesa 静态集）+ `libclang_rt.builtins-x86_64.a`（`:286`）；`--start-group` 里是 mpv/ffmpeg/openssl/curl/… + `-lSceAgc -lSceAgcDriver -lSceSysmodule`（`:231-237`）。
   - 保留一份带符号的 `llvm-pie-symbols.elf`，再 `prospero-strip --strip-debug`（`:355-362`）——崩溃符号化就靠它（`resolve-crash.py`）。
5. **转换 + 签名 + 组装**：`ps5-native-tool link --stub-dir <含 libSceAgc.so 的目录> --module-sdk 0x02000009 --companion-sdk 0x08050001 --strip-sections --file-name eboot.elf`（`:367-382`）→ `ps5-native-tool self --sign --magic 0x1D3D154F`（`:396-397`）→ `dist/<ID>/`：`eboot.bin`、`sce_module/libc.prx`、`assets/ca-bundle.crt`、`sce_sys/icon0.png`（Pillow 缩到 512×512，`:420-441`）、`sce_sys/param.json`（模板 + `titleId`/`conceptId`=ID 后 5 位/`contentId=UP9000-<ID>_00-WILIWILI00000000`，`:443-456`）。
   - `ps5-native-tool` 由 `build-native.sh:76-89` 用宿主 `clang++`/`g++` 现场编译（依赖 `boilerplate/.deps/native/zlib/root` 下的 `libz.a`，实测在 `.../root/usr/lib/libz.a`，脚本用 `find` 递归找，OK）。

### 1.3 打包 / 安装 / 注册 / 启动（三条已验证路径）

| 路径 | 机制 | 适用 | 关键约束 |
|---|---|---|---|
| **真实目录** | `deploy-native.sh`：文件进 `/system_ex/app/<ID>/`，`param.json`+`icon0.png` 进 `/user/app/<ID>/sce_sys/`；再 `register-native.sh` 调 `sceAppInstUtilAppInstallTitleDir` | `/system_ex` 有余量时 | `/system_ex` 只有 1.5 GB 且实测**只剩 19.5 MB**；FTP 大文件在 ~85 MB 处 `ENOSPC`；运行时不能覆盖 `eboot.bin`（FTP 550，且内存映射会读到旧映像） |
| **ffpkg 镜像**（当前主用） | `install-ffpkg.sh`：`UFS2Tool newfs -D dist /tmp/<ID>.ffpkg wiliwili` → FTP 上传 `/data/homebrew/<ID>.ffpkg` → ShadowMountPlus（`app_install_all=1`）挂载 + 注册 | 任何体积（126 MB 镜像 FTP ≈5 s） | **不要**让主机侧 `http2_get` 拉大文件（会拖死主机）；残留挂载点要先 `umount /mnt/shadowmnt/<旧挂载>` |
| 目录形态被 ShadowMountPlus 发现 | `/data/homebrew/<ID>/` | 已证伪 | nullfs 挂载路径上执行 → `SIGSYS` → `CE-107750-0`（`ps5-port-status.md`「真机结论（2026-09-22）」） |

启动：`curl "http://<IP>:8080/launch?titleId=<ID>"`（**503 也可能是成功**，用 `:8084/processes_list` 判定，`launch-native.sh:38-45`）。
日志三通道：`/download0/wiliwili-boot.log`（`native_shims.c:474-492`，每行 `fsync`）+ UDP 实时（`native_shims.c:218-238`）+ klog 3232（只对主机/内核有效）。
调试接口：klog 3232、gdbsrv 2159、pldmgr 8084（`/processes_list`、`/loadpayload:<file>`、`/process_kill?pid=`）、shsrv 2323、zftpd 2120、websrv 8080。

### 1.4 兼容层（标题沙箱缺什么、谁来补）

| 缺的东西 | 补法 | 依据 |
|---|---|---|
| 分配族（`--wrap=malloc…`）的"真"入口 | 独立 mspace（64 MB mmap + `sceLibcMspace*`） | `native_shims.c:46-116` |
| Mesa GL dispatch 的 C++ TLS 初始化 | `_ZTH23_mesa_glapi_tls_Context` 空实现 | `native_shims.c:119-122` |
| `kernel_mprotect`（payload 跨进程补丁能力） | 返回 `EPERM` | `native_shims.c:130-140` |
| `__dlopen/__dlsym/__dlclose/__dladdr` | 返回 `ENOSYS`/假句柄（标题禁止运行期加载） | `native_shims.c:146-183` |
| `klog_puts`（0x259） | 改写入启动日志 | `native_shims.c:813-818` |
| clean-room libc 面（locale `*_l`、`regcomp` 家族、`localtime_r/gmtime_r`、`pipe2/recvmmsg/sendmmsg/mkostemp/mkstemps/utimensat`、`popen/pclose`、`catopen/catgets/catclose`、`dladdr`、`getpwuid_r`、`if_nametoindex`、`__emutls_get_address`、`strsignal`、`sbrk`、`__xuname`、`dirfd`、`qsort_r`、`aligned_alloc`、`printf` 重定向、`realpath`、`isatty`、zstd 的 4 个 trace 钩子…） | `native_libc_compat.c`（1602 行，`newlocale:89`、`pipe2:455`、`recvmmsg:539`、`__emutls_get_address:681`、`getaddrinfo:990`、`fcntl:1081`、`dlopen:1564`…） | `ps5-port-status.md`「原生标题运行期排障记录」 |
| `getaddrinfo`/`getnameinfo`（平台版在 `libScePosixForWebKit` 里，标题内空指针崩） | 自实现：DNS 走 `sceNetResolverStartNtoa`（**timeout/retry/flags 必须为 0**）、必要时自写 UDP 查询 | `native_libc_compat.c:719-1060` |
| POSIX 正则 | `native_regex.c`（631 行，扩展子集，超范围报 `REG_BADPAT`） | `native_regex.c:1-19` |
| 软件渲染期的 `dlopen`（SDL 用它取 OSMesa） | `#ifdef WILIWILI_SOFTWARE_RENDER` 的假句柄 + 静态符号表 | `native_libc_compat.c:1512-1602` |
| 符号可见性 | `app-symbols.map` = `{ local: *; }`（模块转换器只发布 import，不支持应用导出；上游那份只藏 `_Zn*`/`_Zd*`，静态链 libc/mpv/Mesa 时会有 559 个导出被拒） | `app-symbols.map:1-14`、`ps5-port-status.md`「符号可见性」 |
| 环境变量（标题拿不到 env） | `/app0/assets/wiliwili-options.txt` 逐行 `KEY=VALUE` → `putenv`，在 `.preinit_array` 执行 | `native_shims.c:725-756`（其中 `MESA_GL_VERSION_OVERRIDE=3.3` 是默认注入，`:775-782`） |
| 崩溃现场 | SIGSEGV/SIGBUS/SIGILL/SIGFPE 处理器：`crash: addr/rip/base/rsp/rb` + `bt:` + 64×8 个栈字 | `native_shims.c:608-710` |

### 1.5 软渲染分支（`PS5_NATIVE_OSMESA_DIR`）

- `build-osmesa.sh` 交叉编译 **Mesa 22.1.7** 成静态库（5 处源码补丁：`config-tool→cmake`、`DETECT_OS_UNIX→0`、`open_memstream→NULL`、去 `-DUSE_ELF_TLS`、`CACHE_LINE_SIZE 64→128`），产物 `stage/`（llvmpipe，222 MB）与 `stage-softpipe/`（31 MB）；**thin archive 必须就地展开**。
- 链接集见 `native_build.py:259-303`（含 `--whole-archive libsoftpipe.a` —— OSMesa 前端靠驱动入口点建 screen，普通归档拉不动）。
- 启动探针在 `main()` 之后调用（`main.cpp:64-66` → `native_shims.c:365-434`），因为**构造器阶段调 Mesa 会崩**（§4）。
- **现状（2026-09-24 最后一次构建）**：`build-ps5/native/compile-flags.txt` 记的是 `WILIWILI_OSMESA_PROBE` + `WILIWILI_SOFTWARE_RENDER`（llvmpipe 变体），`dist/PPSA99012/` 就是它；卡点是 SDL 仍是 ps5-opengl 的 `ps5-g19` 桥（走 EGL/AGC），而软渲染构建已去掉 GL/EGL ⇒ **需要"带 OSMesa 后端的 SDL + `ps5` 驱动 + dlopen→静态符号"**，见 §3 Step 4。

### 1.6 能力边界（决定了所有"能不能做"）

| 能力 | 原生标题线 | 依据 |
|---|---|---|
| `sceVideoOut*`（开屏/注册缓冲/flip） | ✅ | 交付线在用；`notes/01-agc-bringup.md` §1 |
| AGC（`libSceAgc`/`libSceAgcDriver` 导入） | ✅ 已链（`-lSceAgc -lSceAgcDriver`，`native_build.py:231-237`） | §4「AGC 空桩」 |
| `sceVideodec2` 硬解 | 未实测，但**这是唯一可能成功的上下文**（注册应用槽） | `ps5-port-status.md`「硬解参考实现」 |
| `dlopen` 任何库 | ❌ | §4 |
| `klog` / `mprotect(rx)` / **JIT**（`sceKernelJitCreateSharedMemory`） | ❌（`0x80020001` = `EPERM`） | `ps5-native-handoff.md` §2、`native_shims.c:307-360` |
| 读取外置资源目录（`/data/homebrew`、目录枚举） | ❌（`EPERM`） | §4 |

---

## 2. 原生线 vs payload 线的**应用层差异**（"搬过去要回改什么"）

比较基准：**老原生线** `/root/workspace/ps5wiliwili/wiliwili/`（HEAD `88e5876` + 未提交改动）↔ **新树（payload 线）** `/root/workspace/ps5wiliwili/wiliwili-native/`（HEAD `1c84a1a`，= `wiliwili-payload`）。
完整差异扫描：`diff -rq wiliwili/wiliwili wiliwili-native/wiliwili` → **只有 8 个文件**；`diff -ru …/library/borealis`（排除 SDL 子模块）→ **再 5 个文件**（`sdl_input.cpp` 已由 payload 提交带着 `PS5_NATIVE_APP` 分支，无需回改）。

| # | 文件 | payload 形态 | 原生线形态（要回改成的样子） | 关键行 |
|---|---|---|---|---|
| 1 | `wiliwili/include/api/bilibili/util/http.hpp` | `#if defined(PS5)` 下用 `wiliwili_boot_log`；CA = `/data/homebrew/wiliwili/ca-bundle.crt`；`session->GetCallback(...)` | 宏改 `PS5_NATIVE_APP`；CA = `/app0/assets/ca-bundle.crt`；新增 `curl` `SetDebugCallback`；新增并使用 **`HTTP::runAsync()`**（阻塞 `session->Get()` 跑在 detach 线程）替代 cpr multi | CA `91`/`93`、`runAsync` 定义 `171-183`、`_cpr_get` 调用点 `192`（老树） |
| 2 | `wiliwili/include/utils/image_helper.hpp` | 通用/桌面尺寸；无上传队列 | `#elif defined(PS5_NATIVE_APP)` 封面缩到 `@336w_189h`；新增 `static void drainUploads()` | `83-95`、`125`（老树） |
| 3 | `wiliwili/source/utils/image_helper.cpp` | 内联 `brls::sync` 直接上传 | 上传入队（`std::deque`+`mutex`），每帧 1 张；`extern "C" wiliwili_drain_image_uploads()` | `218`、`220-228`、`296-307`（老树） |
| 4 | `wiliwili/source/main.cpp` | **定义** `wiliwili_boot_log`（`klog_puts` 包装，`:16`） | 只 `extern` 声明（定义在 `native_shims.c`）；加 `WILIWILI_OSMESA_PROBE` 调用；加 `WILIWILI_TEST_BV` 调试入口 | `16-17`、`26-39`、`56-66`、`105-118`（老树） |
| 5 | `wiliwili/source/utils/config_helper.cpp` | 无网络初始化；`getcwd`；`/data/homebrew/wiliwili[\/config]` | `#if defined(PS5_NATIVE_APP)`：`sceNetCtlInit/sceNetInit/sceNetPoolCreate/sceSslInit`；跳过 `getcwd`；路径改 `/download0/wiliwili[\/config]` | `30-35`、`1046-1058`、`1088-1091`、`1157-1160`、`1176-1178`（老树） |
| 6 | `wiliwili/source/utils/number_helper.cpp` | `std::random_device`（打不开 `/dev/urandom`） | 时钟+pid+计数器种子 | `68-77`（老树） |
| 7 | `wiliwili/source/api/util/wbi.cpp` | `session->GetCallback(...)` | `HTTP::runAsync(...)`（老树此处还留了一段**死代码** `char message[96]`+`snprintf`，回改时删掉） | `79`、`80-85`（老树） |
| 8 | `wiliwili/source/fragment/home_recommends.cpp` | 直接 `requestData()` | `#ifdef WILIWILI_SKIP_HOME_REQUEST` 二分用开关 | `111-117`（老树） |
| 9 | `library/borealis/.../extern/nanovg/nanovg_gl.h` | `outColor = result`；逐路径 `glDrawArrays` | `PS5_NATIVE_APP && !WILIWILI_SOFTWARE_RENDER` 时 `outColor = result.bgra`；`wiliwili_multi_draw()`（`glMultiDrawArrays` 合批）+ 6 处调用点 | `688-698`、`1044-1078`、`1095/1113/1181/1192/1204/1220`（老树） |
| 10 | `library/borealis/.../platforms/sdl/sdl_video.cpp` | payload：`ps5` 驱动 + 普通 GL 属性 + `nvgCreateGL3(STENCIL|ANTIALIAS)` | 原生：窗口标志 `SDL_WINDOW_SHOWN`（无 RESIZABLE/全屏）、GL 3.3 Core + DOUBLEBUFFER 且**不指定通道位宽**、`SDL_GL_SetSwapInterval(1)`、启动分步日志、`nvgCreateGL3(0)`、`endFrame()` 加 16.67 ms 节流 + `wiliwili_drain_image_uploads()` + `glFlush()` + `SwapWindow`、`clear()` 的 **BGRA 通道交换** | `22-27`、`181-206`、`207-215`、`278-300`、`313-316`、`333-341`、`348-357`、`358-369`、`373-381`、`397-407`、`423-428`、`489-525`、`538-551`（老树） |
| 11 | `library/borealis/.../platforms/sdl/sdl_platform.cpp` | `PS5_NATIVE_APP` → `ps5-g19` | 前面插一层 `WILIWILI_SOFTWARE_RENDER` → `ps5`（软渲染必须走 VideoOut） | `67-77`（老树） |
| 12 | `library/borealis/.../core/application.cpp` | 无钩子 | `wiliwili_trace_mark(1)` / `wiliwili_note_frame()`（`PS5_NATIVE_APP \|\| WILIWILI_SOFTWARE_RENDER`） | `746-748`、`816-827`（老树） |
| 13 | `library/borealis/.../core/view.cpp` | 无 | `wiliwili_boot_log` 的 **per-view draw 日志**（前 400 个视图） | `37-38`、`166-190`（老树） |

**可以直接用（不需要任何改动）**：`wiliwili/` 下其余全部业务代码（首页/搜索/播放器/弹幕/设置/缓存…）、`resources/`、`library/` 其它子模块、payload 提交 `3a06f50`（SDL flip "只保留一帧在飞"修复，用于软渲染变体的 payload SDL 前缀）与 `1c84a1a`（弹幕默认样式 `incline`：原生线同样想要，**别被老树版本覆盖掉**）、libromfs 内嵌资源机制。

**必须"双向合并"的两处（单向覆盖会丢东西）**：
- `config_helper.cpp`：老树有 `PS5_NATIVE_APP` 网络初始化 + `/download0` 路径，payload（`1c84a1a`）有 `danmaku_style_font` 的 PS5 默认 `incline`（`#if defined(PS5) …1… #else 0`）⇒ 合并时把 `#if defined(PS5)` 保留（`PS5_NATIVE_APP` 也定义 `PS5`）。
- `image_helper.hpp`：老树有 `@336w_189h` 与 `drainUploads`，payload 侧无；直接取老树版即可（payload 的这一处没有新东西）。

**必须新增（老树与 payload 都还没有）**：`wiliwili_boot_log` 的定义方（`native_shims.c`）已在 native 脚本目录里，但**payload 的 `main.cpp` 定义必须删掉**，否则重复定义/走错通道。

---

## 3. 迁移清单（在 `wiliwili-native/` 里按序执行）

### Step 0 — 前置检查（全部已在本机实测存在）

```bash
cd /root/workspace/ps5wiliwili/wiliwili-native

# ① 工具链输入（build-native.sh 默认从 <repo>/.. 推导；工作区同名目录已就位）
ls ../ps5-native/ps5-native-app-boilerplate/tooling/native/{ps5-pie.ld,app-symbols.map,app_crt.cpp,app_cpp_runtime.cpp}
ls ../ps5-native/ps5-native-app-boilerplate/tooling/prospero-clang18      # 编译包装器在 tooling/ 下，不是 tooling/native/
ls ../ps5-native/ps5-native-app-boilerplate/runtime/libc.prx
ls ../ps5-native/ps5-opengl/build/native-sdl2/sdk/lib/libSDL2.a           # GL 变体用
ls ../ps5-native/ps5-opengl/build/sdk/ps5-opengl-sdk-0.3.0/sdk/lib/libPS5OpenGL.a
ls ../ps5-native/ps5-opengl/build/sdk/ps5-opengl-sdk-0.3.0/native-app/app_heap.c   # 运行时对象（SDK 包内副本）
ls ../ps5-native/ps5-opengl/build/sdl-folder/.deps/native/ps5-payload-sdk/target/lib/libSceAgc.so

# ② SDK 与宿主工具
ls /opt/ps5-payload-sdk/bin/{prospero-lld,prospero-strip,prospero-clang}
command -v clang-18 jq ninja cmake g++ python3 readelf
python3 -c "import PIL"          # sce_sys/icon0.png 的 512×512 缩放

# ③ UFS2Tool（build-* 在 .gitignore 里，新树没有；四个脚本硬编码这个路径）
mkdir -p build-ps5/pkg-experiment/third_party
cp -a ../wiliwili/build-ps5/pkg-experiment/third_party/ufs2tool build-ps5/pkg-experiment/third_party/
```
- `sce_module_writer` 的 RELRO 同余补丁必须已在 boilerplate 里（本机已应用：`ps5-native-app-boilerplate/tooling/native/sce_module_writer.cpp:95,848-854`），否则大 `.data.rel.ro` 时报 "not congruently aligned"。
- `PS5_NATIVE_STUB_DIR` 的默认值（`ps5-opengl/build/sdl-folder/.deps/native/ps5-payload-sdk/target/lib`，51 个文件，含 `libSceAgc.so`/`libSceAgcDriver.so`）是**必需的**：`/opt/ps5-payload-sdk/target/lib/` 里**没有** AGC 桩，回落过去会改变行为（`native_build.py:369-374`）。

### Step 1 — 生成 payload 构建产物（CDB + libromfs 归档，两者都是 native 构建的硬输入）

```bash
cd /root/workspace/ps5wiliwili/wiliwili-native
PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk bash scripts/ps5/build.sh
```
产出：
- `build-ps5/compile_commands.json`（Ninja 自动生成；老树同源构建实测 **281 条**、281 个唯一 TU）；
- `build-ps5/library/borealis/library/lib/extern/libromfs/lib/libromfs-wiliwili.a`（≈12.8 MB；`native_build.py:239-247` 的默认路径就指这里，**缺失会直接 `SystemExit`**）。

备注：要**外置资源版**才需要 `WILIWILI_PS5_EXTERNAL_RESOURCES=1`；原生标题必须**内嵌**（沙箱不许目录枚举）⇒ 用默认 `OFF`。
`scripts/ps5/build.sh` 会 clone `ps5-payload-dev/SDL@37acbcac…` 到 `build-ps5/ps5-sdl-src` 并打 `scripts/ps5/sdl.patch`（GL 变体不需要该前缀；软渲染变体需要，见 Step 4）。

### Step 2 — 回改应用层（§2 的 #1–#8）

做法建议：以**老树 `../wiliwili/wiliwili/<同路径>` 覆盖**为基础，再做**两处合并**（`config_helper.cpp` 的弹幕默认值、`image_helper.hpp` 直接取老树版），并删除 `wbi.cpp` 的死代码块。验收：`grep -rn "PS5_NATIVE_APP" wiliwili/source wiliwili/include` 应覆盖上面 8 个文件；`grep -n "runAsync" wiliwili/include/api/bilibili/util/http.hpp` 命中。
**遗漏自检**（老树里 `GetCallback` 仍有 6 处直连：`video_detail_api.cpp:59,299,319,344,367`、`live_data.cpp:170`、`video_snapshot_core.cpp:85`、`version_helper.cpp:99`）——原生线**没有**把这些改掉；首启验证时若崩在这些接口，就照 `http.hpp:171-183` 把调用点逐个换成 `runAsync`。

### Step 3 — 回改 borealis（§2 的 #9–#13）

以 `../wiliwili/library/borealis/library/...` 覆盖对应 5 个文件。**建议顺手删掉 `view.cpp:166-190` 的 400 行 per-view draw 日志**（老树遗留调试代码，没有 `WILIWILI_TRACE` 门控，每行都是一次 open/write/fsync/close）。
`sdl_input.cpp` **不要改**：`407`（无 haptics）与 `605`（`sendRumble` no-op）已随 payload 提交 `135c4a0f` 在新树里。

### Step 4 — 选定渲染路线（互斥；建议先只做 GL）

| 路线 | 需要的额外准备 | 命令差异 |
|---|---|---|
| **GL/AGC（推荐，先做）** | 无（用 ps5-opengl 的 `native-sdl2/sdk`） | 见 Step 6 |
| 软渲染（未完成） | ① 用 `build-osmesa.sh` 产出 `stage-softpipe`（llvmpipe 被 JIT 挡，见 §4）；② **SDL 必须换成带 OSMesa 后端的 payload SDL 前缀**（`build-ps5/ps5-sdl-prefix`），并按 §4「键盘/IME」给它加 `SDL_PS5_KEYBOARD=OFF`；③ softpipe 建 core profile context 会被 Mesa 版本判定拒绝（`ctx->Version==0`）⇒ 需补 `softpipe` 能力或改判定阈值（或确认探针已改用的 `OSMESA_COMPAT_PROFILE` 路线可用） | 增加：`PS5_NATIVE_SDL2_PREFIX=$PWD/build-ps5/ps5-sdl-prefix PS5_NATIVE_OSMESA_DIR=../ps5-native/mesa-build/stage-softpipe PS5_NATIVE_OSMESA_LLVM=0` |

### Step 5 — 选 TITLE_ID

- 已占用：`PPSA99010`（GL/AGC）、`PPSA99011`（llvmpipe ffpkg）、`PPSA99012`（OSMesa 调试）。**建议新任务用 `PPSA99013`**，避免与主机上已注册标题/残留挂载点冲突。
- 约束：9 字符（`register_title.c:24`）；`conceptId` = 后 5 位、`contentId` = `UP9000-<ID>_00-WILIWILI00000000`（`native_build.py:443-456` 自动生成，不用手改）。
- 换 ID 前把旧镜像/挂载清干净：删 `/data/homebrew/<旧ID>.ffpkg`；`console-shell.sh <IP> "umount /mnt/shadowmnt/<旧挂载>"` 再 `rmdir`（否则 ShadowMountPlus 扫描器对残留挂载点空转）。

### Step 6 — 构建

```bash
cd /root/workspace/ps5wiliwili/wiliwili-native
PS5_NATIVE_CDB=$PWD/build-ps5/compile_commands.json \
PS5_NATIVE_TITLE_ID=PPSA99013 \
PS5_NATIVE_JOBS=$(nproc) \
  bash scripts/ps5/native/build-native.sh
```
期望产物：`build-ps5/native/dist/PPSA99013/{eboot.bin, sce_module/libc.prx, sce_sys/{param.json,icon0.png}, assets/ca-bundle.crt}`；`compile-flags.txt` 内容应为 `-DUSE_GL3 -DPS5_NATIVE_APP -DBRLS_RESOURCES=… -fexceptions -frtti`（无 `-DWILIWILI_*`）。
同时保留 `build-ps5/native/llvm-pie-symbols.elf`（崩溃符号化用）。

### Step 7 — 打包 / 安装

```bash
bash scripts/ps5/native/install-ffpkg.sh 192.168.102.118 PPSA99013    # ffpkg 路线（默认）
# 或（/system_ex 有余量时）：
WILIWILI_PS5_FTP_PORT=2120 bash scripts/ps5/native/deploy-native.sh 192.168.102.118 PPSA99013
bash scripts/ps5/native/register-native.sh 192.168.102.118 PPSA99013
```

### Step 8 — 启动与验证（观察点逐条对照）

```bash
# 载荷（主机重启后）：websrv / zftpd / shsrv / klogsrv
for p in websrv_v0.34.elf zftpd_v1.5.0.elf shsrv_v0.20.elf klogsrv_v0.9.elf; do
  curl -sS "http://192.168.102.118:8084/loadpayload:$p"; done

# UDP 实时日志（必须在子 shell 里，否则污染 fd）
( timeout 120 python3 scripts/ps5/native/log-listen.py 9999 > /tmp/PPSA99013.log 2>&1 & )
curl -sS "http://192.168.102.118:8080/launch?titleId=PPSA99013"
bash scripts/ps5/native/launch-native.sh 192.168.102.118 PPSA99013   # 或用 8084 /processes_list 看进程
```
验收信号（按老树真机日志的固定顺序，`ps5-port-status.md`「原生标题网络打通」）：
`wiliwili: preinit` → `wiliwili: build <日期>` → `constructors: start/done` → `main entered` → `net init=…` → `sdl: video driver=ps5-g19` → `sdl: window 1920x1080` → `sdl: gl vendor=… renderer=PS5 AGC / Mesa …` → `frame N swap start`（帧持续推进）→ 首页 `http: code=200` → `main activity: content available`。
只看这三件事就够判断"能不能用"：**进程常驻（8084 有 `eboot.bin`）**、**帧持续推进**、**电视上有画面**。

### Step 9 — 清理（收尾）

- `dist/<ID>/assets/wiliwili-options.txt`：老树的成品里带着 `WILIWILI_TRACE=1`、`WILIWILI_TEST_BV=BV1Da411Y7U4`（调试用，逐帧日志会把帧率压到 ~5 fps，`native_shims.c:494-500`）⇒ 交付版删除该文件或清空。
- `view.cpp` 的 400 行 draw 日志（Step 3 已建议删）、`wbi.cpp` 死代码块（Step 2 已建议删）。
- `native_libc_trace.c` 默认不编（只在 `PS5_NATIVE_LIBC_TRACE=1` 时编），保持默认。
- `/tmp/*.ffpkg` 残留（旧树 205 MB + 126 MB 数个）可清。

---

## 4. 已知坑（真机结论，**不要重踩**）

| # | 现象 | 根因 | 处理 | 依据 |
|---|---|---|---|---|
| 1 | 画面全黑但 GL 无报错 | 链接了 SDK 模板里的 **AGC 空桩**（`agc_link_stub.c`/`agc_driver_link_stub.c`），GPU 提交全变 no-op | **不编这两个文件**（现在只编 `app_heap.c`），AGC 走导入 `-lSceAgc -lSceAgcDriver` + 含 AGC 桩的 `--stub-dir` | `native_build.py:171-199`、`ps5-port-status.md`「五个真机定位的坑」 |
| 2 | 红蓝互换 | SDK 扫描输出 RGBA8，主机 video-out 是 BGRA | nanovg 片段着色器 `outColor = result.bgra`（**必须用 C 预处理选择**，写进 GLSL 字符串无效）+ `clear()` 同步交换；**软渲染变体不能交换** | `nanovg_gl.h:688-698`、`sdl_video.cpp:538-551` |
| 3 | 整屏从左到右刷新/撕裂 | `eglSwapBuffers` 不等 GPU 完成就 present | `glFinish()`/`glFlush()` + `SDL_GL_SetSwapInterval(1)` | `sdl_video.cpp:489-525` |
| 4 | 封面加载冻住界面 | 纹理上传压在 UI 线程（11 ms/张、队列 ~400 ms） | 上传队列 + 每帧 1 张（`ImageHelper::drainUploads` ← `wiliwili_drain_image_uploads` 弱符号钩子）；封面尺寸降 `@336w_189h` | `image_helper.cpp:218-307` |
| 5 | DNS/首个请求崩 | 平台 `getaddrinfo` 在 `libScePosixForWebKit` 里；`sceNetResolverStartNtoa` 的 timeout/retry/flags **必须为 0**（否则 `0x80410116`） | 兼容层自实现 `getaddrinfo` + DNS | `native_libc_compat.c:719-1060` |
| 6 | 首个请求 SIGSYS 杀进程 | 裸 `syscall`；`fcntl(F_SETFL)` 被判非法（klog `PPRBUG-22859 … syscall 92`） | 零裸 syscall；`F_GETFD/F_SETFD` 直接成功，`F_SETFL` 用 `ioctl(FIONBIO)` | `native_libc_compat.c:1081-1120` |
| 7 | 标题"无法启动"/部署静默失败 | `/system_ex` 只剩 19.5 MB；FTP 大文件 ~85 MB 处 `ENOSPC` | 走 ffpkg 到 `/data/homebrew`；大文件只从 PC 上传，**别让主机侧 `http2_get` 拉** | `install-ffpkg.sh`、`ps5-port-status.md` |
| 8 | 标题无法 `dlopen` | 沙箱禁止运行期加载代码（连自己镜像里的 50 KB 库都失败） | 一切静态链接 | `ps5-port-status.md`「静态软渲染…」 |
| 9 | `SDL_Init(VIDEO)` 取指于 0 崩 | `libSceKeyboard`/`libSceImeDialog`/`libScePosixForWebKit` 固件有但标题不预载、也不许载；`sceSysmoduleLoadModule` 扫 0x00–0xFF 全失败 | SDL 编译期关键盘（`SDL_PS5_KEYBOARD=OFF` → `SDL_PS5_NO_KEYBOARD_MODULE`）+ 自实现 `getaddrinfo` | `ps5-native-handoff.md` §3、§4 |
| 10 | 探针"零日志 SIGSEGV" | 探针在**静态构造器阶段**调 Mesa（自身初始化未完成），且 context 为 NULL 还继续 `glGetString` | 探针移到 `main()` 之后；NULL 提前返回 | `native_shims.c:363-434`、`main.cpp:64-66` |
| 11 | 软渲染 llvmpipe 不可行 | 标题无 JIT 权限：`sceKernelJitCreateSharedMemory` → `0x80020001 (EPERM)`，执行生成代码直接杀进程 | 只剩 softpipe（不需 JIT），但 softpipe 报 `PIPE_CAP_GLSL_FEATURE_LEVEL=400`，OSMesa 要 core profile ⇒ `ctx->Version==0` 建 context 失败 | `ps5-native-handoff.md` §2、`native_shims.c:307-360` |
| 12 | 标题"能启动但没画面"整套 | 见 #1/#2/#3；此外帧率 ≈4.7 fps 属**驱动层面**（每次 draw ~0.5 ms），应用侧优化（关 AA、合批、去双缓冲重复光栅化）总共只把 369 ms 降到 206 ms | 不要把它当移植 bug 修 | `ps5-port-status.md`「批处理优化实验…最终结论」 |
| 13 | 运维细节 | 标题运行时**不能覆盖 `eboot.bin`**（FTP 550 + 内存映像）；ShadowMountPlus 残留挂载点会让扫描器空转；UDP 的 `WILIWILI_LOG_HOST` 是**编译期**宏（换开发机 IP 要重编，`native_shims.c:186-189`） | 部署脚本已做 size check；清挂载点再重扫 | `deploy-native.sh`、`ps5-native-handoff.md` §4 |

---

## 5. 工作量估计

| 工作项 | 人时 | 说明 |
|---|---|---|
| Step 0 前置（UFS2Tool 复制、依赖核对） | 0.5–1 | 本机已具备，基本是核对 |
| Step 1 生成 CDB + libromfs | 0.5–1 | `build.sh` 会 clone/构建 payload SDL，首次 20–40 分钟机器时间、人几乎不用管 |
| Step 2–3 应用层 + borealis 回改（13 处） | 3–4 | 以老树文件覆盖为主；两处合并 + 删调试残留需要人判断 |
| Step 6 首次原生构建（279–281 TU + 链接 + 转换） | 1–2 | 老树首次约 10–20 分钟机器时间；需处理增量缓存的坑（`compile-flags.txt` 变更会全量重编） |
| Step 7–8 ffpkg 安装 + 真机启动到"有画面 + 进程常驻" | 2–4 | 主要是主机往返（载荷重启、休眠、挂载点清理） |
| 网络/封面回归（`http: code=200`、封面上屏） | 1–2 | 第 5、6 条坑如果被遗漏才需要 |
| **小计：新树产出可运行 GL 原生标题** | **8–14 人时 ≈ 1.5–2 人天** | 不含新功能 |
| 软渲染变体恢复到可用（Step 4 第二条路线） | 24–40 人时 ≈ 3–5 人天 | 依赖 softpipe 的 Mesa 判定问题定位 + SDL OSMesa 后端 + 键盘关断补丁 |
| AGC 渲染层重写（真正的 GPU 路线） | 不在本批 | 见 `notes/01-agc-bringup.md`（shader 工具链 + DCB + 图元），量级为"重写渲染层" |
| `sceVideodec2` 硬解接入 | 不在本批 | 见 `ps5-port-status.md`「硬解参考实现」；对现有 llvmpipe 瓶颈收益有限 |

---

## 附录 A — 借用的外部实现与许可证

| 来源 | 许可证 | 本线借用的内容（文件/符号） |
|---|---|---|
| `blackbearreloaded/ps5-native-app-boilerplate` | GPL-3.0 | `tooling/native/{app_crt.cpp,app_cpp_runtime.cpp,ps5-pie.ld,sce_module_writer.cpp}`、`runtime/libc.prx`（clean-room libc）、`sce_sys/param.json` 模板 |
| `blackbearreloaded/ps5-opengl` | GPL-3.0 | `native-app/app_heap.c`（`--wrap` 分配族）、`libPS5OpenGLCore33.a`、AGC 桩（`libSceAgc.so`/`libSceAgcDriver.so`）、`integration/SDL2` 产出的原生 SDL2 前缀、以及 `native_shims.c:119-122` 的 TLS 空实现（同款定义也在 `ps5-opengl` 的 `runtime_shims.c`） |
| `ps5-payload-dev/SDL`（固定 `37acbcac`）+ `ps5-payload-sdk` | zlib / 各自许可证 | payload SDL 前缀与 ps5 video 驱动、`prospero-lld/…` 工具链、homebrew 端口库（mpv/ffmpeg/curl/…） |
| 上游 wiliwili 第三方（borealis/nanovg、cpr、lunasvg、yoga、fmt、tinyxml2、libromfs…） | 各自许可证（borealis/nanovg 为 zlib，cpr 为 MIT） | 未改动其许可证声明；本线改动均为 GPL-3.0 应用侧补丁 |

> GPL-2.0 项目（shadPS4、sharpemu、AnyPS5、prosperity）**只能读、不可复制代码**；本笔记未引用其代码。

## 附录 B — 新树缺失 / 现存产物清单（迁移时的对照表）

**新树（`wiliwili-native/`）当前状态**：HEAD `1c84a1a`（分支 `yoga`），工作区除 `?? scripts/ps5/native/` 外干净；`library/borealis` = `135c4a0f`、`library/cpr` = `970c2b2`（payload 补丁已提交）；**没有** `build-*` 目录、没有 `notes/`（本文件所在目录由本批任务创建）。

**需要从旧树搬/生成的**：

| 项 | 旧树位置 | 说明 |
|---|---|---|
| `compile_commands.json` | `wiliwili/build-ps5{,-romfs}/compile_commands.json` | 新树 Step 1 重新生成（也可先拷贝应急） |
| `libromfs-wiliwili.a` | `wiliwili/build-ps5{-romfs}/library/borealis/library/lib/extern/libromfs/lib/` | 同上 |
| UFS2Tool | `wiliwili/build-ps5/pkg-experiment/third_party/ufs2tool/` | 被 `.gitignore` 忽略，必须复制 |
| Mesa 静态库 | 已在工作区：`ps5-native/mesa-build/stage`（222 MB，llvmpipe）、`stage-softpipe`（31 MB） | 只有软渲染变体需要 |
| payload SDL 前缀 | `wiliwili/build-ps5/ps5-sdl-prefix` | 只有软渲染变体需要；**键盘关断改动只在 `build-ps5/ps5-sdl-src` 的未提交改动里，不在 `scripts/ps5/sdl.patch`**（`git -C … status` 显示 `M CMakeLists.txt / M src/video/ps5/SDL_ps5{keyboard,osmesa,video}.c`）⇒ 新树重建时要么补进 patch，要么重做 |

**旧树现存产物（仅供参考，均未随新树迁移）**：`build-ps5/native/dist/PPSA99010/eboot.bin` 108,305,303 B（Sep 23 10:39，GL/AGC）、`dist/PPSA99012/eboot.bin` 108,657,657 B（Sep 24 20:28，llvmpipe+`WILIWILI_TRACE`，带 14,781 个 LLVM 符号）；`/tmp/{PPSA99010,PPSA99010-osmesa,PPSA99011,PPSA99012,PPSA99012-run}.ffpkg`（126–215 MB）。
注意：这些尺寸与 `ps5-port-status.md` 早前记录的 76.5 MB / 103.3 MB 不同（期间链接集变过），**不要拿旧数字做校验依据**。
