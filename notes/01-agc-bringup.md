# 01 — AGC 最小上屏路径（清屏 / 一个四边形 / 一次 flip）

> 调研笔记。**不改动任何本地源码**；所有结论都标注外部来源（项目 / 文件 / 符号 / 行号）与许可证。
> 目标读者：要在 `PPSA99xxx` 注册应用槽里把 GPU 点亮的人。
>
> 外部仓库快照（`git clone --depth 1` 到 `/tmp/agcrefs/`）：
>
> | 目录 | 仓库 | 许可证 | 定位 |
> |---|---|---|---|
> | `ps5link-sdk/` | `Rufidj/ps5link-sdk` | GPL-3.0 | **面向原生标题的最小 GPU 例子**（`examples/gpu_cube`） |
> | `ps5-agc-gears/` | `mpereiraesaa/ps5-agc-gears` | GPL-3.0 | 最小硬件 demo + **完整可跑的单帧循环**（FW 12.02 实测 6 万帧） |
> | `EVO-PLAYER-PS5/` | `sainsaji/EVO-PLAYER-PS5` | GPL-3.0 | 裸金属 AGC UI + 完整工具链文档 + 大量**真机踩坑记录** |
> | `ProsperoLight/` | `blackbearreloaded/ProsperoLight` | GPL-3.0 | 生产级 `native_agc_present.cpp`（预编译 shader blob 路线） |
>
> 所有四个项目都是 **GPL-3.0**，与 wiliwili（GPL-3.0）兼容，**代码可借鉴**。
> （GPL-2.0 的 shadPS4 / sharpemu / AnyPS5 / prosperity 只能读，本笔记未引用其代码。）

---

## 0. 结论（先看这个）

1. **最小上屏 = 一条 pipeline 都没有也能先验证一半。** 分两步做：
   - **第 0 步（不需要 shader）**：`sceAgcInit` + `sceVideoOutOpen` + 注册 2 个 buffer + **CPU 直接往扫描缓冲写纯色** + `sceAgcDcbSetFlip` + submit。这一步证明「直内存映射→VideoOut 注册→GPU flip→面板」这条链通了。
     - 先例：`ps5link-sdk/examples/gpu_cube/main.c:503-505`（`for (unsigned long i = 0; i < n; i++) p[i] = 0xFF201828u;` CPU 填整个扫描缓冲）。
   - **第 1 步（需要 1 条 pipeline）**：用一个**全屏三角形**的 draw 替代 CPU 填充。这是唯一能证明「像素真的由 GPU 写出来」的做法。
     - 先例：`ps5-agc-gears/src/gears_rt_clear.c`（`gears_rt_clear_build` 建 3 个顶点 `{-1,-1,1},{3,-1,1},{-1,3,1}`，`gears_rt_clear_compose` 只发 1 个 SH-direct + 1 个 DrawIndexAuto）+ `docs/ARCHITECTURE.md`（原文：`gears_rt_clear` **replaces the color-buffer DMA fill with an oversized triangle at the far clip plane**）。
2. **最小 pipeline 数量 = 1（一个 VS + 一个 PS）。** 清屏**需要 shader**：在我们可用的 19 个 `sceAgc*` 导入符号里没有任何「CB 清屏」调用（§4.1），而四个参考实现**无一例外**用「全屏/超大三角形 + 写常量色的 PS」做颜色清屏；只有 **depth/stencil** 走 CP DMA fill（`sceAgcDcbDmaData`），而那条导入符号**不在当前 SDK 桩里**（见 §4.2）。
   - `ps5-agc-gears/docs/HARDWARE_VALIDATION.md`：`Color clear: render-target draw (color_dma=false)` / `Depth clear: DMA (depth_dma=true)`。
3. **初始化必须尽早做完（main 早期），然后把所有权攥在自己手里**：`libSceAgc*`/`libSceVideoOut` 在 self-unjail 换凭证后会 API 失效；`sceVideoOut` 只能有一个 owner，第二次 open 会 panic。
   - `EVO-PLAYER-PS5/docs/hardware/gpu-notes.md:21-25`。
   - 我们原生线目前**不做 self-unjail**，所以这条约束暂时是「未来加 unjail 时的顺序要求」，不是当前阻塞。
4. **我们 native 线的导入面已经够了**：`scripts/ps5/native/native_build.py:237` 已经 `-lSceAgc -lSceAgcDriver -lSceSysmodule`，`--stub-dir` 指向的目录里有 `libSceAgc.so`（19 个符号）/`libSceAgcDriver.so`（5 个符号）/`libSceVideoOut.so`（§4.1 实测枚举）。**最小路径不需要新增任何导入桩。**
5. **最大的单点风险不是寄存器而是缓存一致性**：不做到「init 时 flush ISA/寄存器数组」+「每帧 flush DCB」+「end-of-pipe 两个 cache event」，症状是**提交成功、GPU 也跑了，但电视上是黑屏/上一帧**。
   - `EVO-PLAYER-PS5/docs/evo-pro/agc-bare-metal-ui.md`（"End-of-pipe cache protocol" 与 "Init-time cache flush" 两节）。

---

## 1. 初始化顺序与约束

### 1.1 顺序（照抄即可，每步标注来源）

以 `ps5-agc-gears/native/main.c:511-700`（一个真机跑过 6 万帧的最小实现）为主轴，括号内是其它项目的等价步骤：

