/*
 * wiliwili PS5 native application - platform glue.
 * Copyright (C) 2026 wiliwili contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Gaps the native runtime does not cover for a statically linked application:
 *
 *  * The application heap helper (ps5-opengl app_heap.c) wraps the C allocation
 *    family and expects the "real" libc entry points behind the wrappers. The
 *    platform exports only the sceLibcMspace* family, so the real entry points
 *    are backed by a second, process-lifetime mspace over an mmap'ed region.
 *
 *  * Mesa's GL dispatch keeps its context in a C++ thread-local; the converter
 *    still needs the compiler's TLS-init entry point to bind.
 *
 *  * Payload libc expects payload-provided helpers (kernel_mprotect, __dl*),
 *    which a sandboxed title cannot offer. They report the unsupported
 *    operation instead of pretending to succeed.
 *
 *  * klog_puts() is part of the payload logging contract (ps5-payload-sdk
 *    ps5/klog.h) and is called by wiliwili's startup logging. Native titles may
 *    be denied the klog service, so the boot log below records progress in the
 *    writable download-data mount instead: a crashed title leaves no stdout and
 *    the coredump path is not always enabled.
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <ucontext.h>
#include <unwind.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <time.h>

int wiliwili_trace_enabled(void);
void wiliwili_note_frame(void);

void *sceLibcMspaceCreate(const char *name, void *base, size_t size,
                          unsigned flags);
void *sceLibcMspaceMalloc(void *mspace, size_t size);
void *sceLibcMspaceCalloc(void *mspace, size_t count, size_t size);
void *sceLibcMspaceRealloc(void *mspace, void *address, size_t size);
void sceLibcMspaceFree(void *mspace, void *address);
int sceLibcMspacePosixMemalign(void *mspace, void **address, size_t alignment,
                              size_t size);
size_t sceLibcMspaceMallocUsableSize(const void *address);

#define REAL_HEAP_SIZE (64u * 1024u * 1024u)

static void *real_heap_base;
static void *real_heap_mspace;

static void *real_mspace(void) {
  if (real_heap_mspace)
    return real_heap_mspace;

  void *base = mmap(NULL, REAL_HEAP_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
  if (base == MAP_FAILED)
    return NULL;

  void *mspace = sceLibcMspaceCreate("wiliwili-real", base, REAL_HEAP_SIZE, 0);
  if (mspace == NULL) {
    munmap(base, REAL_HEAP_SIZE);
    return NULL;
  }

  real_heap_base = base;
  real_heap_mspace = mspace;
  return mspace;
}

void *__real_malloc(size_t size) {
  void *mspace = real_mspace();
  return mspace ? sceLibcMspaceMalloc(mspace, size) : NULL;
}

void *__real_calloc(size_t count, size_t size) {
  void *mspace = real_mspace();
  return mspace ? sceLibcMspaceCalloc(mspace, count, size) : NULL;
}

void *__real_realloc(void *address, size_t size) {
  void *mspace = real_mspace();
  return mspace ? sceLibcMspaceRealloc(mspace, address, size) : NULL;
}

void __real_free(void *address) {
  if (address && real_heap_mspace)
    sceLibcMspaceFree(real_heap_mspace, address);
}

int __real_posix_memalign(void **address, size_t alignment, size_t size) {
  void *mspace = real_mspace();
  if (mspace == NULL)
    return ENOMEM;
  return sceLibcMspacePosixMemalign(mspace, address, alignment, size);
}

size_t __real_malloc_usable_size(const void *address) {
  return address ? sceLibcMspaceMallocUsableSize(address) : 0;
}

/*
 * Mesa's GL dispatch keeps its context in a C++ thread-local. The SDK builds
 * Mesa with emulated TLS, so the compiler's TLS-init entry point is never
 * reached from application code, but the converter still requires a definition
 * to bind. Same definition as ps5-opengl integration/SDL2 and native-app
 * runtime_shims.c (GPL-3.0-or-later).
 */
void wiliwili_mesa_glapi_tls_context_init(void) __asm__(
    "_ZTH23_mesa_glapi_tls_Context");

void wiliwili_mesa_glapi_tls_context_init(void) {}

