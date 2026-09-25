# 04 — UI 渲染器：AGC 上的 UI 方案、混合方案可行性与工作量

> 范围：只做调研，不改任何源码。
> 背景前提（已确认，不重复验证）：硬解与 AGC 只在"注册应用槽"（带 TITLE_ID 的 PPSA 标题）内可用；payload（hbldr/elfldr）上下文 `sceVideodec2Decode` 返回 errno 5200、GL 拿不到 surface。
> 现状：wiliwili 的 UI 走 **nanovg(GL3) + Mesa/llvmpipe**，1080p 主界面 `flush` 17.7 ms（约 48 fps），播放页 13-14 fps（见 `run-continuation/ps5-port-status.md`「payload 线帧率」「播放器帧率根因」「已放弃的方向：720p」「已证伪：payload 线的硬件渲染/硬解」四节）。
> 上游调研结论（`ps5-port-status.md`「GPU 渲染参考实现」）：GL/Vulkan 上 GPU 在标题环境是死路，唯一可行的是直接写 `sceAgc` 命令缓冲（裸金属）。

---

## 0. 结论（先看这段）

1. **AGC 上渲染 UI 已被真机验证可行，而且比想象的薄**：EVO-PLAYER-PS5 的 `EvoRenderInterfaceAGC` 在真机上把 RmlUi 界面（菜单/文字/图标/缩略图/圆角裁剪/背景模糊）跑在裸金属 `sceAgc` 上，状态是 **WORKING，2026-09-12 硬件验证**（`docs/evo-pro/agc-bare-metal-ui.md:3`）。它整套 UI 只用了 **1 个 2D pipeline**（`ui_screen_2d`）+ 1 个可选模糊 pipeline。
2. **我们的 nanovg 面比 RmlUi 更简单**：nanovg 的 GL3 后端全部能力由 **1 个顶点着色器 + 1 个片元着色器**（`EDGE_AA` 开/关两个 program）承载，顶点格式只有 `pos.xy + uv.xy`（16 B，`nanovg.h:666-669`），颜色/渐变/scissor 全在 **11×vec4 = 176 B 的常量块**里（`nanovg_gl.h:210-214`）。⇒ 若走 AGC UI，需要自己写的是"nanovg 的 AGC 后端"，不是"新写一个 UI 框架"。
3. **最不确定的一点是 stencil 语义**，不是性能：nanovg 的普通填充是 stencil-then-cover，依赖 `INCR_WRAP/DECR_WRAP`（`nanovg_gl.h:1046-1047`）；EVO 的裸金属运行时只实现了 `KEEP/REPLACE/INCR_CLAMP` 三个（`evo_agc_runtime.c:749`），而 ps5-opengl 的 Gallium→AGC 映射表给出的数值与之不一致（`ps5_screen.c:2322-2331`）。这不是拦路虎，但**必须安排一轮真机确认**（或绕开 stencil，见 §2.2）。
4. **混合方案（视频/背景走 AGC、UI 仍走 llvmpipe）可行**，而且有真机先例：EVO 的 `pp_agc_present` / 新裸金属运行时就是"GPU 画一层、CPU 再往同一张 VideoOut 缓冲上叠一层"的模式（`docs/evo-pro/status.md:205-238`、`agc-bare-metal-ui.md`「Black band creeping down」条）。但 **VideoOut 没有多平面/叠加层能力**：四个 AGC 项目（EVO / ProsperoLight / ps5-agc-gears / ps5link-sdk）全部是"单平面、2 缓冲、自己合成"，没有任何一例用到第二平面或 `RegisterBuffers2` 的 `set_index != 0`（见 §4 证据）⇒ "让硬件替我们合成 UI 与视频"这条路没有 ABI 证据，不要押。
5. **最小增量路径**：先做"**AGC 只画视频四边形，UI 继续 llvmpipe**"，两块叠在同一张 VO 缓冲上。这能让**播放页**（当前 13.7 fps）受益最大，且不需要动 borealis/nanovg 一行绘制代码，也不需要新的 UI 着色器（视频四边形可以直接走 UI pipeline：`pos+uv+采样器`）。**主页这类纯 UI 页面没有视频，混合方案一分钱都拿不到**——那 17.7 ms 全在 UI 光栅化里，只有 AGC UI（或已被否决的 720p）能救。
6. **工作量**：
   - 最小增量（AGC 视频 + llvmpipe UI 叠加）：**9-15 人天**（不含硬解；硬解另算）。
   - AGC UI（给 nanovg 写 AGC 后端 + 新 VideoContext + 保真/性能联调）：**另加 15-25 人天**；合计 25-40 人天，属"重写渲染层"量级。
   - 复用 EVO 的 `*_pipe.h`（已生成好的 ISA + 寄存器表，GPL-3.0）可以省掉 LLPC/amdllpc 工具链的 1-2 人天，但会受其顶点格式（20 B stride）约束，见 §5.4。

---

## 1. 依据：外部项目、许可证、文件与符号