| # | 调用 | 来源 / 行 | 备注 |
|---|---|---|---|
| 1 | `sceSysmoduleLoadModuleInternal(0x80000094)` | `ps5-agc-gears/native/main.c:32,517` | gears 显式装载 AGC 模块（`AGC_MODULE = 0x80000094u`）。EVO 在 app-module 里**不显式装载**（`grep sceSysmoduleLoad` 在 `EVO-PLAYER-PS5/projects` 里只有 IME/Videodec）。→ 先试不装载；失败再补。 |
| 2 | `sceAgcInit(&state, sizeof(state))` | `ps5-agc-gears/native/main.c:521` | state 是 8 字节（`uint64_t agc_state;`，`main.c:61`）。同一调用在 `ps5link-sdk/examples/gpu_cube/main.c:385-386`（`sceAgcInit(&agc_state, 8)`）、`ProsperoLight/src/native_agc_present.cpp:1086`（`sceAgcInit(&agc_state, 8)`，static state 在 `:199` 附近声明）、`EVO-PLAYER-PS5/projects/evoplayer/media/src/evo_agc_runtime.c:1097`（`sceAgcInit(8)`，其头文件 `media/include/sce/sce_agc.h:75` 把它写成 `sceAgcInit(uint32_t ring_size)`，**与其它三家不一致，以前三者的 `(state,8)` 为准**）。 |
| 3 | 分配直内存（type 12）+ 映射 prot `0x33` | `EVO .../evo_agc_runtime.c:29-30,980-1000` | `EVO_AGC_DIRECT_MEM_TYPE 12 /* SCE_KERNEL_WB_ONION */`、`EVO_AGC_MAP_PROTECTION 0x33`、对齐 `0x200000`（`:50`）。gpu_cube 用 `PROT_ALL (CPU_RW\|GPU_ALL)=0x33`、最小对齐 16384（`main.c:22-28`（`MEM_TYPE_CACHED_SHARED`/`PROT_*`）与 `:113,124-128`（`DirectMem`/`DIRECT_MEM_MIN_ALIGN`/`direct_alloc`））。**gears 用 `sceKernelBatchMap` 而非 `sceKernelMapDirectMemory`**（`native/main.c` cleanup 段），两者都能用。 |
| 4 | 分配 2 个扫描缓冲（**2 MiB 对齐**） | `gears`: `PS5_SURFACE_ALLOCATION_BYTES` / `PS5_SURFACE_ALIGNMENT`；`gpu_cube/main.c:392-397`（`padded_w/h` + 2 MiB 上取整）；`EVO :1005-1008`（`EVO_AGC_SCANOUT_STRIDE = 0x04000000`） | EVO 有断言式日志 `agc scanout align mis0/mis1` 必须为 0，否则「tiled display surface is shuffled」（`evo_agc_runtime.c` 注册处）。 |
| 5 | `sceAgcGetRegisterDefaults()` | `EVO :1103`、`gpu_cube/main.c:464-467`、`gears/native/main.c`（`ps5_color_select_runtime_defaults` 调用点） | 返回的根结构里 `+0x00` 是 `register** table_cx`、`+0x20` 是 count（`gpu_cube/main.c:465-466`；`gears/include/ps5_agc_registers.h` 用 `_Static_assert(sizeof(struct ps5_agc_register_defaults)==0x40)` 钉死布局）。**所有寄存器复位值必须从这里取，不能硬编码。** |
| 6 | 建 MRT0 颜色目标寄存器块（16 条） | `gears/src/ps5_color_target.c`（`ps5_color_select_runtime_defaults` + `ps5_color_build_target`）；`gpu_cube/main.c:511-545`；`EVO` 的 `setup_color_target` | 细节见 §2.2.1。 |
| 7 | 建 pipeline（shader）：`sceAgcCreateShader` ×2 → `sceAgcLinkShaders(...,4)` | `gears/native/main.c`（`ps5_shader_header_build` → create → link，:`~540-560`）；`EVO :570-590`；`gpu_cube/main.c:420-446` | `4 = triangle list`（`EVO :587` 注释 `/* 4 = triangle list. */`）。 |
| 8 | `sceVideoOutOpen(0xff, 0, 0, NULL)` | `EVO :1234`、`gears/src/ps5_videoout.c:31`（`ops->open(0xff, 0, 0, 0)`）、`gpu_cube/main.c:387`（`SCE_USER_SYSTEM=0xFF, VIDEOOUT_BUS_MAIN=0`） | **只能 open 一次**。 |
| 9 | `sceVideoOutSetFlipRate(handle, 0)` | `EVO :1244`、`gpu_cube/main.c:388`、`gears/src/ps5_videoout.c:34` | 0 = 不限速（由 DCB 的 VSync flip 决定节奏）。 |
| 10 | `sceVideoOutGetResolutionStatus(handle, &st)` | `EVO :1256`（`evo_vo_resolution_status`：`full_width/full_height/pane_width/pane_height/refresh_rate/screen_inches`）；`ProsperoLight/src/native_agc_present.cpp:994` | 用于决定渲染尺寸；**注意** EVO 拿到面板分辨率后会尝试改渲染尺寸，但 VideoOut 只接受白名单模式（SharpProspero 研究结论：**1920×1080 是唯一普遍被接受的模式**，见 `EVO .../sharpprospero-agc-reference.md` 的 `DisplayDevice` 条）。我们**建议直接固定 1920×1080**（与现有 native 线一致）。 |
| 11 | `sceVideoOutSetBufferAttribute2(attr, fmt, tiling=0, W, H, 0, 0, 0)` | `gpu_cube/main.c:412`（`pixelFormat=0x8000000000000000` BGRA8_SRGB，`tilingMode=0` = Tiled）、`EVO :1319`、`gears/src/ps5_videoout.c:41`（`ops->set_attribute(&attribute, plan->format_word, 0u, w, h, 0,0,0)`） | **pitch/option 字段必须留 0**（SharpProspero `DisplayDevice` 结论，EVO 参考文档转述）。 |
| 12 | `sceVideoOutRegisterBuffers2(handle, 0, 0, bufs, 2, attr, 0, NULL)` | `gpu_cube/main.c:413`、`EVO :1323`、`gears/src/ps5_videoout.c:44` | `SceVideoOutBuffers = {void*Data; void*Metadata; void*Reserved0; void*Reserved1;}`（`gpu_cube/main.c:40`）。 |
| 13 | 一次性 cache flush：**ISA/header/link 输出 + 寄存器数组** | `EVO :1226-1227`（`evo_agc_runtime_cache_flush(shader_storage, ...)` + `(gpu_regs, ...)`） | 不做这一步 → 「first DCB containing a draw never retired at all and wedged the GPU」（原文）。 |

**相机/输入/flip 队列**不在最小路径内。

### 1.2 硬约束（每条都有出处）

| 约束 | 出处 |
|---|---|
| **必须先 init AGC/VideoOut，再 self-unjail**（换凭证后 API 失效） | `EVO-PLAYER-PS5/docs/hardware/gpu-notes.md:21-25`：`libSceAgc* / libSceVideoOut go API-dead after the self-unjail credential swap, so the device cannot be lazily brought up on first draw` |
| **`sceVideoOut` 只能有一个 owner，第二次 open 会 panic 主机** | 同上（`A second sceVideoOut open panics the console`）；`EVO .../agc-bare-metal-ui.md` 末段（`dual sceVideoOut ownership panics the console`） |
| **AGC runtime 是 `sceAgc` + flip 队列的唯一 owner** | `EVO .../gpu-notes.md:22-25` |
| 只允许在**注册应用槽**（TITLE_ID + `param.json`，ShadowMountPlus 注册）里跑 | 我们 `run-continuation/ps5-port-status.md`「硬解参考实现」「payload 线的能力边界」两节；gears/EVO 都用 `PPSA99997`/`PPSA99002` 这类注册槽 |
| shader 必须 **AOT**：主机固件不提供运行时 shader 编译器 | `EVO-PLAYER-PS5/docs/hardware/shader-compilation.md` §1：`The console firmware does not provide runtime shader compilers. All shaders must be compiled ahead-of-time (AOT)` |
| `sceAgcDcbSetFlip` 是异步的，**必须等 flip 完成才能重用该缓冲** | `EVO .../agc-bare-metal-ui.md` 表格行：`sceVideoOutSubmitFlip is async and was never waited on; with 2 buffers the CPU clear wiped the live framebuffer` → 修法 `poll sceVideoOutGetFlipStatus until status[3] == flip_arg` |
| DCB 的 `wordCount` ≤ `0xFFFFF` | SharpProspero `Renderer3D` 序列（`EVO .../sharpprospero-agc-reference.md` §3 末） |

### 1.3 `sceAgcInit` 参数的正确形式

三家独立实现一致：**`sceAgcInit(&state, 8)`**，`state` 指向 8 字节（`ps5link-sdk`、`ProsperoLight`、`gears` 的 `sizeof(...)=8`）。
只有 EVO 自己的头文件 `media/include/sce/sce_agc.h:75` 把它声明成 `int32_t sceAgcInit(uint32_t ring_size)` 并调用 `sceAgcInit(8)` —— 这是**同一 ABI 的两种写法**（`state` 可为 8 字节缓冲区），照前三家写。

---

## 2. 命令缓冲（DCB）

### 2.1 DCB 结构体与生命周期

- 结构体（0x38 字节，`bottom/top/up/down/callback/user_data/reserved_dwords/padding`）：
  - 声明：`EVO-PLAYER-PS5/projects/evoplayer/media/include/sce/sce_agc.h:19-28`（`SceAgcCommandBuffer`）。
  - 逐字段构造：`ps5-agc-gears/src/ps5_agc_writer.c` 的 `begin_writer()`；gpu_cube 逐字节镜像了同一布局（`ps5link-sdk/examples/gpu_cube/main.c:85-94`，配 `:96-110` 的 `out_of_space()` 与 `dcb_reset()`）。
  - **`callback` 必须是非空函数指针**（返回 0 = 不处理溢出）；`down` 初始化等于 `top`。