/*
 * Payload libc's mprotect() reaches for kernel_mprotect() when it has to set
 * execute permission (ps5-payload-sdk ps5/kernel.h, implemented by payload
 * kernel helpers that patch another process' VM). A native title has no such
 * capability, so the helper reports EPERM instead of pretending to succeed.
 */
int kernel_mprotect(int pid, long addr, unsigned long size, int prot) {
  (void)pid;
  (void)addr;
  (void)size;
  (void)prot;
  errno = EPERM;
  return -1;
}

/*
 * Payload libc routes dlopen()/dlsym() through payload-provided __dl* helpers
 * (ps5-payload-sdk dlfcn ABI). A sandboxed title cannot load arbitrary code
 * modules, so they report the unsupported operation. Callers that treat a
 * missing optional library as unavailable (mpv hardware backends, for example)
 * behave correctly; nothing silently succeeds.
 */
void *__dlopen(const char *path, int mode) {
  (void)path;
  (void)mode;
  errno = ENOSYS;
  return NULL;
}

void *__dlsym(void *handle, const char *name) {
  (void)handle;
  (void)name;
  errno = ENOSYS;
  return NULL;
}

int __dlclose(void *handle) {
  (void)handle;
  errno = ENOSYS;
  return -1;
}

char *__dlerror(void) { return (char *)"dynamic loading is unavailable"; }

int __dladdr(const void *address, void *info) {
  (void)address;
  (void)info;
  return 0;
}

/*
 * Boot progress log. A crashed title leaves nothing behind: process stdout is
 * not visible and klog may be denied to a title sandbox. Append to the writable
 * download-data mount (host: /user/download/<TITLE_ID>/) so a failing startup
 * still leaves evidence.
 */
/*
 * The download data area is a filesystem image the console writes back lazily,
 * so a running title is hard to observe. Every line is therefore also sent as a
 * UDP datagram to the development host (see scripts/ps5/native/log-listen.py).
 */
#ifndef WILIWILI_LOG_HOST
#define WILIWILI_LOG_HOST "192.168.100.7"
#endif
#ifndef WILIWILI_LOG_PORT
#define WILIWILI_LOG_PORT 9999
#endif

static unsigned int wiliwili_parse_ipv4_literal(const char *text) {
  unsigned int octets[4] = {0, 0, 0, 0};
  const char *cursor = text;

  for (int index = 0; index < 4; ++index) {
    if (*cursor < '0' || *cursor > '9')
      return 0;
    unsigned int value = 0;
    while (*cursor >= '0' && *cursor <= '9') {
      value = value * 10 + (unsigned int)(*cursor - '0');
      if (value > 255)
        return 0;
      ++cursor;
    }
    octets[index] = value;
    if (index < 3) {
      if (*cursor != '.')
        return 0;
      ++cursor;
    }
  }
  if (*cursor != '\0')
    return 0;
  return (octets[3] << 24) | (octets[2] << 16) | (octets[1] << 8) | octets[0];
}

static void wiliwili_log_stream(const char *message) {
  static int socket_descriptor = -2;
  static struct sockaddr_in target;

  if (socket_descriptor < 0) {
    /* Retried on every line: the first messages are emitted before the network
     * stack is up, and a cached failure would silence the whole stream. */
    socket_descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&target, 0, sizeof(target));
    target.sin_len = sizeof(target);
    target.sin_family = AF_INET;
    target.sin_port =
        (uint16_t)(((WILIWILI_LOG_PORT & 0xff) << 8) | ((WILIWILI_LOG_PORT >> 8) & 0xff));
    target.sin_addr.s_addr = wiliwili_parse_ipv4_literal(WILIWILI_LOG_HOST);
  }
  if (socket_descriptor < 0)
    return;

  (void)sendto(socket_descriptor, message, strlen(message), 0,
               (struct sockaddr *)&target, sizeof(target));
}

void wiliwili_boot_log(const char *message);

/* Every boot records the build it came from: a launch request is silently
 * ignored while another instance owns the display, so stale logs are otherwise
 * indistinguishable from fresh ones. */
static void wiliwili_boot_marker(void) {
  static int written;
  if (written)
    return;
  written = 1;
  wiliwili_boot_log("wiliwili: build " __DATE__ " " __TIME__);
}

#if defined(WILIWILI_OSMESA_PROBE)
/* Software rendering cannot be loaded at runtime (the sandbox refuses dlopen),
 * so OSMesa is linked into the image instead. This probe reports whether a
 * context can be created and made current inside a title. */