| 项目 | 许可证 | 关键文件 / 符号 | 本笔记用它证明什么 |
|---|---|---|---|
| `sainsaji/EVO-PLAYER-PS5` | GPL-3.0 | `docs/evo-pro/agc-bare-metal-ui.md`（全文，尤其 `:3` 状态、`Clip masks` 节、`Runtime bugs` 表、「Not done」节）；`docs/evo-pro/gpu-rendering-plan.md`（§2 三条路线、§3 ProsperoLight 分工、§Step3）；`docs/evo-pro/agc-implementation.md`（§3 DCB 注解、§4a 基础件、§5 Step3 工作分解）；`projects/evoplayer/ui_rml/src/evo_rmlui_render_agc.cpp`（1096 行，2D UI 渲染器实现）；`projects/evoplayer/ui_rml/include/evo_rmlui_render_agc.h`（接口与状态）；`projects/evoplayer/shaders/agc/ui_screen_2d.pipe`（179 行，完整 2D pipeline 定义 + `[ResourceMapping]`）；`media/src/evo_agc_runtime.c`（2771 行，AGC 运行时/VideoOut/stencil/翻转）；`media/src/evo_agc_writer.c`（362 行，DCB 包与寄存器配方）；`media/include/evo_agc_runtime.h`（pipeline 枚举、layer、user-data layout）；`projects/common/include/evo_ps5.h:196-215`（VideoOut ABI 与格式字） | AGC UI 的可行性与最小 pipeline 集；寄存器级配方；stencil 实现；VideoOut 属性；真机风险清单 |
| `mpereiraesaa/ps5-agc-gears` | GPL-3.0 | `README.md`（真机 60,000 帧连续运行）；`src/ps5_color_target.c`（颜色目标寄存器块）；`src/ps5_surface.h:13`（`PS5_SURFACE_FORMAT_WORD 0x8000000022000000`，64 MiB 缓冲步长、0x20000 对齐）；`src/ps5_pipeline.c`；`shaders/gears_lit.pipe`（`.pipe` 格式 + `push_constant` 用法） | **最小** AGC 上屏 demo 的构成与代码量（见 §5.4）；`0x8000000022000000` 这个格式字确实能被 AGC 画进去 |
| `Rufidj/ps5link-sdk`（`examples/gpu_cube`） | GPL-3.0 | `examples/gpu_cube/main.c:43-45,412-414`（`VIDEOOUT_TILING_TILED=0 / LINEAR=1`、`VIDEOOUT_PIXELFORMAT_BGRA8_SRGB=0x8000000000000000`、`RegisterBuffers2` 调用点）；`linker/catalog.c:436-437` | VideoOut 的 tiling/格式字取值；面向原生标题的最小 GPU 例子 |
| `mpereiraesaa/ps5-xash3d-halflife` | GPL-3.0 | `docs/CAPABILITY_MATRIX.md`（AGC 渲染器 Ready、`sceAgcSuspendPoint` 为必需）；`src/ps5_agc_writer.c`(180)、`src/ps5_present.c`(69)、`src/ps5_shader_header.c`(110)、`native/ps5_agc_native.c`(119) | 完整 3D 游戏的 AGC 基础设施规模；`sceAgcSuspendPoint` 的必需性 |
| `blackbearreloaded/ProsperoLight` | GPL-3.0 | `src/native_agc_present.cpp`（`sceVideoOutSetBufferAttribute2` @1194、`RegisterBuffers2` @1198、`render_frame` 全流程）；`vendor/ps5/sdk/stubs/videoout_link_stub.c` | 生产级"AGC 呈现 + YUV→RGB + 合成 + flip"；证明 AGC 与 VideoOut 的接法 |
| `blackbearreloaded/ps5-opengl`（本地 `/root/workspace/ps5wiliwili/ps5-native/ps5-opengl`） | GPL-3.0 | `src/gallium/ps5/ps5_screen.c:2316-2379`（`ps5_encode_depth_stencil_state` + `stencil_op[]` 映射表）；`src/platform/ps5_agc_native_runtime.c:2495-2515`（`DB_STENCIL_INFO 0x20000181`） | 已知的 GL→AGC stencil 编码（与我们需要的 wrap 语义相关） |
| `SvenGDK/SharpProspero` | GPL-2.0（**只读，不可复制代码**） | 经 EVO 整理的 `docs/evo-pro/sharpprospero-agc-reference.md`（`DisplayDevice` 行、§3 帧循环、`// row pitch MUST be left 0`、1920×1080 白名单） | VideoOut 属性/尺寸约束的旁证 |

本地参照：
- `wiliwili-native/library/borealis/library/include/borealis/extern/nanovg/nanovg_gl.h`（GL3 后端 1709 行，着色器源码 `:560-690`，各类 call 实现 `:1030-1175`，纹理 `:732-884`）
- `.../nanovg/nanovg.h:666-669`（`NVGvertex { float x,y,u,v; }`）、`nanovg.h` 的 `NVG_IMAGE_*` 标志
- `.../nanovg/fontstash.h`（字形图集、`fonsSetDilate`、`fons__dilate` @1444）
- `wiliwili-native/library/borealis/library/lib/platforms/sdl/sdl_video.cpp:343-353`（后端选择点：`nvgCreateGL2/GL3/GLES2/GLES3/D3D11/MTL`）
- `wiliwili-native/wiliwili/source/view/{danmaku_core,live_core,mpv_core,animation_image}.cpp`（实际绘制用法）
- `wiliwili-payload/build-ps5/ps5-sdl-src/src/video/ps5/SDL_ps5video.c:172-181,237-250`（我们当前自己的 VideoOut 注册：格式 `0x8000000022000000`、tiling=0、2 缓冲、`AllocateMainDirectMemory` 对齐 0x20000）

---

## 2. nanovg 绘制能力 → AGC 的逐项映射

### 2.0 先看我们实际用到什么（本地符号统计）

`borealis` + `wiliwili/source` 里出现频次最高的 nanovg 调用（`grep -o 'nvg[A-Z][A-Za-z]*('` 统计）：

| 能力 | 代表调用（出现次数，borealis+wiliwili） | nanovg 内部落点 |
|---|---|---|
| 矩形 / 圆角矩形 | `nvgRect`(26+17)、`nvgRoundedRect`(14+9)、`nvgRoundedRectVarying`(1) | `NVG_CONVEXFILL`（凸路径：1 次填充 + 1 次描边 fringe） |
| 路径填充 / 多边形 | `nvgBeginPath`(30+24)+`nvgFill`(18+22)、`nvgMoveTo`/`nvgLineTo`/`nvgBezierTo`/`nvgCircle` | `NVG_FILL`（stencil-then-cover，2-3 次 draw） |
| 描边 | `nvgStroke`(12+1)、`nvgStrokeWidth`、`nvgLineCap` | `NVG_STROKE`（两遍 stencil） |
| 线性/径向/盒渐变 | `nvgLinearGradient`(3+3)、`nvgRadialGradient`(2)、`nvgBoxGradient`(2)、`nvgFillPaint`(7+10) | 同一个 box-gradient SDF 片元代数（`nanovg_gl.h:648-653`） |
| 图片 / 位图 | `nvgImagePattern`(2+6)、`nvgCreateImage*`、`nvgUpdateImage` | `NVG_TRIANGLES`（带 RGBA 纹理） |
| 文本（字体图集） | `nvgText`(3+11)、`nvgTextBox`、`nvgTextBounds`、`nvgFontDilate`(4)、`nvgFontBlur`(4) | `NVG_TRIANGLES`（带 R8 图集纹理 + 顶点色 tint，`nanovg.c:2476-2492`） |
| 裁剪 | `nvgScissor`/`nvgIntersectScissor`(4+3) | `scissorMat`（片元）+ `glScissor`（硬件剪裁） |
| 透明度混合 | `nvgGlobalAlpha`/`nvgFillColor` 的 alpha、`nvgSave/Restore` | `glBlendFuncSeparate`（按 composite operation） |
| stencil 描边 | `nvgCreateGL3(NVG_STENCIL_STROKES \| NVG_ANTIALIAS)`（`sdl_video.cpp:352`） | stencil 写入 + 测试 |

### 2.1 映射表（逐项：AGC 上怎么做、难度、参考）