- 一块 DCB 每帧 `reset`（`up = bottom`），写完直接提交整段；**每帧在飞的帧数 = 缓冲数**时，DCB 也要分槽（EVO 用 3 槽 × 2 MB，`:1019-1024`；gpu_cube/gears 用 2 槽）。
- 提交描述符 0x10 字节：`{const uint32_t* words; uint32_t count; uint8_t flag; uint8_t pad[3];}`（`EVO .../sce_agc.h:30-38`；gears `src/ps5_agc_submit.c`：`the zero-flag 0x10-byte DCB descriptor`），然后 `sceAgcDriverSubmitDcb` → `sceAgcSuspendPoint`。

### 2.2 每帧要写的寄存器块（清单 + 顺序）

顺序以 `EVO .../evo_agc_runtime.c` 的 `frame_begin`（`:1588-1660`，视口在 `:1627`、pipeline CX 在 `:1657`）与 `ps5link-sdk/examples/gpu_cube/main.c:498-620` 为模板：

| 序 | 块 | 内容 / 偏移 | 来源 |
|---|---|---|---|
| 1 | 等待扫描安全包 | `sceAgcDriverGetWaitRenderingPacketSizeInDwords()` → `sceAgcDriverWaitUntilSafeForRendering(&cb.up, size, 0, video_handle, buffer_index)` | `EVO :1593-1597`、`gears/src/ps5_agc_writer.c:wait_rendering`、`ProsperoLight/src/native_agc_present.cpp:721-722` |
| 2 | MRT0/CB 颜色目标（16 条 CX） | `sceAgcDcbSetCxRegistersIndirect(cb, mrt, 16)` | `EVO :1601`（`evo_agc_writer_set_target`）、`gears/src/ps5_agc_writer.c:set_indirect` |
| 3 | （可选）DB/stencil 状态（16 条 CX） | `evo_agc_writer_set_cx_indirect(cb, depth_target, 16)` | `EVO :1609` |
| 4 | 视口 + 裁剪 + target mask（14 条 CX，`EVO :1627`） | `0x10F..0x114`、`0x0B4/0x0B5`、`0x2FA..0x2FD`、`0x090`、`0x091`、`0x08E`(=`0xF`) | `gpu_cube/main.c:546-568`；`gears/src/ps5_pipeline.c` 的 `viewport[]`（同 14 条，含注释） |
| 5 | 光栅状态（可选，背面剔除） | `PA_SU_SC_MODE_CNTL` → 上下文偏移 `0x205`，bit0=cull front / bit1=cull back / bit2=front 绕向 | `gpu_cube/main.c:570-590`（含 yScale 为负导致绕向翻转的说明） |
| 6 | LinkShaders 输出（34 条 CX） + UC（3 条） | `sceAgcDcbSetCxRegistersIndirect(cb, cx, N)` / `SetUcRegistersIndirect(cb, uc, 3)` | `EVO :594-600`（`compile_agc_pipeline` 的寄存器计划顺序：linker 34 + pre-raster 10 + pixel 9），`gears/src/ps5_pipeline.c`（`PS5_PIPELINE_LINKED_CX_REGISTERS`），`gpu_cube/main.c:592-600` |
| 7 | 两个 stage 自带的 CX（pre-raster 10 + pixel 9） + SH（6+6） | `sceAgcDcbSetShRegistersIndirect(cb, sh, 12)` | `gears/src/ps5_shader_header.c`（`num_cx_registers = 10 / 9`），`EVO .../evo_agc_shader_header.c:73-91` |
| 8 | 用户数据 / 资源描述符 | `sceAgcCbSetShRegisterRangeDirect(cb, GS_USER_DATA_BASE + slot, words, 4)`（GS base `0x8C`，PS base `0x0C`） | `gpu_cube/main.c:254-255,609-610`；`ProsperoLight :541,576,765`（`0x8c + slot`、`0x0c`） |
| 9 | index buffer + draw | `SetIndexSize(1,0)` → `SetIndexBuffer` → `SetIndexCount` → `DrawIndex`，或 `DrawIndexAuto(4, 2)` / `(3, modifier)` | `gpu_cube/main.c:612-615`；`ProsperoLight :772`（`DrawIndexAuto(&command, 4, 2)` = 四边形 tri-strip） |
| 10 | flip | `sceAgcDcbSetFlip(cb, video_handle, buffer_index, 1 /*VSync*/, flip_arg)` | `gpu_cube/main.c:616`、`ProsperoLight :809`、`gears/src/ps5_agc_writer.c:set_flip` |
| 11 | （可选但推荐）end-of-pipe 缓存协议 | `sceAgcCbReleaseMem`：event 45 + GCR 12（`FLUSH_AND_INV_CB_DATA_TS`），再 event 40 + GCR `0x30c`（`CACHE_FLUSH_AND_INV_TS`） | `EVO .../agc-bare-metal-ui.md`（End-of-pipe cache protocol 一节）；`EVO` 的 `evo_agc_writer_flush_color_target` |
| 12 | flush DCB 字节范围 → `sceAgcDriverSubmitDcb` → `sceAgcSuspendPoint` | `clflush`+`mfence` 只 flush 用到的字节 | `gears/src/ps5_agc_submit.c`（`ps5_agc_submit_checked`）、`EVO :1994-2010` |

**几何包大小的实测契约**（可直接拿来做断言）：`ps5-agc-gears/src/gears_draw_compose.c` 要求**每个 draw 的 SH-direct 恰好 27 DWORD、DrawIndexAuto 恰好 3 DWORD**；`gears_rt_clear.c` 要求清屏 = 27 + 3 = **30 DWORD**。`EVO` 的 `agc-bare-metal-ui.md` 也给出 `dcb_min_presented=2845` 这类下限。

#### 2.2.1 颜色目标 16 条寄存器的具体偏移（两个来源完全一致）

```
0x318, 0x31B, 0x31C, 0x31D, 0x31E, 0x31F, 0x321, 0x323,
0x324, 0x325, 0x390, 0x398, 0x3A0, 0x3A8, 0x3B0, 0x3B8
```

- `gears/src/ps5_color_target.c` 的 `color_offsets[]`（+ `ps5_color_build_target` 的取值逻辑，是最干净的一份实现）。
- `gpu_cube/main.c:468-471`（`kRenderTargetOffsets[16]`）。
- 语义拆解（EVO `agc-implementation.md` §3）：`cx[0]`/`cx[10]` = 目标基址（`addr>>8` / `addr>>40`）、`cx[14] = (h-1) | ((w-1)<<14)`、格式/通道序在 `cx[2]`、tile mode 在 `cx[15]`。

### 2.3 flip 的两种等待方式（选一个）

| 方式 | 用法 | 出处 |
|---|---|---|
| **DCB 内 flip + 轮询** | `sceAgcDcbSetFlip(...)` 然后 `sceVideoOutGetFlipStatus(handle,&st)` 直到 `st[3] == flip_arg`，轮询间 `sceVideoOutWaitVblank` | `EVO` flip 等待段（`:2050-2056`）+ `agc-bare-metal-ui.md` 表格行 |
| **equeue 事件** | `sceVideoOutAddFlipEvent(equeue, handle, 0)` 后用 `sceKernelWaitEqueue` 收事件 | `gears/src/ps5_videoout.c:32-35` + `native/main.c:284` |