extern void *OSMesaCreateContextExt(unsigned int format, int depth_bits,
                                    int stencil_bits, int accum_bits,
                                    void *share_list);
extern void OSMesaDestroyContext(void *context);
extern int OSMesaMakeCurrent(void *context, void *buffer, unsigned int type,
                             int width, int height);
extern const unsigned char *glGetString(unsigned int name);

/* Whether generated code may run at all decides the software rendering stack.
 * llvmpipe JITs every shader, and LLVM's memory manager obtains the pages the
 * same way this probe does: mmap(PROT_READ|PROT_WRITE) first and mprotect to
 * PROT_READ|PROT_EXEC once the code is written (SectionMemoryManager). Mesa's own
 * rtasm instead maps one PROT_EXEC region up front, which this sandbox refuses.
 * So this probe replicates the LLVM order - write "mov eax,42; ret" into a
 * writable mapping, flip it executable, call it - and reports the return value
 * (42) when the sandbox lets it through. */
static void wiliwili_exec_probe(void) {
  unsigned char *code = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON, -1, 0);
  if (code == MAP_FAILED) {
    wiliwili_boot_log("exec: rw mapping failed");
    return;
  }
  code[0] = 0xb8; /* mov eax, 42 */
  code[1] = 0x2a;
  code[2] = 0x00;
  code[3] = 0x00;
  code[4] = 0x00;
  code[5] = 0xc3; /* ret */

  char line[96];
  int status = mprotect(code, 0x1000, PROT_READ | PROT_EXEC);
  snprintf(line, sizeof(line), "exec: mprotect rx=%d errno=%d", status, errno);
  wiliwili_boot_log(line);

  /* Two lines around the call: a process that dies on the jump leaves the first
   * one behind, which is what distinguishes "no execute permission" from a
   * wrong result. */
  wiliwili_boot_log("exec: calling generated code");
  int (*volatile entry)(void) = (int (*)(void))code;
  int result = entry();
  snprintf(line, sizeof(line), "exec: generated code returned %d", result);
  wiliwili_boot_log(line);
  munmap(code, 0x1000);
}

/* Does this sandbox allow making a writable mapping executable? Every
 * llvmpipe shader is JIT'ed into such a mapping, and a page that stays
 * non-executable faults with "read instruction, protection violation" the
 * moment the first shader runs. */
static void wiliwili_wx_probe(void) {
  char line[128];
  void *region = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
  if (region == MAP_FAILED) {
    snprintf(line, sizeof(line), "wx: mmap rw failed errno=%d", errno);
    wiliwili_boot_log(line);
    return;
  }
  int status = mprotect(region, 0x10000, PROT_READ | PROT_EXEC);
  snprintf(line, sizeof(line), "wx: mprotect rx=%d errno=%d", status, errno);
  wiliwili_boot_log(line);

  errno = 0;
  void *executable = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
  snprintf(line, sizeof(line), "wx: mmap rwx=%d errno=%d", executable != MAP_FAILED,
           errno);
  wiliwili_boot_log(line);
  if (executable != MAP_FAILED) {
    /* Whether instructions in a PROT_EXEC mapping may run is the question that
     * decides the software stack, but executing one here kills the process
     * (measured), so the probe only reports the mapping. */
    munmap(executable, 0x10000);
  }
  munmap(region, 0x10000);

  /* A plain mapping cannot be executed here, so the kernel's own object type for
   * code memory is the remaining route; report what a title may create. */
  {
    extern int sceKernelJitCreateSharedMemory(const char *name, size_t size,
                                              int protection, int *descriptor);
    int descriptor = -1;
    int status = sceKernelJitCreateSharedMemory("wiliwili-jit", 0x10000,
                                                PROT_READ | PROT_WRITE | PROT_EXEC,
                                                &descriptor);
    snprintf(line, sizeof(line), "jit: create=%#x fd=%d", status, descriptor);
    wiliwili_boot_log(line);
    if (status == 0 && descriptor >= 0) {
      void *code = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_SHARED, descriptor, 0);
      snprintf(line, sizeof(line), "jit: mmap=%p errno=%d", code, errno);
      wiliwili_boot_log(line);
      if (code != MAP_FAILED) {
        ((unsigned char *)code)[0] = 0xC3;
        wiliwili_boot_log("jit: calling");
        ((void (*)(void))code)();
        wiliwili_boot_log("jit: ran");
      }
    }
  }

  /* Last, because a refusal kills the process instead of returning an error. */
  wiliwili_exec_probe();
}