| # | nanovg 图元 | AGC 上需要什么 | 难度 | 参考实现怎么做的 |
|---|---|---|---|---|
| 1 | 矩形 / 圆角矩形（凸路径） | 同一 2D pipeline：顶点 = 三角化后的 `pos+uv`，颜色走常量块或顶点色；**圆角不需要特殊处理**（几何已由 CPU 三角化） | **低** | EVO 用 RmlUi 的 CPU 三角化 + `ui_screen_2d` 一个 pipeline 覆盖全部 UI（`agc-bare-metal-ui.md` 表格首行；`evo_rmlui_render_agc.cpp:194-307` 的 `RenderGeometry`）。圆角"容器裁剪"另用片元 SDF（见 §2.2 第 6 项） |
| 2 | 路径填充（凹路径/多子路径，nonzero） | stencil-then-cover：**写 stencil（front `INCR_WRAP` / back `DECR_WRAP`，colorMask=0）→ 画 cover 四边形（stencil `NOTEQUAL 0`，随后 `ZERO`）** | **中-高**（唯一真不确定项） | EVO 有 stencil 通路但只用到 `KEEP/REPLACE/INCR_CLAMP`（`evo_agc_runtime.c:749`，DB 寄存器 `0x0200`/`0x010b`，`DB_STENCIL_INFO 0x20000181` = S8）。ps5-opengl 的 Gallium 表里存在 wrap 类条目（`ps5_screen.c:2322-2331`），但**两处编码数值不一致**（EVO `REPLACE=2`，ps5-opengl `REPLACE=3`）⇒ 必须真机确认 |
| 3 | 线性 / 径向 / 盒渐变 | 无需新 pipeline：把 `paintMat(3×vec4) + innerCol + outerCol + extent + radius + feather` 放进常量块，片元里跑 `sdroundrect` SDF（nanovg 已经这么干） | **低** | EVO 的圆角裁剪片元就是同一个 SDF 形状（`ui_screen_2d.pipe` 的 `FsGlsl`：`half_size/centre/q/r/dist/coverage`），常量也在 128 B 块里 |
| 4 | 图片（RGBA 位图） | `NVG_TRIANGLES` → 顶点 + **T#/S# 纹理描述符**（RGBA8、pitch 隐含、256 B 对齐、双线性/最近、CLAMP/REPEAT） | **低-中** | EVO：`CreateTextureInternal()` 行拷贝进 256 B 对齐、pitch 对齐的 direct-mem，`evo_agc_build_tsharp_rgba8/bgra8` + `evo_agc_build_ssharp`（`ui_rmlui_render_agc.cpp:464-517`；`evo_agc_writer.c:70-150`）。**256 对齐是硬要求**（描述符存 `addr>>8`） |
| 5 | 文本（字形图集） | 同 4，但纹理是 **R8 单通道**；颜色来自顶点色/常量 tint；图集重建（512→4096）时换 T# | **中** | EVO 的字形图集就是 `GenerateTexture` 上传的普通纹理（同一 `CreateTextureInternal`）；`evo_agc_build_tsharp_r8`（`evo_agc_writer.c:140-148`，selects `X,0,0,1`）正是 R8 alpha 图集需要的形式。详见 §3 |
| 6 | 裁剪（矩形 scissor） | 双保险：**硬件 scissor 寄存器** `0x090/0x091`（左上/右下，含 `0x80000000` 位）+ 片元里的 `scissorMat` SDF | **低** | EVO：`evo_agc_writer_set_scissor()`（`evo_agc_writer.c:214-228`）；片元 scissor 在 nanovg 着色器里已存在 |
| 6b | 圆角容器裁剪（RmlUi 有 `EnableClipMask`/`RenderToClipMask`，nanovg **没有**对应 API，只有矩形 scissor；我们目前不做圆角裁剪） | 真形状需要 stencil（见 2），**或**用片元 SDF 直接丢弃像素 | **低**（片元路线）；只有将来要加"圆角容器裁剪"才需要 | EVO 明确选片元：`ui_screen_2d.pipe` 注释「neither the stencil path nor the scissor register is proven… The fragment shader is」，把 `clip_rect + clip_radii` 当 flat varying 传给片元做 1 px feather |
| 7 | stencil 描边（`NVG_STENCIL_STROKES`） | 同 2 的 stencil 通路（两遍：膨胀写 stencil → 测试后覆盖） | **中-高**（同 2） | 无 AGC 先例；`glnvg__stroke` 语义见 `nanovg_gl.h:1119-1146` |
| 8 | 透明度混合 | CB 寄存器 `0x01e0`（混合控制）+ `0x008e`（写掩码） | **低** | EVO 已映射三种：`SRC_ALPHA/ONE_MINUS_SRC_ALPHA`(0x40000504)、预乘 `ONE/ONE_MINUS_SRC_ALPHA`(0x40000501)、加法 `ONE/ONE`(0x40000101)（`evo_agc_writer.c:230-262`）。nanovg 的 composite 组合更多（`NVG_COPY`/`NVG_DESTINATION`/`NVG_SOURCE_IN` 等），需补表 |
| 9 | 图片 mipmap / `NVG_IMAGE_GENERATE_MIPMAPS` | 采样器需要 mip 链（自己生成 + 描述符 level 数） | **中**（但**当前用不到**） | 统计显示 wiliwili/borealis 未使用 `NVG_IMAGE_GENERATE_MIPMAPS`；`nvglCreateImage` 只在 flag 有要求时 `glGenerateMipmap`（`nanovg_gl.h:827`）。EVO 的 `build_ssharp` 只设 `maxLod` + 双线性，没有 mip 链 |
| 10 | `nvgFontBlur` / `nvgFontDilate` | **纯 CPU**（fontstash 侧的字形栅格化参数），AGC 没有额外工作 | **无** | `fonsSetBlur`/`fonsSetDilate` 决定图集里字形位图的形状（`fontstash.h:991-994`、`fons__dilate` @1444）。注意：弹幕描边就是把同一段文字用 `dilate>0` 的**另一份图集字形**再画一遍（`wiliwili/source/view/danmaku_core.cpp:196-212`），代价在图集面积与像素覆盖上，不在片元上 |

### 2.2 AGC 侧必需的状态面（按 nanovg 用到的量）

nanovg GL3 后端对 GL 的依赖可以完全枚举（`nanovg_gl.h`）：`glDrawArrays`、`glStencilFunc/Op/OpSeparate`、`glColorMask`、`glBlendFuncSeparate`、`glEnable/Disable(STENCIL_TEST/CULL_FACE/DEPTH_TEST/SCISSOR_TEST)`、`glCullFace/glFrontFace`、`glViewport`、`glScissor`、`glUseProgram`、`glUniform*`、`glBindTexture`、`glTexImage2D/glTexSubImage2D/glGenerateMipmap`、`glPixelStorei`、`glBufferData`（顶点/UBO）、`glVertexAttribPointer`、VAO。对应 AGC 面：