两种混用时注意：`gears` 明确「**GPU fence 与 VideoOut token 是两个独立完成信号**，两者都到才可复用该槽」（`docs/ARCHITECTURE.md` 的 Frame ownership 一节）。

---

## 3. 着色器：`.pipe` → ISA → AGC header → CreateShader/LinkShaders

### 3.1 工具链全链（照做即可）

```
projects/evoplayer/shaders/agc/*.pipe           AMD LLPC pipeline 源（GLSL450 + [ResourceMapping]）
  -> amdllpc -gfxip=10.1.3 -o=X.pal.elf          gfx1013 = PS5 GPU
  -> llvm-objcopy --dump-section=.text           取出 .text
  -> llvm-readelf --symbols                      按 _amdgpu_gs_main / _amdgpu_ps_main 切 ISA
  -> llvm-readelf --notes                        取 AMDGPU Metadata (PAL) YAML
  -> tools/build_agc_pipes.py derive()           推导 AGC 寄存器
  -> <name>_pipe.h                               ISA blob + 寄存器表
  -> evo_agc_shader_header.c                     建 0x148 字节 arena
  -> sceAgcCreateShader + sceAgcLinkShaders
```

出处：`EVO-PLAYER-PS5/docs/evo-pro/agc-bare-metal-ui.md`（The shader toolchain 一节）；同一链条在 `docs/hardware/shader-compilation.md` §1 有同样的框图；实现是 `EVO-PLAYER-PS5/tools/build_agc_pipes.py`（608 行，`derive()` 在 `:203`，`resource_mapping_plan()` 在 `:136`）。
`ps5-agc-gears` 有一份**更小、更易读**的同款实现：`tools/build_shader.py`（168 行，注释原文：`Compile the project-owned LLPC pipe and extract its two gfx1013 stages`），ISA 通过 `native/shader_assets.S` 的 `.incbin` 嵌入。

**最简工具链的容器依赖（会踩）**：
- `amdllpc` 编译镜像需要 **`dxc`**（LLPC 的 `gfxruntime` 编译 HLSL advanced-blend 库，无法跳过）；`gpurt` 可以 `-DVKI_RAY_TRACING=OFF` 关掉。
- 构建要 `-j4`、链接 1 个任务：AMDGPU CodeGen 单个 TU 峰值 2-4 GB，每核一个会把 Docker VM 打爆（上游原文：`once taking the whole Docker engine down with it`）。
- 一次构建约 1 小时（`-j4`），可缓存。
- 出处均见 `EVO .../agc-bare-metal-ui.md`（Building the compiler 一节）。

### 3.2 gfx1013 不在公开 AMDVLK 里，必须打补丁

- 现象：`amdllpc -gfxip=10.1.3` → `Invalid gfxip: gfx1013`。
- 原因：`lgc/state/TargetInfo.cpp` 的 `gpuNameMap` 列了 gfx1010/1011/1012 与 1030+，**漏掉 1013**；LLVM 自己的 AMDGPU 后端认识 gfx1013（`llc-18 -march=amdgcn -mcpu=help` 列得出）。
- 补丁：`EVO-PLAYER-PS5/tools/patches/llpc-add-gfx1013.py` —— 克隆 `setGfx1011Info`（Navi12，最接近的 GFX10.1 兄弟，含区别于 1010 的 integer-dot 能力）为 `setGfx1013Info`，并在 `gfx1012` 那行后面注册 `{"gfx1013", "Navi10Lite", &setGfx1013Info}`。该脚本自带 `grep -q` 断言，静默失败会产出无编译器的镜像。
- **注意**（上游原文）：如果某个 shader 编译出来有「像是少了硬件 errata workaround」的味道，第一个该看的就是这张克隆出来的表。

### 3.3 PAL 元数据 → AGC 寄存器（关键推导，`build_agc_pipes.py:203` 的 `derive()`）

| AGC 寄存器 / 字段 | 推导来源 | 备注 |
|---|---|---|
| `VGT_ESGS_RING_ITEMSIZE`（CX `0x2AB`） | `graphics[".vgt_esgs_ring_itemsize"] & 0x7FFF` | **顶点源 NGG 必须是 1**。历史事故：写 4 → 第一个真 draw 的 DCB 把 GPU 卡死，`sceAgcSuspendPoint()` 不返回，OS 约 55 s 后杀进程。 |
| `DRAW_MODIFIER` | `bit(has BaseVertex) \| (bit(BaseInstance)<<2) \| (bit(DrawIndex)<<3)`，三个 bit 由 `gs.user_data_reg_map` 是否含 `0x10000003/4/5` 决定 | gpu_cube 打印 `user_dwords` / `draw_modifier`，EVO 日志同款 |
| `SPI_SHADER_PGM_RSRC1_GS/PS` | `rsrc1(stage, wave32, component, gs_stage)`：`vgprs \| (sgprs<<6) \| (192<<12) \| (1<<21) \| (1<<25)`，GS 另加 `wgp_mode<<27 \| (component<<29)` | `build_agc_pipes.py:203` |
| `SPI_SHADER_PGM_RSRC2_GS/PS` | `(user_sgprs & 0x1F) << 1`（GS 另加 `es_vgpr_comp_cnt << 16`） | 同上 |
| `SPI_PS_INPUT_CNTL_0..N` | `.spi_ps_input_cntl[]`：`offset \| default_val<<8 \| flat_shade<<10 \| ... \| attr0_valid<<24 \| attr1_valid<<25` | **不能放进 arena**（pixel stage CX 槽位固定 9），必须作为 bind-time CX 追加，见 §3.4 |
| `SPI_PS_INPUT_ENA`/`ADDR`（`0x1B3`/`0x1B4`） | `ps_inputs()` 按 16 个 `_ena` 字段打包 | 同上 |
| `SPI_VS_OUT_CONFIG`（`0x1B1`） | `(vs_export_count & 0x1F) << 1 \| no_pc_export<<7` | 事故记录：留 0 → 参数缓存分配太小，多余的 varying 变成逐三角形变化的垃圾（对角线楔形） |
| `GE_CNTL` / `VGT_SHADER_STAGES_EN` / `VGT_GS_OUT_PRIM_TYPE` | `ge_cntl` / `stage_word` / 常量 `2` | 写进 shader arena 的 specials（§3.4） |
| 用户 SGPR 槽位 | `user_data_reg_map` 里 `0x10000000`=GlobalTable（驱动供）、`0x1000000F`=VertexBufferTable（`IndirectUserDataVaPtr`，PAL special）、普通 `DescriptorTableVaPtr` 节点按 stage 内声明顺序编号 | 事故记录：**槽位必须推导，不能猜**——猜错的表现是「draw 提交了、也没 fault，但一个片元都没写出来」 |

配置文件里有 `[ResourceMapping]` 与 shader 源码并排，**这是这套工具链相对「手写 wrapper」的全部理由**：漏掉 vertex stage 的 descriptor binding 在 `.pipe` 形式里根本表达不出来（历史事故 #2：`layout(set=0,binding=0)` 的 uniform 从没人描述的槽位读 → 0 片元）。

### 3.4 shader header arena（0x148 字节，硬 ABI）