void wiliwili_osmesa_probe_now(void);

static void wiliwili_osmesa_probe(void) {
  static int done;
  if (done)
    return;
  done = 1;
  wiliwili_boot_log("osmesa: probe enter");


  static unsigned int framebuffer[1920 * 1080];
  char line[192];
  /* OSMesaCreateContextExt builds a core profile context, and softpipe only
   * advertises GLSL 4.00, so the version computation yields 0 and the context
   * is refused. A compatibility profile is accepted at that level. */
  extern void *OSMesaCreateContextAttribs(const int *attrib_list, void *share);
  const int attribs[] = {
      0x33 /* OSMESA_PROFILE */,        0x35 /* OSMESA_COMPAT_PROFILE */,
      0x22 /* OSMESA_FORMAT */,         0x1908 /* OSMESA_RGBA */,
      0x30 /* OSMESA_DEPTH_BITS */,     24,
      0x31 /* OSMESA_STENCIL_BITS */,   8,
      0x32 /* OSMESA_ACCUM_BITS */,     0,
      0};
  void *context = OSMesaCreateContextAttribs(attribs, 0);
  snprintf(line, sizeof(line), "osmesa: context=%p", context);
  wiliwili_boot_log(line);
  if (context == 0) {
    /* Report which step of the front end failed: the manager needs a screen and
     * the state tracker needs its API object. */
    extern void *osmesa_create_screen(void);
    extern void *st_gl_api_create(void);
    void *front_screen = osmesa_create_screen();
    snprintf(line, sizeof(line), "osmesa: osmesa_create_screen=%p", front_screen);
    wiliwili_boot_log(line);
    void *api = st_gl_api_create();
    snprintf(line, sizeof(line), "osmesa: st_gl_api_create=%p", api);
    wiliwili_boot_log(line);
    wiliwili_boot_log("osmesa: probe abort (no context)");
    return;
  }

  int current = OSMesaMakeCurrent(context, framebuffer, 0x1401 /* GL_UNSIGNED_BYTE */,
                                  1920, 1080);
  snprintf(line, sizeof(line), "osmesa: make_current=%d", current);
  wiliwili_boot_log(line);

  const unsigned char *renderer = glGetString(0x1F01 /* GL_RENDERER */);
  snprintf(line, sizeof(line), "osmesa: renderer=%s",
           renderer != 0 ? (const char *)renderer : "(null)");
  wiliwili_boot_log(line);

  OSMesaDestroyContext(context);
  wiliwili_boot_log("osmesa: probe done");
}

void wiliwili_osmesa_probe_now(void) { wiliwili_osmesa_probe(); }

static void wiliwili_osmesa_probe_unused(void) { wiliwili_osmesa_probe(); }
#endif

int pthread_getthreadid_np(void);

/* Which thread writes what: a crash is reported by the kernel with the thread
 * id of the process it killed, and the threads here are unnamed, so the log
 * records the first line each id produces. That is what lets a kernel report be
 * attributed to the render thread, a request, or an image worker. */
static void wiliwili_note_thread(void) {
  static int seen[32];
  static int count;
  int id = pthread_getthreadid_np();
  for (int index = 0; index < count; ++index)
    if (seen[index] == id)
      return;
  if (count < 32)
    seen[count++] = id;

  char line[64];
  char *cursor = line;
  const char *prefix = "thread: id=";
  while (*prefix)
    *cursor++ = *prefix++;
  static const char digits[] = "0123456789";
  char reversed[12];
  int length = 0;
  int value = id;
  do {
    reversed[length++] = digits[value % 10];
    value /= 10;
  } while (value != 0 && length < 12);
  while (length > 0)
    *cursor++ = reversed[--length];
  *cursor = '\0';
  wiliwili_log_stream(line);
}