| GL 面 | AGC 面（寄存器/包） | 依据 |
|---|---|---|
| `glDrawArrays` | `sceAgcDcbDrawIndex` / `DrawIndexAuto`（+ 每 draw 的 `SetShRegisterRangeDirect` 写 user-SGPR：GS 槽 `0x8c`、PS 槽 `0x0c`） | `evo_agc_writer.c:288-334` |
| 视口 | CX regs `0x10f-0x114`、`0x0b4/0x0b5`、`0x2fa-0x2fd` | `evo_agc_writer.c:181-208` |
| `glScissor` | CX regs `0x090`/`0x091` | `evo_agc_writer.c:214-228` |
| `glBlendFuncSeparate` + `glColorMask` | CX regs `0x01e0` / `0x008e` | `evo_agc_writer.c:230-262` |
| `glStencilFunc/Op/ColorMask(0)` | DB regs `0x0200`（stencil+z enable、func）、`0x010b`（ops）、ref/mask、read/write base、`DB_STENCIL_INFO 0x20000181`（S8） | `evo_agc_runtime.c:728-800, 1062` |
| 顶点缓冲/属性 | **V# 描述符**（4 dwords，含 base/stride/records，`0x11014fac` 标准缓冲格式）+ `IndirectUserDataVaPtr` 槽 | `evo_agc_writer.c:67-79`；`ui_screen_2d.pipe` 的 `userDataNode[1]` |
| 常量块 | **V# 常量描述符**（`0x31016fac`，4 dwords 指向一块 GPU 可见内存），VS user-data 槽 | `evo_agc_writer.c:81-93` |
| 纹理 / 采样器 | **T# + S#（合并描述符 12 dwords）**，PS user-data 槽；base 必须 256 B 对齐 | `evo_agc_writer.c:95-150`；`ui_screen_2d.pipe` 的 `userDataNode[2]` |
| 程序绑定 | `sceAgcCreateShader` + `sceAgcLinkShaders` | `agc-implementation.md` §3；`agc-bare-metal-ui.md`「The shader toolchain」 |
| 帧结束可见性 | `sceAgcCbReleaseMem(45, GCR 12)`（CB 数据 flush/inv）+ `(40, GCR 0x30c)`（L2 写回 + 失效，带 fence 写 marker） | `evo_agc_writer.c:336-362`；`agc-bare-metal-ui.md`「End-of-pipe cache protocol」 |
| 提交 | `sceAgcDriverSubmitDcb` + **`sceAgcSuspendPoint()`** | `xash3d/docs/CAPABILITY_MATRIX.md`（明示必需）；`agc-implementation.md` §3 |

---

## 3. 文本渲染专章（字形图集 + 采样/混合注意点）

**图集本身（本地事实）**
- 初始化尺寸 `NVG_INIT_FONTIMAGE_SIZE = 512`，上限 `NVG_MAX_FONTIMAGE_SIZE = 4096`（`nanovg.c:40-52, 341-350`）。CJK + 多语言 fallback 会逼近上限（4096×4096×1 B = 16 MB）。
- 格式：`NVG_TEXTURE_ALPHA` → GL3 下单通道 **`GL_R8`/`GL_RED`**（`nanovg_gl.h:782-784`），增量上传走 `glTexSubImage2D`（`nanovg_gl.h:844-884`）。
- 文字绘制 = `renderTriangles`（`nanovg.c:2476-2488`）：**一次 draw per 文本 run**，颜色是 `innerColor` tint，纹理是当前图集。
- 图集"增长"= 重建更大的纹理（`nvg__allocTextAtlas`，`nanovg.c:2467-2468`）+ 重画所有字形；nanovg 会把新的 image handle 传给后续 draw。

**AGC 上的做法**
1. 图集 = 一块 256 B 对齐、pitch 对齐的 GPU 可见 direct-mem（可在 **CPU 侧直接写**：`texData` 的矩形拷贝进 GPU 内存），T# 用 `evo_agc_build_tsharp_r8`（`GFX10_FORMAT_8_UNORM` + selects `X,0,0,1`，`evo_agc_writer.c:140-148`）——这正是单通道字形图集需要的形式；**不需要重新上传整张图集**，字形增量就是"CPU 写几个矩形 + clflush 该范围"。
2. 每次图集增长：新分配 + 新建 T#（旧的 256 对齐内存释放）。nanovg 的 handle 语义是"每张图集一个 id"，实现里跟着 id 换描述符即可。
3. 采样器：字形需要 `LINEAR`（与今天 GL 的默认一致）+ `CLAMP`。EVO 的 `evo_agc_build_ssharp(clamp=1, bilinear=1)` 给出可用字（`0x09500000` 双线性位）；`NEAREST` 只要清掉对应位（`danmaku_core.cpp:63` 的 mask 图用 `NVG_IMAGE_NEAREST`）。
4. 混合：字形是 **alpha-only**，必须走 `type==3`（textured tris）× 顶点/常量 tint，也就是"预乘/直乘"选择要和 GL 路径的 `glBlendFuncSeparate` 一致，否则 CJK 小字号会出现"边发灰/发白"。nanovg 的 tint 是 `color = texture(tex, ftcoord) * innerCol`（`nanovg_gl.h:676-684`），innerCol 通常是直乘 RGBA → 混合模式要选 `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`（非预乘），与 EVO 的 `EVO_AGC_BLEND_ALPHA` 对应。
5. 通道/顺序：AGC 侧有三处必须互相一致——**CB 导出格式 / 顶点色格式 / T# 的 select**。EVO 为此付了两次真机代价：颜色在片元输出端交换后所有 UI 变琥珀色，以及"只修顶点属性反而全错"（`ui_screen_2d.pipe` 的 `[GraphicsPipelineState]` 与 `[VertexInputState]` 注释原文）。我们的目标面是 `0xAABBGGRR` 扫描输出 ⇒ 选 `CB_COLOR0_INFO` 的 `COMP_SWAP = ALT`（EVO 修复表首行「Whole UI colour-swapped」）。
6. 弹幕这类高频文本：当前 `incline` 默认（两遍普通文字，1 px 偏移）在 nanovg/AGC 下同样是"两次 `renderTriangles`"；`stroke` 样式是"带 dilate 的另一份图集字形 + 两次绘制"，图集内存与像素覆盖都会翻倍（`status.md`「播放器帧率根因」已量化）。

---

## 4. 混合方案可行性（重点）

### 4.1 问题拆解
三种"混合"是不同的东西，要分开判断：
- **(H1) 视频/重图层走 AGC，UI 仍走 llvmpipe，两者写同一张 VideoOut 缓冲**；
- **(H2) 用 VideoOut 的多平面/叠加层让硬件合成**；
- **(H3) UI 走 AGC，视频仍在 llvmpipe（反向混合）**。

### 4.2 (H2) VideoOut 多平面/叠加层：**没有 ABI 证据**
- `sceVideoOutSetBufferAttribute2(attr, pixelFormat, tilingMode, width, height, 0,0,0)` + `sceVideoOutRegisterBuffers2(handle, startIndex, unk, buffers, bufferNum, attr, unk2, unk3)`（`evo_ps5.h:208-215`；ps5link-sdk `gpu_cube/main.c:49-52` 同形）。参数里有 `set_index`，EVO 的桩把它当"缓冲集索引"（`tools/native-app/stubs/videoout_link_stub.c:98,103`），但**四个 AGC 项目全部只用 `set 0` + 2 缓冲**：
  - EVO：`sceVideoOutRegisterBuffers2(handle, 0, 0, video_buffers, 2, &attr, 0, NULL)`（`evo_agc_runtime.c:1323,1357`；`agc-implementation.md:250-251`）
  - ProsperoLight：`RegisterBuffers2(presenter.video, 0, 0, buffers, 2, &attribute, 0, NULL)`（`native_agc_present.cpp:1198`）
  - ps5-agc-gears：`PS5_SURFACE_BUFFER_COUNT = 2`、单一格式字（`src/ps5_surface.h:4-14`）
  - ps5link-sdk：`gpu_cube/main.c:412-414`