- 布局断言（两份独立实现完全一致）：`ps5-agc-gears/src/ps5_shader_header.c`（`user_data@0x60 / specials@0x98 / cx@0xc8 / sh@0x118 / sizeof==0x148`）与 `EVO .../evo_agc_shader_header.c:8-19`（同一组 `_Static_assert`）。`struct ps5_shader_header == 0x60`、`code@0x10`、`type@0x5a`（`ps5-agc-gears/include/ps5_shader.h`）。
- 关键字段：`file_header = 0x34333231`（`"1234"`）、`version = 0x18`、`target = 5`、`num_sh_registers = 6`、`type = PRE_RASTER / PIXEL`、`num_cx_registers = 10 / 9`。
- **指针字段是「自相对」的**（`self_relative()`）：每个指针存「从自己到目标的距离」，所以整块可以 memcpy 进 GPU 内存；`sceAgcCreateShader` 会**就地**把它们解析成绝对指针，并把 arena 本身作为 handle 返回。
  - 出处：`ps5-agc-gears/src/ps5_shader_header.c` 的 `self_relative`；`EVO` 同函数名 + 注释 `The header's pointer fields are stored SELF-RELATIVE ... which is why evo_agc_runtime.c can read cx_registers/sh_registers straight off the object afterwards`。
  - **两个项目都断言 `handle == arena`**：不等就是 header 被拒（`EVO :573-583`、`gears/native/main.c` create 段）。这是最省事的 header 正确性检查。
- SH 寄存器 6 条：
  - PS：`0x006`(PGM_LO)、`0x006`(PGM_HI)、`0x008`、`0x009`、`0x00A = ps_rsrc1`、`0x00B = ps_rsrc2`；
  - pre-raster：`0x080`、`0x080`、`0x08A = gs_rsrc1`、`0x08B = gs_rsrc2`、`0x0C8`、`0x0C9`。
  - PGM_LO/HI 留 0，由驱动在 CreateShader 时填代码地址 → **`gs_pgm`/`ps_pgm` 非 0 就是「驱动认了这个 shader」的证据**。
- ISA 末尾要补 **0x30 字节 footer**，并写入 8 字节魔法 `"barefoot"`：`ps5-agc-gears`（`SHADER_FOOTER_BYTES = 0x30`，`memcpy(gs_code + gs_size - SHADER_FOOTER_BYTES, "barefoot", 8u)`）与 `EVO .../evo_agc_shader_header.c` 的 `evo_agc_shader_write_code()`（注释：`sceAgcCreateShader looks for this marker, which only the PSSL toolchain emits; amdllpc's raw ISA has no trailer, so synthesise it the way the reference implementation does`）。

### 3.5 最小需要几条 pipeline？（问题的直接答案）

| 目的 | 需要的 pipeline | 依据 |
|---|---|---|
| 纯色清屏 / 全屏上色 | **1 条**（VS + PS） | `gears` 只有一条 pipeline，同时用于清屏与三个齿轮：`native/main.c` 里 `ps5_shader_header_build` 只建一对 arena，`ps5_pipeline_build` 为两个 buffer 各建一份寄存器计划；`docs/ARCHITECTURE.md`：`gears_rt_clear ... reuses the independently authored gfx1013 pipeline` |
| 一个四边形（顶点缓冲 + 索引 + 常量缓冲） | 仍是 **1 条** | `ps5link-sdk/examples/gpu_cube/main.c`：全程序只有 `mesh_vs_sb` + `mesh_ps_sb` 两个 shader |
| UI（文字/图标/圆角/混合） | 至少再多 1 条带纹理 + 顶点色的 2D pipeline | `EVO` 的 `ui_screen_2d.pipe`（`docs/hardware/shader-compilation.md` 的 Active Shaders 表）；`EVO` 原文：`Only the UI pipeline is mandatory at init`（`agc-bare-metal-ui.md`） |

**清屏需要 shader**：AGC 有 `sceAgcCbDispatch`（compute）、`sceAgcDcbAcquireMem`、`sceAgcCbReleaseMem`，但**没有公开的「CB 清屏」调用**；参考实现的颜色清屏一律是 draw（`gears` 的 `color_dma=false`），只有 depth/stencil 用 `sceAgcDcbDmaData` 的 CP fill（`gears` 的 `depth_dma=true`）。

### 3.6 教训：手写 ISA / 手搓寄存器会挂

- **纹理 PS 过不了 `sceAgcCreateShader` 校验（`0x8a6c001f`）**：Sony 的编译器会 emit 一段 `sl00` resource-metadata trailer，手写不出来 ⇒ 「纹理 UI 作为 GPU 几何」在这条路上被卡死。转机是 `.pipe` + amdllpc。
  - `EVO-PLAYER-PS5/docs/hardware/gpu-notes.md`（History 一节）。
- 更早的 `llvm-mc` 手搓路线（`EVO .../agc-implementation.md` §1）只能复用 ProsperoLight **预编译**的 `pixel.header.bin`，并只换 `.text`（靠「RGBA-passthrough PS 与 NV12 PS 输入签名相同、资源更少」这条侥幸）。**不要走这条路。**
- 反例参考：`ps5link-sdk/examples/gpu_cube` 直接内嵌 SharpProspero 的 `.sb` 容器（`shaders/build.sh` 产出 `mesh_vs_sb.h`/`mesh_ps_sb.h`），运行时用 `shader_parts_from_elf()` 解析 `.shader_header` / `.shader_text` 两个 section（`main.c:214` 的 `shader_parts_from_elf()`）。如果需要**立刻**跑起来而不先搭 amdllpc 镜像，这是唯一现成的 blob 来源（容器格式见 `EVO .../sharpprospero-agc-reference.md` 的 `ShaderBinary.cs` 条：header 里 magic `0x34333231`、program-type @90、ctx-reg-count @91、sh-reg-count @92）。
  注意：`ps5-xash3d-halflife` 只发布源码、**不发布编译好的 shader**，只能作结构参考（`EVO .../agc-bare-metal-ui.md` 原文）。

---

## 4. 导入桩清单与本地 native 线的衔接

### 4.1 我们当前**实际可用**的导入符号（实测枚举，非推测）

`scripts/ps5/native/native_build.py:369-374` 的默认 `stub_dir` 是
`ps5-native/ps5-opengl/build/sdl-folder/.deps/native/ps5-payload-sdk/target/lib/`，
其中：

**`libSceAgc.so` —— 19 个符号**（`nm -D --defined-only`）：

```
sceAgcInit                  sceAgcGetRegisterDefaults   sceAgcCreateShader
sceAgcLinkShaders           sceAgcSuspendPoint           sceAgcDcbSetCxRegistersIndirect
sceAgcDcbSetShRegistersIndirect  sceAgcDcbSetUcRegistersIndirect
sceAgcCbSetShRegisterRangeDirect sceAgcCbReleaseMem      sceAgcCbDispatch
sceAgcDcbAcquireMem         sceAgcDcbSetFlip             sceAgcDcbSetIndexSize
sceAgcDcbSetIndexBuffer     sceAgcDcbSetIndexCount       sceAgcDcbDrawIndex
sceAgcDcbDrawIndexAuto      sceAgcDcbSetNumInstances
```

**`libSceAgcDriver.so` —— 5 个符号**：

```
sceAgcDriverSubmitDcb   sceAgcDriverGetWaitRenderingPacketSizeInDwords
sceAgcDriverWaitUntilSafeForRendering   sceAgcDriverGetTFRing   sceAgcDriverSetTFRing
```

**`libSceVideoOut.so`** 是 payload SDK 的完整表（含 `sceVideoOutOpen/Close/SetFlipRate/SetBufferAttribute2/RegisterBuffers2/GetResolutionStatus/GetOutputStatus/SubmitFlip/GetFlipStatus/WaitVblank/AddFlipEvent/...`）。

> 这 19 + 5 个符号**足够**跑通「清屏 + 一个四边形 + 一次 flip」的全部必需调用（§2 的清单里唯一用到但缺失的是 `sceAgcDcbDmaData`，见下）。

### 4.2 缺失符号：`sceAgcDcbDmaData`（以及怎么补）