void wiliwili_boot_log(const char *message) {
  /* ps5-opengl（GL 路线用的 Mesa/AGC 驱动）在绘制热路径里有未加条件的 printf，
   * 例如每个 draw batch 一行 "[ps5-gallium] first-vertex …"、"[ps5-multidraw-batch] …"。
   * 这些行经这里落盘时每行一次 open/write/fsync/close 外加一个 UDP 数据报——真机
   * 实测一分钟 1.6 万行，等于每秒近 300 次 fsync，足以吃掉整个帧预算。驱动噪音
   * 默认丢弃；要看驱动级调试就用 trace 开关（assets/wiliwili-options.txt）。 */
  if (message && strncmp(message, "[ps5-", 5) == 0 && !wiliwili_trace_enabled())
    return;
  /* libcurl 的详细回调（http.hpp 里无条件装的 cpr::DebugCallback）与 DNS 解析
   * 打印都是每请求 20+ 行，而这里每行一次 open/write/fsync/close 外加一个 UDP
   * 数据报：播放页每拉一次评论/封面就要付一次。它们只在查 TLS/DNS 时需要，
   * 因此与驱动噪音同样关到 trace 后面（`http:`/`dns:` 的结论行不受影响，走
   * 各自的上层日志）。 */
  if (message && (strncmp(message, "curl: ", 6) == 0 || strncmp(message, "dns: ", 5) == 0) &&
      !wiliwili_trace_enabled())
    return;

  /* The datagram is sent first: if the download mount is unavailable (a mount
   * the console refused to repair, for instance) the file write fails and the
   * host would otherwise never learn why. */
  wiliwili_note_thread();
  wiliwili_log_stream(message);

  int fd = open("/download0/wiliwili-boot.log", O_WRONLY | O_CREAT | O_APPEND,
                0644);
  if (fd < 0)
    return;
  (void)write(fd, message, strlen(message));
  (void)write(fd, "\n", 1);
  /* The download data area is a filesystem image the console only writes back
   * when the title stops, which makes a running title unobservable. Pushing the
   * file out keeps the log readable while the application is alive. */
  (void)fsync(fd);
  close(fd);
}

/*
 * Per-frame logging is opt-in. Each line costs an open/write/fsync/close on the
 * download image plus a datagram, which is far more than a software rendered
 * frame itself: a traced build drops to about 5 fps, so the render loop stays
 * silent unless WILIWILI_TRACE is set. The application image carries the switch
 * in assets/wiliwili-options.txt (see wiliwili_apply_options), which turns
 * "measure this build" into a repackage instead of a rebuild.
 */
int wiliwili_trace_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = getenv("WILIWILI_TRACE") != NULL ? 1 : 0;
  return enabled;
}

/* Frame phase marks, recorded only while tracing. Slot 0 opens the frame (the
 * application finished recording the previous one), slot 1 is the entry to the
 * present path, slot 2 is the end of the GL submission and slot 3 the completed
 * present: the software renderer rasterises inside the submission, so the deltas
 * separate "the UI is expensive to build" from "the rasteriser is slow" and
 * from "the display path blocks". */
static unsigned long long wiliwili_marks[4];
static unsigned long long wiliwili_previous_first_mark;

static unsigned long long wiliwili_clock_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (unsigned long long)now.tv_sec * 1000000000ull +
         (unsigned long long)now.tv_nsec;
}

void wiliwili_trace_mark(int slot) {
  if (slot < 0 || slot > 3 || !wiliwili_trace_enabled())
    return;
  unsigned long long stamp = wiliwili_clock_ns();
  if (stamp != 0)
    wiliwili_marks[slot] = stamp;
}

/* Frame pacing checkpoint: one line every five seconds while tracing. */
void wiliwili_note_frame(void) {
  static unsigned frames;
  static unsigned long long window_start;

  if (!wiliwili_trace_enabled())
    return;

  unsigned long long stamp = wiliwili_clock_ns();
  if (stamp == 0)
    return;
  if (window_start == 0)
    window_start = stamp;
  ++frames;

  unsigned long long elapsed = stamp - window_start;
  if (elapsed < 5000000000ull)
    return;

  char line[160];
  unsigned long long tenths =
      (unsigned long long)frames * 10000000000ull / elapsed;
  unsigned long long record = wiliwili_previous_first_mark == 0 || wiliwili_marks[0] == 0
                                  ? 0
                                  : (wiliwili_marks[0] - wiliwili_previous_first_mark) / 1000000ull;
  unsigned long long ui = (wiliwili_marks[1] - wiliwili_marks[0]) / 1000000ull;
  unsigned long long raster = (wiliwili_marks[2] - wiliwili_marks[1]) / 1000000ull;
  unsigned long long present = (wiliwili_marks[3] - wiliwili_marks[2]) / 1000000ull;
  snprintf(line, sizeof(line),
           "fps: %llu.%llu frame=%llu ui=%llu raster=%llu present=%llu (ms)",
           tenths / 10, tenths % 10, record, ui, raster, present);
  wiliwili_boot_log(line);
  if (wiliwili_marks[0] != 0)
    wiliwili_previous_first_mark = wiliwili_marks[0];
  frames = 0;
  window_start = stamp;
}