- 没有任何项目注册过 YUV/NV12 扫描输出缓冲或第二平面；EVO 连"视频四边形"也是画进**同一张 BGRA 缓冲**（`gpu-rendering-plan.md` §3「Composite UI surface over video → GPU: second textured quad」）。
- 结论：**不存在"硬件分层合成"这条路**（至少在公开 ABI 里没有）。H2 放弃。
- 附带约束（若哪天要试）：`row pitch MUST be left 0`；1920×1080 是唯一普遍被接受的模式（SharpProspero 整理文 `sharpprospero-agc-reference.md` §1 `DisplayDevice` 行、§3 注释）；`0x8000000022000000`（= 内存 `0xAABBGGRR`）与 `0x8000000000000000`（BGRA8_SRGB）两种格式字在项目间并存，混用会导致"画面正常但通道互换"（EVO 的 `status.md:216`、`:840` 记录了同一类事故）。

### 4.3 (H1) 单平面、两个绘制者：**有真机先例，可行，但有一串硬约束**
先例（都是"GPU 画内容 + CPU 往同一张 VO 缓冲上再画/合成"）：
- EVO `pp_agc_present_nv12(vout_handle, buf_idx, gpu_target, nv12, …)`：AGC 把 YUV 四边形画进 **`pp_videoout` 自己注册的** VO 缓冲（不是 AGC 自己注册的），DCB 里可以带 `SetFlip`，但他们"保留 CPU flip 为已验证路径"（`gpu-rendering-plan.md` Step2 进度条 + `agc-bare-metal-ui.md`「Not done」最后一条）。
- EVO 反向版本：AGC/GL 画完视频后 **CPU 合成 OSD**（`evo_agc_composite_bgra`，`evo_agc_runtime.h` 注释）——与我们"GPU 画视频、CPU 画 UI"完全同构。
- ProsperoLight 的分工表：视频与合成都上 GPU，UI 仍是 CPU 表面（`gpu-rendering-plan.md` §3 表）。

真机踩过的坑（直接决定我们的实现顺序）：
| 现象 | 根因（EVO 原文） | 对我们的含义 |
|---|---|---|
| 加载时画面出现向下蔓延的黑带 | `sceVideoOutSubmitFlip` 是异步的且没等；2 缓冲下 CPU 清屏擦掉了**正在被扫描输出**的那张缓冲 | 混合写入必须严格"每槽 fence 后才写"，不能想当然 |
| UI 一帧后冻住 / 空白帧 | 帧槽 seal/token 状态机用错（`TOKEN_MISMATCH` 被忽略），以及该 abort 却 seal | 需要一个显式的 per-slot 状态机（EVO `evo_agc_transient_ring.h` / `frame_begin`） |
| 画面通道互换但"看起来没错" | RT 的 format/tiling/COMP_SWAP 与 VO 注册属性不匹配 | 注册属性与 CX 寄存器必须成对确定 |
| 首帧后 GPU 卡死、约 55 s 后进程被杀 | `VGT_ESGS_RING_ITEMSIZE` 用了 4（应为 1，NGG 非 passthrough 下会把顶点索引乘掉） | 走 LLPC 自己生成 pipeline 时要按 PAL 元数据推导，不要手填 |
| 整个屏幕"重载"式扫描 / 画面不更新 | 提交后没等 GPU 完成就 present；以及 CB/L2 里的帧没写回 DRAM | 必须有 `ReleaseMem(45)` + `ReleaseMem(40, GCR 0x30c)`；我们 payload 侧已经用 `glFinish()` 解了 GL 版本的同款问题 |

**H1 的两种实现，推荐后者**：
- (a) 分离表面 + CPU 混合：AGC 画视频到 VO 缓冲，llvmpipe 画 UI 到自己的表面（透明背景），CPU 用 SIMD 预乘 alpha 混合写回 VO 缓冲。稳定、简单，但多一遍 1080p 读写（约 2-4 ms）+ 双向缓存可见性处理。
- (b) **共用一个缓冲（推荐）**：AGC 先把视频四边形画进当前 VO 缓冲 → 等该槽 fence → llvmpipe **以该缓冲为 framebuffer** 画 UI（只在这一帧跳过视频矩形区域的清屏；现有代码里已出现过 `VideoContext::clearExcept(保留矩形)` 的原型，见 `status.md`「主页'局部渲染（静态层）'尝试」）→ CPU `SubmitFlip`。零额外拷贝；代价是"GPU→CPU 可见性 + 严格帧槽序"。
  - 可见性：GPU 侧用 `ReleaseMem(45/40)`；CPU 侧对 direct-mem 的写入用 `clflush`/非临时存储（EVO `evo_agc_runtime_cache_flush`、`stream_fill` 的注释讲得很细：direct mem 是 `SCE_KERNEL_WB_ONION`，不 flush GPU 可能一直看不到）。
  - 顺序：必须"先等 fence 再写这一槽"，否则复现 EVO 的黑带事故。
  - llvmpipe 读视频像素（UI 与视频做 alpha 混合时）也必须发生在 GPU 写回之后，否则残影。

### 4.4 (H3) 反向混合（UI 走 AGC、视频留 llvmpipe）：不推荐
UI 一旦上 AGC，llvmpipe 里剩下的就只有视频；那不如把视频也搬走（AGC 视频管线/或 UI pipeline 直接画 RGBA 帧），否则又要在两个上下文间传递视频帧。

### 4.5 结论
- **H1 可行**，请按 4.3(b) 做；**H2 不可行**（无 ABI 支持）；**H3 无意义**。
- 收益的诚实边界：混合方案只解决"视频帧的 YUV→RGB + 缩放 + 上屏"这一段（真机上就是播放页那 69 ms `flush` 的大头），**对纯 UI 页面（主页/设置页）毫无帮助**——那里 17.7 ms 全在 UI 光栅化。
- 附：EVO 的 AGC 4K 视频合成收据 **982 µs/帧**（`gpu-rendering-plan.md` 开头 + `ps5-port-status.md`「GPU 渲染参考实现」），对照我们 llvmpipe 每帧 18 ms。

---

## 5. 若走 AGC UI：最少需要什么