- 参考实现用它做 **CP 同步填充**（清 depth/stencil、填 L2）：
  - `gears/include/ps5_agc.h:45-53`（`ps5_agc_dcb_fill_l2_sync(writer, gpu_destination, repeated_word, byte_count)` → `sceAgcDcbDmaData(writer, 0, 3, 0, dest, 2, 0, word, bytes, 0, 0, 1)`，`dst_select=3` + `cp_sync=1`）；
  - `EVO` 清 stencil：`sceAgcDcbDmaData(&cb, 0u, 3u, 0u, stencil_base, 2u, 0u, 0u, 4MB, 0u, 0u, 1u)`（`evo_agc_runtime.c:1612`）。
- **最小路径不需要它**（没有 depth buffer、颜色清屏走 draw），所以**第 0/1 步不受影响**。
- 将来需要时，补齐方式（**不需要手填 NID**）：
  1. 往 `ps5-native/ps5-opengl/native-app/agc_link_stub.c` 里加同名函数（该文件现在覆盖 19 个 `sceAgc*`，含 `sceAgcDcbAcquireMem`/`sceAgcCbDispatch`/`sceAgcDcbSetNumInstances`/`sceAgcCbReleaseMem`）；
  2. 重新生成桩 `.so`：`prospero-clang18 -std=c11 -O2 -fPIC -c agc_link_stub.c -o agc_link_stub.o` → `prospero-lld --shared -soname libSceAgc.prx -o <stub_dir>/libSceAgc.so agc_link_stub.o`（照抄 `ps5-opengl/tools/build-native-test-app.sh:205-222`）；
  3. 链接时直接引用该符号即可 —— 转换器会**按算法算出 NID**：
     `tooling/native/sce_module_writer.cpp` 的 `nid()`（`:193-214`）= `SHA1(符号名 + 固定 16 字节后缀)` 取前 8 字节**反转** → 11 字符 base64（`/`→`-`）；`imports.push_back({symbol.name, provider, ...})`（`:716-717`）要求 **stub 目录里必须有某个 `.so` 导出该符号**，否则直接 `require` 失败。
- 同样的方法可以补 `sceAgcDcbSetCxRegisterDirect` / `SetShRegisterDirect`（gpu_cube 声明但未用；`ps5link-sdk/examples/gpu_cube/main.c:62-63`）——**不推荐**，indirect 形式更省命令且是参考实现的主流。

### 4.3 与现有 `app-symbols.map` / 链接方式的衔接（现状，无需改动）

- 链接配方不改：`scripts/ps5/native/native_build.py:236-238` 已经在 `payload_libs` 里带 `-lSceAgc -lSceAgcDriver -lSceSysmodule`，`archives` 后面用 `--start-group ... --end-group` 包住。
- **符号可见性**由 `scripts/ps5/native/app-symbols.map`（`{ local: *; };`）负责：它把所有**导出**藏起来（PS5 模块转换器只发布 import，不支持应用导出），**不影响** `sceAgc*` 这种 UND 导入。所以「加 AGC」不需要动这个 map。
- AGC 是**导入**（不是本地定义）：`native_build.py:177-180` 的注释明确 —— 模板里那三个本地空桩（`agc_link_stub.c` / `agc_driver_link_stub.c` / `video_out` 相关）**绝不能编进构建**，否则「every GPU submission turns into a no-op, which renders the application invisible while every GL call still reports success」。当前 `compile_runtime_objects()` 只编 `app_heap.c`，**符合要求**。
- 结论：**最小 AGC 上屏在本仓库不需要任何构建系统改动**（前提是用它自己的 shader 输入与 `sceVideoOut*` 调用）。

### 4.4 与 SDL / VideoOut 归属的冲突（必须提前决定）

这是**本任务最大的集成决策点**：

- 我们 native 线的 SDL 平台层是 ps5-opengl 的 **`ps5-g19` 桥**（走 EGL/AGC），它**自己** `sceVideoOutOpen` + 注册缓冲 + flip（`run-continuation/ps5-port-status.md`：「payload 线 SDL ps5 驱动就是 `sceVideoOutOpen`/`RegisterBuffers2`/`SubmitFlip`」，native 线用同一族桥）。
- 而 AGC 侧**必须**成为 `sceAgc` + flip 队列的**唯一 owner**，第二个 VideoOut open 会 **panic 主机**（§1.2）。
- ⇒ 两条互斥选项：
  1. **独立探针标题（推荐做第 0/1 步）**：新 TITLE_ID（如 `PPSA99013`），`main()` 里只有 AGC + VideoOut + 一个 draw + flip，**不链接 SDL/EGL/Mesa**。这样零冲突，且能把「GPU 是否真的上屏」与 wiliwili 的渲染栈彻底隔离开。
  2. **接管 SDL 的呈现**：让 SDL/GL 那条路不再 open VideoOut，把 AGC runtime 作为唯一呈现者（EVO 的做法：`Only one present path can exist at a time`）。这是后期「给 borealis 写 AGC 后端」的形态，工作量大得多。

---

## 5. 最小可跑草图（伪代码）与验证点

### 5.1 伪代码（第 0 步 + 第 1 步合并，单文件即可）