/*
 * A title has no debugger attached and the console only reports "signal 11".
 * Record the faulting address and instruction pointer so the crash can be
 * resolved against the built binary with addr2line.
 */
struct wiliwili_trace_state {
  unsigned long long frames[12];
  int count;
};

static _Unwind_Reason_Code wiliwili_trace_frame(struct _Unwind_Context *context,
                                               void *argument) {
  struct wiliwili_trace_state *state = (struct wiliwili_trace_state *)argument;
  if (state->count >= 12)
    return _URC_END_OF_STACK;
  uintptr_t instruction_pointer = _Unwind_GetIP(context);
  if (instruction_pointer != 0)
    state->frames[state->count++] = (unsigned long long)instruction_pointer;
  return _URC_NO_REASON;
}

static char *wiliwili_append_hex(char *cursor, unsigned long long value) {
  static const char digits[] = "0123456789abcdef";
  char reversed[17];
  int count = 0;
  do {
    reversed[count++] = digits[value & 0xf];
    value >>= 4;
  } while (value != 0 && count < 16);
  *cursor++ = '0';
  *cursor++ = 'x';
  while (count > 0)
    *cursor++ = reversed[--count];
  return cursor;
}


static void wiliwili_crash_handler(int signal_number, siginfo_t *info,
                                   void *context) {
  ucontext_t *ucontext = (ucontext_t *)context;
  unsigned long long fault =
      info != NULL ? (unsigned long long)(uintptr_t)info->si_addr : 0;
  unsigned long long instruction_pointer = 0;
  if (ucontext != NULL) {
    instruction_pointer =
        (unsigned long long)ucontext->uc_mcontext.mc_rip;
  }

  /* The allocator's own accounting says whether a failing allocation returned
   * NULL: that is the difference between a bug in the ported libraries and a
   * heap that ran out. */
  {
    extern void ps5_opengl_heap_snapshot(const char *phase, unsigned iteration);
    ps5_opengl_heap_snapshot("crash", 0);
  }

  char message[256];
  int length = 0;
  message[0] = '\0';
  /* No snprintf here: keep the handler to async-signal-safe calls. */
  const char *prefix = "crash: ";
  while (*prefix && length < (int)sizeof(message) - 1)
    message[length++] = *prefix++;
  /* 信号号：SIGSEGV/SIGBUS/SIGILL/SIGFPE 的处理是同一个，但它们的含义完全不同
   * （段错误 vs 非法指令），之前只记地址无法分辨。 */
  if (signal_number >= 0 && signal_number <= 99) {
    if (signal_number >= 10)
      message[length++] = (char)('0' + signal_number / 10);
    message[length++] = (char)('0' + signal_number % 10);
    message[length++] = ' ';
  }
  /* si_code：SIGBUS 的 BUS_ADRALN/BUS_ADRERR/BUS_OBJERR 指向不同的根因
   * （对齐 / 映射之外 / 对象错误），没有它只能猜。 */
  {
    long long code = info != NULL ? (long long)info->si_code : 0;
    message[length++] = 'c';
    message[length++] = '=';
    if (code < 0) {
      message[length++] = '-';
      code = -code;
    }
    char reversed[12];
    int digits = 0;
    do {
      reversed[digits++] = (char)('0' + (int)(code % 10));
      code /= 10;
    } while (code != 0 && digits < 11);
    while (digits > 0)
      message[length++] = reversed[--digits];
    message[length++] = ' ';
  }
  static const char digits[] = "0123456789abcdef";
  extern void *malloc(size_t);
  extern int pthread_create(void *, const void *, void *(*)(void *), void *);
  const char *labels[5] = {"addr=0x", " rip=0x", " base=0x", " rsp=0x", " rb=0x"};
  const unsigned long long values[5] = {
      fault,
      instruction_pointer,
      (unsigned long long)(uintptr_t)&wiliwili_boot_log,
      ucontext != NULL ? (unsigned long long)ucontext->uc_mcontext.mc_rsp : 0,
      ucontext != NULL ? (unsigned long long)ucontext->uc_mcontext.mc_rbp : 0};
  for (int i = 0; i < 5; ++i) {
    for (const char *label = labels[i]; *label && length < (int)sizeof(message) - 1;)
      message[length++] = *label++;
    char reversed[17];
    int count = 0;
    unsigned long long value = values[i];
    do {
      reversed[count++] = digits[value & 0xf];
      value >>= 4;
    } while (value != 0 && count < 16);
    while (count > 0 && length < (int)sizeof(message) - 1)
      message[length++] = reversed[--count];
  }
  message[length] = '\0';
  wiliwili_boot_log(message);

  struct wiliwili_trace_state trace;
  trace.count = 0;
  _Unwind_Backtrace(wiliwili_trace_frame, &trace);

  char line[512];
  char *cursor = line;
  const char *header = "bt:";
  while (*header)
    *cursor++ = *header++;
  for (int i = 0; i < trace.count; ++i) {
    *cursor++ = ' ';
    cursor = wiliwili_append_hex(cursor, trace.frames[i]);
  }
  *cursor = '\0';
  wiliwili_boot_log(line);

  /* The kernel does not fill the user context for a title, so the call chain
   * has to come out of the stack: the words are printed with their addresses,
   * which lets the host line them up with the faulting stack pointer the kernel
   * reports for the same run and walk the return addresses from there. */
  {
    const unsigned long long *stack =
        (const unsigned long long *)(uintptr_t)&stack;
    for (int chunk = 0; chunk < 64; ++chunk) {
      char frame_line[400];
      char *frame_cursor = frame_line;
      const char *frame_header = "stk:";
      while (*frame_header)
        *frame_cursor++ = *frame_header++;
      for (int i = 0; i < 8; ++i) {
        unsigned long long slot = (unsigned long long)(uintptr_t)&stack[chunk * 8 + i];
        *frame_cursor++ = ' ';
        frame_cursor = wiliwili_append_hex(frame_cursor, slot);
        *frame_cursor++ = '=';
        frame_cursor = wiliwili_append_hex(frame_cursor,
                                           stack[chunk * 8 + i]);
      }
      *frame_cursor = '\0';
      wiliwili_boot_log(frame_line);
    }
  }

  signal(signal_number, SIG_DFL);
  raise(signal_number);
}