### 5.1 最小 pipeline 集
| pipeline | 用途 | 依据 |
|---|---|---|
| `ui_screen_2d`（**必需**） | 所有 2D 三角形：纯色、纹理化、顶点色调制、带圆角裁剪 SDF | EVO 只有这一个 UI pipeline（`shaders/agc/ui_screen_2d.pipe`，179 行） |
| `ui_screen_2d`（`EDGE_AA` 变体） | nanovg 的 AA 描边/fringe 需要 stroke 掩码；GL3 后端就是"同一份着色器 + `#define EDGE_AA 1`"两个 program（`nanovg_gl.h:697-701`） | 本地 nanovg |
| `ui_backdrop_blur`（可选） | 只有做 `backdrop-filter: blur` 类效果才需要 | EVO `shaders/agc/ui_backdrop_blur.pipe`；**wiliwili 不需要** |
| 视频管线条（可选，属另一条线） | NV12/planar/P010 的 YUV→RGB + 缩放 | EVO 4 条 video pipeline；若走 H1 最小增量、且视频帧在 CPU 侧已是 RGBA，则**连这条都不需要**（RGBA 四边形走 `ui_screen_2d` 即可） |
| 无 Z 缓冲 | 只有 stencil（S8）需要；EVO 明确不分配深度面 | `agc-bare-metal-ui.md`「Clip masks」；`evo_agc_runtime.c:84` 给 stencil 16 MB 预算 |

### 5.2 顶点 / 常量缓冲布局
- **nanovg 顶点**：`struct NVGvertex { float x,y,u,v; }`，stride 16（`nanovg.h:666-669`），V# 用 `stride=16`。
- **EVO 的 UI 顶点**（作对照，也可直接借用其 pipeline）：`Rml::Vertex`，stride 20 = `vec2 pos + BGRA8 color + vec2 uv`（`ui_screen_2d.pipe` 的 `[VertexInputState]`）。
- **每 draw 的常量块**：nanovg 需要 `scissorMat(3×vec4) + paintMat(3×vec4) + innerCol + outerCol + scissorExt + scissorScale + extent + radius + feather + strokeMult + strokeThr + texType + type` = **11×vec4 = 176 B**（`nanovg_gl.h:203-215`，`NANOVG_GL_UNIFORMARRAY_SIZE 11`）。EVO 的 `ScreenConstants` 是 128 B（`mat4 projection + vec4 translation + clip_rect + clip_params + clip_radii`，`ui_screen_2d.pipe` 的 `VsGlsl`）。
- **每 draw 的临时内存（可照抄 EVO 的分配模式）**：常量 128 B + 常量 V# 16 B + 顶点 V# 16 B + T#/S# 合并描述符 48 B = **208 B/draw**，从 per-frame 的 transient ring 里分配（`evo_rmlui_render_agc.cpp:210-300`）；顶点/索引数据是 nanovg 每帧重建的一整块，直接整块上传（对应 `glBufferData(STREAM_DRAW)`，`nanovg_gl.h:1256-1268`）。
- 纹理侧：**256 B 对齐 + pitch 256 对齐**是硬约束（GFX10 描述符存 `addr>>8`），EVO 的最佳实践是"超配 255 B 再对齐"，并在构造器里**拒绝**未对齐的 base（`evo_agc_writer.c:101-113`、`ui_rmlui_render_agc.cpp:481-497`）。
- 三个"必须一致"的地方（EVO 用两次真机事故换来的教训）：CB 导出格式、顶点属性格式、T# select；扫描输出是 `0xAABBGGRR` ⇒ CB 用 `COMP_SWAP=ALT`、导出声明 `R8G8B8A8_UNORM`、T# 与顶点属性不要各自再换一次。

### 5.3 每帧提交规模（对照 wiliwili 主界面 150-200 个绘制对象）
- nanovg 的 draw 次数与"对象数"不是 1:1（`nanovg_gl.h` 的 call 表 `:166-174`）：
  - 凸路径（矩形/圆角矩形）= `glnvg__convexFill`：每条路径 **1 次填充 + 0/1 次 fringe** ⇒ 1-2 次；
  - 非凸路径 = `glnvg__fill`：stencil fan(1) + （AA 时）fringe strip(1) + cover strip(1) ⇒ **2-3 次**；
  - 描边 = `glnvg__stroke`：**2 次**（+AA 再 +1）；
  - 文字 / 图片 = `glnvg__renderTriangles`：**1 次 per run**。
  ⇒ 主界面 150-200 个对象 ≈ **300-500 个 AGC draw 包**（弹幕/长文本页更多，`status.md` 里"每帧数千次 `glDrawArrays`"是在播放页量到的）。
- DCB 规模对照：EVO 的 RmlUi 帧在真机上 `dcb=11872/524288 dwords`（`agc-bare-metal-ui.md`「Verifying a build」的 health 行），即整帧 **约 47 KB / 槽容量 2 MB（≈2%）**。我们的量级与 EVO 同阶（对象数相当，且 nanovg 的 per-draw 常量更小：176 B vs 208 B）⇒ **DCB 容量不是问题**，2 MB/槽 + 3 槽轮转足够。
- 顶点量级估算：一个圆角矩形三角化后约 30-80 顶点（含 fringe），文字每字形 4 顶点 ⇒ 主界面约 **1万-5万顶点 × 16 B = 160-800 KB/帧**的环形上传；1080p 下这点带宽可忽略（对照：EVO 的 transient ring 与 direct-mem 预算 `direct_mem=6249472/67108864`，`agc-bare-metal-ui.md` 同处）。
- 帧槽纪律：每帧 = 1 个 DCB submit + fence marker（GPU 回写 `ReleaseMem`），翻转沿用 CPU `SubmitFlip`（先 `GetFlipStatus` 轮询）⇒ payload 侧已经验证过的"只保留一帧在飞 + 等上一帧 flip 事件"这套逻辑可移植到标题侧（`status.md`「payload 线帧率：30 → 48 fps」）。

### 5.4 代码量对照（决定"从零写还是抄"）
| 参考 | 规模 | 说明 |
|---|---|---|
| `ps5-agc-gears` 全部源码（含 demo、telemetry） | **3817 行**（`src/*.c,h` 48 个文件 + `native/`） | 最小可跑的 AGC 上屏（深度测试 + 光照 gears） |
| `ps5link-sdk/examples/gpu_cube/main.c` | 631 行（单文件） | 更极简的 cube 例子 |
| EVO `evo_agc_runtime.c` + `evo_agc_writer.c` + `evo_agc_transient_ring.c` + `evo_agc_shader_header.c` | **约 3400 行** | 生产级运行时（含 layer/blur/clip mask/诊断） |
| EVO `evo_rmlui_render_agc.cpp`（2D UI 渲染器本体） | **1096 行** | 覆盖 RmlUi 全部 RenderInterface 方法（含 clip mask / layer / blur） |
| 我们的 nanovg AGC 后端（自估） | **1200-1800 行** C | 覆盖 6 类 call + 纹理 + stencil + scissor + 176 B 常量块；比 EVO 的 UI 渲染器多"stencil-then-cover"、少"layer/blur/RmlUi 适配" |
| xash3d 的 AGC 基础设施（writer/present/shader_header/native 等） | 约 700 行（`src/ps5_agc_writer.c` 180、`present` 69、`shader_header` 110、`native/ps5_agc_native.c` 119 + 头） | 完整 3D 游戏在跑，可作为拆分粒度的参考 |