```c
/* ---- 一次性 init（main 早期；不早于/不晚于 self-unjail 的规则见 §1.2） ---- */
sceSysmoduleLoadModuleInternal(0x80000094);           /* 失败可忽略，先试 */
static uint64_t agc_state;
rc = sceAgcInit(&agc_state, 8);                       /* rc==0 必须 */

/* 直内存：一块 0x4000000，type 12，prot 0x33，2 MiB 对齐 */
phys = sceKernelAllocateDirectMemory(0, 16GB, TOTAL, 0x200000, 12, &off);
sceKernelMapDirectMemory(&base, TOTAL, 0x33, 0, off, 0x200000);
/* 切分：fb[0] (0x4000000) | fb[1] (0x4000000) | shader_storage | dcb[0] | dcb[1] */

defaults = sceAgcGetRegisterDefaults();               /* 非空必须 */
ps5_color_select_runtime_defaults(cx_default, defaults);   /* gears 的取法 */
for (i = 0; i < 2; i++) ps5_color_build_target(rt[i], cx_default, fb_addr(i), 1920, 1080);

/* shader：从 *_pipe.h 拿 ISA + 寄存器表（§3） */
build_arena(&gs_arena, PRE_RASTER, gs_bytes, &meta);  write_code(gs_code, gs_isa); memcpy(gs_code+gs_isa, "barefoot", 8);
build_arena(&ps_arena, PIXEL,      ps_bytes, &meta);  write_code(ps_code, ps_isa); memcpy(ps_code+ps_isa, "barefoot", 8);
assert(sceAgcCreateShader(&gs, &gs_arena, gs_code) == 0 && gs == &gs_arena);
assert(sceAgcCreateShader(&ps, &ps_arena, ps_code) == 0 && ps == &ps_arena);
sceAgcLinkShaders(linked_cx /*34*/, linked_uc /*3*/, NULL, gs, ps, /*tri list*/ 4);

/* VideoOut：唯一 owner */
vh = sceVideoOutOpen(0xff, 0, 0, NULL);               /* > 0 */
sceVideoOutSetFlipRate(vh, 0);
sceVideoOutGetResolutionStatus(vh, &res);             /* 记录用；我们固定 1920x1080 */
SceVideoOutBuffers bufs[2] = { {fb[0],0,0,0}, {fb[1],0,0,0} };
sceVideoOutSetBufferAttribute2(attr, 0x8000000000000000ULL, /*Tiled*/0, 1920, 1080, 0,0,0);
assert(sceVideoOutRegisterBuffers2(vh, 0, 0, bufs, 2, attr, 0, NULL) == 0);

/* 一次性 cache flush：ISA + link 输出 + 所有寄存器数组（type 12 是 write-back） */
clflush_range(shader_storage, used); clflush_range(gpu_regs, sizeof(gpu_regs));

/* ---- 每帧 ---- */
slot = frame & 1;
/* 第 0 步：CPU 填色（证明扫描链）；第 1 步删除这行，改成 draw */
for (n = 0; n < 1920*1080; n++) ((uint32_t*)fb[slot])[n] = 0xFF201828u;

dcb_reset(&dcb[slot], dcb_buf[slot], cap_dwords);
wait_size = sceAgcDriverGetWaitRenderingPacketSizeInDwords();
sceAgcDriverWaitUntilSafeForRendering(&dcb->up, wait_size, 0, vh, slot);
sceAgcDcbSetCxRegistersIndirect(&dcb, rt[slot], 16);
sceAgcDcbSetCxRegistersIndirect(&dcb, viewport14, 14);          /* 0x10F.. + 0x090/0x091 + 0x08E=0xF */
sceAgcDcbSetCxRegistersIndirect(&dcb, cx_pipeline, cx_count);   /* 34 + 10 + 9 (+ ps_input_cntl) */
sceAgcDcbSetUcRegistersIndirect(&dcb, linked_uc, 3);
sceAgcDcbSetShRegistersIndirect(&dcb, sh_combined, 12);
sceAgcCbSetShRegisterRangeDirect(&dcb, 0x8C + cb_slot, cb_words, 4);   /* GS base 0x8C */
sceAgcCbSetShRegisterRangeDirect(&dcb, 0x8C + vb_slot, vb_words, 4);
sceAgcDcbDrawIndexAuto(&dcb, /*3=全屏三角*/ 3, draw_modifier);         /* 或 SetIndexSize/Buffer/Count + DrawIndex */
sceAgcCbReleaseMem(&dcb, ...);                                        /* §2.2 第 11 项，两个 event */
sceAgcDcbSetFlip(&dcb, vh, slot, /*VSync*/1, flip_arg);
clflush_range(dcb->bottom, (dcb->up - dcb->bottom) * 4);
submit = { dcb->bottom, dcb->up - dcb->bottom, 0 };
assert(sceAgcDriverSubmitDcb(&submit) == 0);
sceAgcSuspendPoint();                                  /* 返回值要看 */
do { sceVideoOutWaitVblank(vh); } while (flip_status[3] != flip_arg);   /* flip 完成才能复用该缓冲 */
```

### 5.2 必须自己决定的几个常量（都有参考出处，别猜）

| 量 | 建议值 | 出处 |
|---|---|---|
| VideoOut 像素格式 | `0x8000000000000000`（BGRA8 sRGB） | `gpu_cube/main.c:48`、`EVO :121` |
| VideoOut tiling 参数 | `0`（Tiled） | `gpu_cube/main.c:45`（`VIDEOOUT_TILING_TILED = 0`）、`EVO :1319` |
| CB 通道序 | `ChannelOrder = kAlt`（`RT_SET(2, 0x00001800u, 0x800u)`）+ `COMP_SWAP=ALT` | `gpu_cube/main.c:520`、`EVO` 的 `comp_swap=1` 日志（颜色互换事故） |
| CB 格式 | `8_8_8_8` + `UNorm` | `gpu_cube/main.c:518-519`、`gears/src/ps5_color_target.c`（`0x00008028` 掩码） |
| `CB_TARGET_MASK` | `0x0000000F` | `gpu_cube/main.c:567`、`gears/src/ps5_pipeline.c` |
| 视口缩放/偏移 | `xs=W/2, xo=W/2, ys=-H/2, yo=H/2`（**y 负**） | `gpu_cube/main.c:549-556`、`gears/src/ps5_pipeline.c` |
| 扫描缓冲对齐 | 2 MiB（`0x200000`） | `gpu_cube/main.c`（`2*1024*1024`）、`EVO :50`、gears `PS5_SURFACE_ALIGNMENT` |
| 直内存 type / prot | `12` / `0x33` | `EVO :29-30` |
| 图像描述符对齐 | **256 字节**（描述符存 `address>>8`） | `EVO .../agc-bare-metal-ui.md`（`textures were 64-byte aligned ... needs 256`） |
| `draw_modifier` | 由 PAL 推导（§3.3），**不要写死** | `build_agc_pipes.py:203`、`gpu_cube/main.c` 的 `notify_vals("chk1", ..., "vscx", "pscx")` |

### 5.3 验证点（怎么判断「真的上了 GPU」）

**A. 初始化阶段的硬证据**

| 检查 | 期望 | 依据 |
|---|---|---|
| `sceAgcInit` 返回 0 | `rc == 0` | `gears/native/main.c:521` |
| `sceAgcCreateShader` 返回的 handle **等于**传入的 arena 指针 | `handle == arena` | `EVO :573-583`、`gears/native/main.c` create 段 |
| `gs_pgm` / `ps_pgm` **非 0**（SH 数组里的 PGM_LO/HI） | 非 0 | `EVO .../agc-bare-metal-ui.md` 验证段（`gs_pgm / ps_pgm non-zero — sceAgcCreateShader resolved the entries`） |
| `comp_swap == 1` | 是 | 同上（`Without it the whole UI is R-B swapped`） |
| `sceAgcSuspendPoint()` 返回 0 | 0 | `EVO :2006-2010`（失败会打印 `agc health ... timeouts`） |

**B. 帧循环阶段（区分三种「像成功」的假象）**

| 现象 | 说明 | 用什么区分 |
|---|---|---|
| CPU 填色上屏、帧时间 ≈ 16.6 ms（VSync 限速） | 只证明 **VideoOut+flip 链通**，**不能**证明 GPU | `flip_waits == presents`、`dcb_min_presented` 之类的计数（`agc-bare-metal-ui.md` 验证段） |
| 画面**动画**（旋转/相位变化） | 只有 GPU draw 才会随 `flip_arg`/常量变化 | `gpu_cube` 的旋转立方体；静态纯色无法区分 GPU 与 CPU |
| draw 提交了但全黑 | 典型是「shader 读了没人设的指针」或缓存没 flush | 看 `VGT_ESGS_RING_ITEMSIZE==1`、看 user-SGPR 槽位推导日志、看 init-time flush 是否做了 |
| 提交成功、GPU 跑了、屏幕是上一帧/黑 | end-of-pipe cache 协议缺失 | 加 `ReleaseMem` 两个 event（§2.2 第 11 项） |
| GPU 卡死、约 55 s 后进程被杀 | DCB 里有非法 draw（历史：`VGT_ESGS_RING_ITEMSIZE=4`） | `sceAgcSuspendPoint()` 不返回 / 看门狗 |

**C. 定量 receipt（对照用）**

| 指标 | 数值 | 出处 |
|---|---|---|
| gears 命令组装 | ≈ 2.2 µs | `ps5-agc-gears/docs/HARDWARE_VALIDATION.md`（Timing interpretation） |
| gears GPU wait | ≈ 1.1 ms | 同上 |
| gears VideoOut wait | ≈ 15.56 ms（= VSync 节奏） | 同上 |
| gears 连续运行 | 12,020 帧 / 60,000 帧 soak，`renderer errors = 0`，guards 完好 | 同上（Continuous / Strict 60,000-frame 两节） |
| EVO 4K 视频 convert+flip | **982 µs/帧**（≈ 30 fps 预算的 3%） | `EVO .../agc-implementation.md` 头部 |