__attribute__((constructor(102))) static void wiliwili_install_crash_handler(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = wiliwili_crash_handler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, NULL);
  sigaction(SIGBUS, &action, NULL);
  sigaction(SIGILL, &action, NULL);
  sigaction(SIGFPE, &action, NULL);
  sigaction(SIGABRT, &action, NULL);
}

/* Runs from .preinit_array, i.e. before every static constructor: it separates
 * "the image never started" from "a constructor faulted". */
/* Mesa and LLVM read their switches from the environment, and a title cannot be
 * given one: the only writable place is the download mount. The switches are
 * therefore carried in the application image, as KEY=VALUE lines in
 * assets/wiliwili-options.txt, which turns "try another Mesa option" into a
 * repackage instead of a rebuild. */
static void wiliwili_apply_options(void) {
  int fd = open("/app0/assets/wiliwili-options.txt", O_RDONLY, 0);
  if (fd < 0)
    return;

  char data[1024];
  long size = read(fd, data, sizeof(data) - 1);
  close(fd);
  if (size <= 0)
    return;
  data[size] = '\0';

  char *line = data;
  while (*line != '\0') {
    char *end = line;
    while (*end != '\0' && *end != '\n')
      ++end;
    char saved = *end;
    *end = '\0';
    if (line[0] != '#' && line[0] != '\0') {
      char *entry = malloc((size_t)(end - line) + 1);
      if (entry != 0) {
        memcpy(entry, line, (size_t)(end - line) + 1);
        putenv(entry);
        wiliwili_boot_log(entry);
      }
    }
    if (saved == '\0')
      break;
    line = end + 1;
  }
}