---

## 6. 风险清单（按"会静默失败"排序）

| 风险 | 说明 | 处理 |
|---|---|---|
| **stencil wrap 编码**（高） | nanovg 的非凸填充/描边需要 front `INCR_WRAP` + back `DECR_WRAP`（`nanovg_gl.h:1046-1047`）；EVO 只验证 `KEEP/REPLACE/INCR_CLAMP`（`evo_agc_runtime.c:749`），ps5-opengl 的表里 wrap 类编码与 EVO 数值不一致（`ps5_screen.c:2322-2331`）。写错的表现是"某些图形整块消失/糊成一片"，不报错 | 1 轮真机专测；或改走 §7 备选（CPU 三角化，不用 stencil） |
| **颜色/通道三处一致性**（高，但已被文档化） | CB 导出 / 顶点色 / T# select 任一不一致 ⇒ 全 UI 通道互换或"看起来没错但图片错" | 照 `ui_screen_2d.pipe` 的注释做，并保留 EVO 的 `agc_dump_scanout` 式回读诊断 |
| **256 B 对齐**（中，静默） | T# 存 `addr>>8`，未对齐 ⇒ 部分图集/图片花屏、部分正常 | 分配时超配且构造器里硬拒绝未对齐（照抄 EVO） |
| **VideoOut 单一 owner**（中） | `sceVideoOutOpen` 第二次会 panic；`libSceAgc`/`libSceVideoOut` 在 self-unjail 之后失效 ⇒ 必须 main 早期初始化 | AGC 只能跑在**原生标题**（app slot）里；而标题当前是 ps5-opengl 的 G19 桥在持有 VideoOut/EGL ⇒ 走 AGC 就必须**由自己接管 VideoOut 并停用该桥**。payload SDL 驱动的 `RegisterBuffers2/SubmitFlip` 与"只保留一帧在飞"的修复（`SDL_ps5video.c:172-181,103`）是**代码参考**（同一套 API、同样的两缓冲模型），不是同进程复用 |
| **挂死/看门狗**（中） | AGC submit 卡死会冻结整个应用槽 | 照抄 EVO：独立 submit 线程 + 250 ms 超时 + `sigsetjmp` 首次故障保护 + `pp_agc_available()` **运行时重查**回退到 llvmpipe（`agc-implementation.md` §5a「AGC-death recovery」） |
| **扫描输出对齐**（中） | 我们当前 `AllocateMainDirectMemory(…, 0x20000, …)`、两块缓冲间隔 `memsize/2`（`SDL_ps5video.c:237-249`）；AGC 画 tiled 扫描输出要求 **2 MiB 对齐**且缓冲步长是 2 MiB 倍数（EVO：`EVO_AGC_DIRECT_MEM_ALIGN`，并打印 `scanout align mis0/mis1` 必须为 0；gears 用 64 MiB 步长，`ps5_surface.h:9-11`） | 改成 0x200000 对齐 + 步长对齐；这是走 AGC 的前置条件 |
| **着色器工具链**（中，一次性） | `.pipe`(GLSL 450) → `amdllpc -gfxip=10.1.3`（公开 AMDVLK **不含 gfx1013**，需打补丁）→ 切 ISA + 读 PAL → 推导 AGC 寄存器；且 LLPC 构建需要 `dxc`、必须 `-j4`（EVO 实测） | 直接复用 EVO 的 `Dockerfile.amdllpc` + `tools/patches/llpc-add-gfx1013.py` + `tools/build_agc_pipes.py`（GPL-3.0）；1-2 人天 |
| **NGG 参数**（中，静默） | `VGT_ESGS_RING_ITEMSIZE` 必须为 1（EVO 曾用 4 ⇒ GPU 卡死、55 s 后被杀）；所有寄存器必须从 PAL 元数据推导，不能硬编码（psbc 时代的两个静默事故） | 用 `build_agc_pipes.py` 的失败即拒发策略，别手填 |
| **字体图集重建**（低-中） | 512→4096 增长时换 T#，期间可能在一帧中途发生 | 图集分配走自己的 ring/池，重建只在帧边界提交（nanovg 的 `nvg__allocTextAtlas` 本身也在 draw 之间） |
| **`NVG_STENCIL_STROKES` 关不掉**（低） | 已实测：关掉对 llvmpipe 无收益（`status.md` 合批实验），但它决定 AGC 后端是否必须实现描边 stencil 通路 | 若 stencil 编码确认困难，可先把 `NVG_STENCIL_STROKES` 关掉（走"无 stencil 描边"），用 `EDGE_AA` 保住观感 |

---

## 7. 工作量估计与最小增量路径

### 7.1 阶段 A：混合（AGC 画视频，UI 留 llvmpipe）— 推荐先做
| 工作 | 人天 |
|---|---|
| 把 EVO 的 AGC 运行时裁剪成"仅 init + 1 个 pipeline + 1 个 draw + fence + 结束可见性"（去掉 layer/blur/clip mask/视频管线），在**原生标题**里接管 VideoOut（照 payload SDL 驱动 `SDL_ps5video.c:172-181,103` 的注册/翻转写法，含"只保留一帧在飞"的修复），并停用 ps5-opengl 的 G19 桥 | 2-4 |
| 视频帧进入 AGC：把**标题**构建切回 `MPV_SW_RENDER`（代码仍在：`mpv_core.cpp:422-424`、`mpv_core.hpp:407-420`，`sw_format="rgba"` 由 mpv 写入 CPU 缓冲），改成写入 GPU 可见 direct-mem 并 clflush；或先 memcpy+clflush 保稳 | 2-3 |
| 帧序与合成：per-slot fence、`clearExcept(视频矩形)`、llvmpipe 以 VO 缓冲为 framebuffer 画 UI（OSMesa 可绑定调用方提供的缓冲，不需要 EGL/GL 上下文）、CPU `SubmitFlip`；异常回退到今天的纯 llvmpipe 路径 | 3-4 |
| 真机联调（黑带/残影/撕裂/通道/性能） | 3-5 |
| 合计 | **10-16 人天** |
| 预期收益 | 播放页 13.7 fps → 取决于 mpv sw 转换成本，目标 40-60 fps；4K/HDR 上屏能力打开；**主页等纯 UI 页无收益** |

未知需先量（1 人天可出结论）：`MPV_RENDER_API_TYPE_SW` 在本机的 YUV→RGBA + 缩放成 (1080p 全屏) 的 CPU 成本。若它超过约 10 ms，则阶段 A 的收益缩水，应直接跳到阶段 B 或改走 `sceVideodec2` + AGC NV12 管线（另一条线）。