**D. 回读（最硬的证据）**：`EVO` 的 `agc_dump_scanout()` —— 在第 40 帧把扫描缓冲读回，打印亮度缩略图与主要颜色。它当初就是靠这个把「缓冲里是 `ffedbe00`（正确的青）而面板显示金色」定位成通道序问题的。出处：`agc-bare-metal-ui.md`（Verifying a build 一节）。

---

## 6. 风险与工作量

### 6.1 风险表

| 风险 | 症状 | 触发条件 | 缓解 |
|---|---|---|---|
| **缓存一致性**（最高） | 提交成功但黑屏/上一帧/GPU 卡死 | init 时没 flush ISA+寄存器数组，或每帧没 flush DCB，或缺 end-of-pipe 协议 | 照 §1.1 第 13 步 + §2.2 第 12 步 + §2.2 第 11 步 |
| **手写 shader header / ISA** | `sceAgcCreateShader` 返回错误（纹理 PS 常见 `0x8a6c001f`）或 draw 产出 0 片元 | 想省掉 amdllpc 镜像 | 必须用 amdllpc + `.pipe`；或直接复用 SharpProspero 的 `.sb` blob |
| **PAL 推导写死** | 0 片元 / 对角线垃圾 | 槽位或 `SPI_VS_OUT_CONFIG` 猜错 | 全部读 `derive()` 输出；运行时读 layout 而不是硬编码 |
| **VideoOut 双 owner** | **主机 panic** | AGC 与 SDL/EGL 同时 open | 探针独立标题；或让 GL 侧不再 open（§4.4） |
| **`/system_ex` 空间** | 部署静默失败 | eboot/镜像过大 | 走 ffpkg 到 `/data/homebrew`（我们已有链路，见 status doc） |
| **`sceAgcDcbDmaData` 不在桩里** | 链接错误 | 想用 CP fill 清 depth/stencil | 按 §4.2 加 stub 符号（NID 自动推导） |
| **amdllpc 镜像构建** | 数小时、内存打爆 Docker | 一次构建 | `-j4` + 链接任务串行；`dxc` 必须装 |
| **颜色 R↔B 互换 / 范围偏移** | 画面能看但颜色错 | VideoOut 属性与 CB 寄存器不是「一套」 | 抄 §5.2 那一组配对；用回读定位（§5.3 D） |
| **wiliwili 渲染层是 nanovg/GL3** | 不能直接复用 | 目标是「用 GPU 跑 UI」 | 这是**重写渲染层**量级的工作，见 §6.2 第 3 档；不要与「先点亮 GPU」混在一个任务里 |

### 6.2 工作量估计（AI 协助 + 真机往返的实测口径）

我们 native 线已有基础：注册应用槽能跑（`PPSA99010/12`）、能启动、能部署（ffpkg/FTP/UDP 日志/`download0` 取日志），这省掉了参考项目里最耗时的 1-3 天。

| 档 | 内容 | 估计 | 说明 |
|---|---|---|---|
| **D0** 探针标题：CPU 填色 + flip | 独立 `PPSA99013`，无 SDL/EGL/Mesa；把 §5.1 第 0 步做出来 | **8-16 人时（1-2 人天）** | 大部分是直内存/VideoOut/DCB 样板；`gpu_cube` 可直接当模板抄 |
| **D1** 第一个 GPU draw（全屏三角清屏） | 需要 shader ⇒ 必须先有 amdllpc + gfx1013 补丁 | **24-40 人时（3-5 人天）** 其中 **4-8 人时**是机器时间（LLPC 构建 ~1 h）与调试 | 含：搭 `amdllpc` 镜像、跑通 `.pipe`→ISA→arena、CreateShader/LinkShaders、draw 上屏、颜色正确 |
| **D2** 一个四边形（顶点缓冲 + 索引 + 常量缓冲 + SRD） | §2.2 第 8/9 项的完整版 | **16-32 人时（2-4 人天）** | `gpu_cube` 已给出可照抄的 SRD/描述符与 viewport 块；难点是 `user_data_reg_map` 槽位（§3.3 事故 #2） |
| **D3** 接进 wiliwili（给 borealis 写 AGC 后端） | 图元：矩形/圆角/纹理/文字图集/混合/裁剪；替换 nanovg+SDL 呈现 | **240-480 人时（30-60 人天）**及以上 | 参考项目的一致判断：AGC 不是 GL，必须**重写渲染层**（`EVO` 自写 RmlUi 渲染器 + 纹理/图集/裁剪遮罩；其 shader 集 = `ui_screen_2d` + backdrop-blur + 4 条视频 pipeline）。这属于独立立项，不在「点亮 GPU」范畴 |
| **D4** 视频走 AGC（NV12→RGB + OSD 合成） | 与硬解配合 | 另计（依赖 05 号硬解笔记） | `EVO`/`ProsperoLight` 各有一条完整实现；wiliwili 侧还要解决 mpv 不接受外部帧的问题 |

**建议的节奏**：D0 → D1 先做（合计约 **4-7 人天**），产出「GPU 真的上屏了」的确定性结论与可复用骨架；D2/D3 是否投入，等 D1 的真机 receipt（帧时间、`gs_pgm/ps_pgm`、回读颜色）出来再定。

---

## 附录 A — 许可证

本笔记引用的全部代码/文档来自 **GPL-3.0** 项目（`ps5link-sdk`、`ps5-agc-gears`、`EVO-PLAYER-PS5`、`ProsperoLight`），与 wiliwili（**GPL-3.0**）兼容 ⇒ **可以复制代码**（需保留版权与许可声明）。
本笔记未引用任何 GPL-2.0-only 项目（shadPS4 / sharpemu / AnyPS5 / prosperity）的代码。

`ps5link-sdk/shaders/third_party/sharpprospero/` 下的 blob 来自 `SvenGDK/SharpProspero`，其 `LICENSE` 随该目录一起提供（引用前请确认）。

## 附录 B — 本地可直接复用的东西（现状核对）

| 需要的东西 | 本地位置 | 状态 |
|---|---|---|
| AGC 导入 | `scripts/ps5/native/native_build.py:237`（`-lSceAgc -lSceAgcDriver -lSceSysmodule`） | ✅ 已有 |
| AGC 桩 `.so` + `prx.script` | `ps5-native/ps5-opengl/build/sdl-folder/.deps/native/ps5-payload-sdk/target/lib/` | ✅ 有（19+5 符号，§4.1） |
| 桩源文件（要加符号时改这里） | `ps5-native/ps5-opengl/native-app/agc_link_stub.c`、`agc_driver_link_stub.c` | ✅ 有 |
| 桩 `.so` 重建命令 | `ps5-native/ps5-opengl/tools/build-native-test-app.sh:205-222` | ✅ 有 |
| NID 自动推导 | `ps5-native/ps5-native-app-boilerplate/tooling/native/sce_module_writer.cpp:193-214,699,716` | ✅ 有 |
| VideoOut 导入 | payload SDK `libSceVideoOut.so`（完整表） | ✅ 有 |
| 部署/日志链路 | `scripts/ps5/native/{deploy-native,log-listen.py,app-log.sh,launch-native.sh}` | ✅ 已有（见 status doc） |
| 一个可照抄的最小 AGC 单文件 | 需自取：`Rufidj/ps5link-sdk` → `examples/gpu_cube/main.c`（631 行，GPL-3.0） | 外部 |
| shader 工具链（`.pipe`→ISA） | 需自建：`sainsaji/EVO-PLAYER-PS5` 的 `tools/build_agc_pipes.py` + `tools/patches/llpc-add-gfx1013.py`（GPL-3.0） | 外部，需 Docker + amdllpc |