static void wiliwili_early_marker(void) {
  wiliwili_apply_options();
  /* ps5-opengl（GL 路线的 Mesa 驱动）在绘制热路径里有未加条件的 printf
   * （例如 ps5_screen.c 的 "[ps5-gallium] first-vertex"、"[ps5-multidraw-batch]"），
   * 每个 draw batch 都写一次 stdout，直接把帧率拖垮。正式日志走
   * wiliwili_boot_log（/download0/wiliwili-boot.log + UDP:9999），所以默认把
   * stdout 丢掉。
   *
   * 但驱动自己的诊断计数只走 stdout（`[ps5-driver-cycles]` 相位周期、
   * `[ps5-cpu-flush-summary]` 的 flush 字节数），标题的 stdout 默认无处可去，
   * 所以要取证就只能先落盘：WILIWILI_CAPTURE_STDOUT=1（或 trace）时写进
   * /download0/wiliwili-stdout.log，标题停止后镜像写回、用 read-download0.sh 取回。 */
  if (wiliwili_trace_enabled() || getenv("WILIWILI_CAPTURE_STDOUT") != 0) {
    /* 行缓冲：标题崩溃时块缓冲里的内容会全部丢掉（第一次取回来是空文件）。 */
    if (freopen("/download0/wiliwili-stdout.log", "w", stdout) != 0)
      setvbuf(stdout, NULL, _IOLBF, 0);
  } else {
    (void)freopen("/dev/null", "w", stdout);
  }
  /* Capability probes describe the sandbox, not the application: they are
   * diagnostics and stay behind the trace switch, the executable-memory one
   * especially (executing generated code is a hard failure where the sandbox
   * refuses it, which would take the whole title down before main). */
#if defined(WILIWILI_OSMESA_PROBE)
  /* 该探针只对软渲染（llvmpipe JIT）有意义，定义也在同一个宏里；
   * GL 路线不定义它，因此调用点必须同样受守卫，否则编译不过。 */
  if (wiliwili_trace_enabled())
    wiliwili_wx_probe();
#endif
  /* Mesa computes its GL version from driver capabilities and refuses to create
   * the context when the result is 0 (a core profile request against softpipe's
   * GLSL 4.00). Pinning the version keeps the software path usable.
   *
   * llvmpipe rasterises across a pool of worker threads. Pinning that pool to a
   * single thread (LP_NUM_THREADS=1) is correct but costs three quarters of the
   * frame rate on this console - measured 7.4 fps against 30 fps with the pool
   * left at llvmpipe's own default - so the switch is deliberately not set here;
   * assets/wiliwili-options.txt can still pin it for experiments. */
  if (getenv("MESA_GL_VERSION_OVERRIDE") == 0) {
    char *override = malloc(32);
    if (override != 0) {
      memcpy(override, "MESA_GL_VERSION_OVERRIDE=3.3", 29);
      putenv(override);
    }
  }
  wiliwili_boot_log("wiliwili: preinit");
}

__attribute__((section(".preinit_array"), used))
static void (*wiliwili_early_marker_entry)(void) = wiliwili_early_marker;

__attribute__((constructor(101))) static void wiliwili_boot_log_start(void) {
  wiliwili_boot_marker();
  wiliwili_boot_log("constructors: start");
}

__attribute__((constructor(65535))) static void wiliwili_boot_log_done(void) {
  wiliwili_boot_log("constructors: done");
}


/* The port imports libSceKeyboard, libSceImeDialog and libScePosixForWebKit:
 * the firmware ships the first two but preloads neither into a title, and a
 * title cannot load them - sweeping every sceSysmoduleLoadModule id (0x00-0xff)
 * never resolved the keyboard import, so the SDL video driver is built without
 * the keyboard subsystem instead (SDL_PS5_NO_KEYBOARD_MODULE). The port's own
 * getaddrinfo replacement covers the third. */


/*
 * klog: the payload logging service is reached through syscall 0x259, which a
 * title sandbox denies - the kernel kills the calling process instead of
 * returning an error. Payload builds get these messages through the klog
 * server; a title records them in the same boot log as the startup
 * checkpoints, which is readable through the download-data image.
 */
int klog_puts(const char *s) {
  if (s == NULL)
    return EINVAL;
  wiliwili_boot_log(s);
  return 0;
}