### 7.2 阶段 B：AGC UI（nanovg AGC 后端）— 真正解决 UI 页
| 工作 | 人天 |
|---|---|
| 着色器工具链落地（amdllpc + gfx1013 补丁 + `build_agc_pipes.py`，或先直接沿用 EVO 的 `*_pipe.h` 并把顶点格式适配成 20 B） | 1-2 |
| AGC 运行时完整化（3 槽 DCB + transient ring + fence/看门狗 + 诊断 + 回退） | 3-5 |
| `nanovg_agc.h` 后端：6 类 call、176 B 常量块、V#/T# 描述符、纹理创建/更新（R8/RGBA）、硬件 scissor、混合表、**stencil-then-cover**、AA fringe | 8-12 |
| borealis 接入：新增一个非 GL 的 VideoContext/后端选择（对照 `sdl_video.cpp:343-353` 的后端分支），`ImageHelper` 上传队列改为直写 GPU 纹理，丢帧/失效处理 | 3-4 |
| 真机保真 + 性能（AA 边缘、CJK 字形、弹幕、渐变、圆角、通道、4K） | 4-8 |
| 合计 | **19-31 人天**（区间中值 ~25） |

阶段 A + B 合计 **约 30-45 人天**（单人 6-9 周，不含硬解那条线）。这与既有调研的"重写渲染层"量级一致（`ps5-port-status.md`「GPU 渲染参考实现」末段）。

### 7.3 最小增量路径（明确排序）
1. **前置（0.5-1 人天）**：VO 缓冲改成 2 MiB 对齐 + 2 MiB 倍数步长；确认 `0x8000000022000000`/tiling=0 与 CX 的 format/COMP_SWAP 配对；在 main 早期（self-unjail 前）初始化 AGC。
2. **AGC 冒烟（1-2 人天）**：照 `ps5link-sdk/examples/gpu_cube` 或 `ps5-agc-gears`（都是 GPL-3.0）在**原生标题**（已验证能启动的 app slot，如 PPSA99010/12）里画一个全屏纯色/纹理四边形到我们注册的 VO 缓冲上，验证"AGC 能画进我们的缓冲 + CPU flip 能上屏"。这一步不需要 nanovg、不需要 stencil。**不要**在 payload（hbldr）上下文里试 AGC——它连硬解都被上下文门挡住（errno 5200），AGC 也从未被证实可用。
3. **阶段 A（10-16 人天）**：视频四边形 + UI 叠加（§4.3(b)）。收益集中在播放页。
4. **阶段 B（19-31 人天）**：nanovg AGC 后端，主页/设置页也上 GPU；此后可把 llvmpipe 完全移出渲染路径（只留作回退）。
5. **备选（若不想要 stencil 风险）**：把"路径 → 三角形"的三角化搬到 CPU（RmlUi 的做法，EVO 因此完全不需要 stencil 填充；本地已有 `lunasvg` 可参考 SVG→几何），只用 `ui_screen_2d` 那种简单 pipeline。代价是 AA 语义与 overdraw 变化，且要写/接一个三角化器（约 +1000-1500 行），总工作量与阶段 B 相当但**去掉 §6 的第一条风险**。

---

## 8. 可复用的外部文件清单（含许可证）

| 我们可以直接借鉴/抄的文件（GPL-3.0，与 wiliwili 兼容） | 用途 |
|---|---|
| `EVO-PLAYER-PS5/projects/evoplayer/media/src/evo_agc_runtime.c` + `evo_agc_writer.c` + `evo_agc_transient_ring.c` + `evo_agc_shader_header.c` + 对应头 | AGC 运行时/DCB 包/帧槽/着色器头（阶段 A 与 B 的底座） |
| `EVO-PLAYER-PS5/projects/evoplayer/shaders/agc/ui_screen_2d.pipe` + 生成的 `ui_screen_2d_pipe.h` / `ui_backdrop_blur_pipe.h` | 2D UI pipeline 定义与预生成 ISA（**注意顶点 stride 20**，与 nanovg 的 16 不同） |
| `EVO-PLAYER-PS5/projects/evoplayer/ui_rml/src/evo_rmlui_render_agc.cpp`（+ 头） | 2D 渲染器的逐 draw 组织方式（常量块/描述符/纹理对齐/scissor/裁剪） |
| `EVO-PLAYER-PS5/tools/build_agc_pipes.py`、`tools/patches/llpc-add-gfx1013.py`、`Dockerfile.amdllpc` | 自己的 `.pipe` → `*_pipe.h` 工具链（1 次性，需 `dxc`、`-j4`） |
| `EVO-PLAYER-PS5/tools/native-app/stubs/prx/libSceAgc.syms` / `libSceAgcDriver.syms` + `agc_link_stub.c` / `agc_driver_link_stub.c` | 链接期导入桩（**不要链空桩**，我们已踩过"全黑但无报错"：见 `status.md`「原生标题画面与性能：五个真机定位的坑」首条） |
| `ps5-agc-gears`（全部，3817 行）+ `ps5link-sdk/examples/gpu_cube` | 最小 AGC 上屏样板；`src/ps5_color_target.c` 的 CB 寄存器块构造 |
| `ProsperoLight/src/native_agc_present.cpp` | YUV→RGB + 合成 + flip 的生产级流程（若以后走硬解/视频管线） |
| 只读参考（GPL-2.0，**不可复制代码**）：`SharpProspero`（经 EVO 整理的 ABI/寄存器整理文）、`shadPS4`、`sharpemu`、`AnyPS5` | API 语义、寄存器含义、VideoOut 约束 |

**不要做的事**（已在别处证伪，避免重复投入）：走 Mesa/GL/Vulkan 上 GPU（EVO 实测 ~1.2 s/帧，我们自己也测到 206-369 ms/帧）；指望 VideoOut 多平面合成；指望 payload（hbldr）上下文里用 AGC/硬解。

---

### 附：本笔记引用的本地事实锚点
- 帧预算与瓶颈：`run-continuation/ps5-port-status.md` /「payload 线帧率：30 → 48 fps」「播放器帧率根因：弹幕描边」「原生 GL 路径的性能实测」「批处理优化实验」——主界面 `ui 0.8 / raster 1.6 / flush 17.7` ms，48 fps；全屏播放 1080p 17.3 ms/57.9 fps（`incline` 弹幕），视频页 72-75 ms/13.4 fps。
- 静态层原型（`setStaticLayer` + `clearExcept`）：同文「主页'局部渲染（静态层）'尝试」，已实现且有效（48.7→59.9 fps）但因视觉缺陷回退；其中"保留矩形 + 扫描线补集清屏"的实现思路在阶段 A 直接可用。
- VideoOut 现状：`wiliwili-payload/build-ps5/ps5-sdl-src/src/video/ps5/SDL_ps5video.c:172-181`（格式 `0x8000000022000000`、tiling=0、2 缓冲）、`:237-250`（`AllocateMainDirectMemory` 对齐 0x20000、两缓冲间隔 `memsize/2`）、`:103`（`SubmitFlip` + flip 事件队列）。
- nanovg/nanovg 后端选择点：`library/borealis/library/lib/platforms/sdl/sdl_video.cpp:343-353`；nanovg 的 deko3d 后端 `library/lib/extern/nanovg/deko3d/dk_renderer.cpp` 说明"多后端"是该 fork 既有的扩展方式。
