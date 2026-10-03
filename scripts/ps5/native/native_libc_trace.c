/*
 * Null-argument trace for the string and memory functions.
 *
 * The crash inside the software renderer reports nothing but a fault at address
 * zero with every argument register cleared, which means some libc call was
 * entered with NULL arguments and dereferenced them immediately. The functions
 * below are wrapped (--wrap) so such a call names itself in the boot log before
 * the fault happens: the wrapper reports the call and then performs it
 * unchanged.
 *
 * This file is a diagnostic build aid, not part of the runtime.
 */
#include <errno.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void wiliwili_boot_log(const char *message);

void *__real_memcpy(void *destination, const void *source, size_t size);
void *__real_memmove(void *destination, const void *source, size_t size);
int __real_memcmp(const void *left, const void *right, size_t size);
void *__real_memchr(const void *block, int value, size_t size);
size_t __real_strlen(const char *text);
int __real_strcmp(const char *left, const char *right);
int __real_strncmp(const char *left, const char *right, size_t size);
char *__real_strchr(const char *text, int character);
char *__real_strstr(const char *haystack, const char *needle);
size_t __real_strnlen(const char *text, size_t limit);
size_t __real_strlcpy(char *destination, const char *source, size_t size);
size_t __real_strlcat(char *destination, const char *source, size_t size);
char *__real_strcpy(char *destination, const char *source);
char *__real_strncpy(char *destination, const char *source, size_t size);
char *__real_strcat(char *destination, const char *source);
int __real_memset(void *destination, int value, size_t size);
int __real___srget(void *stream);
int __real___swbuf(int character, void *stream);
int __real_mprotect(void *address, size_t size, int protection);
void *__real_mmap(void *address, size_t size, int protection, int flags,
                  int descriptor, long offset);
int __real_munmap(void *address, size_t size);
int __real_fprintf(void *stream, const char *format, ...);
int __real_vfprintf(void *stream, const char *format, va_list arguments);
int __real_vsnprintf(char *buffer, size_t size, const char *format,
                     va_list arguments);
size_t __real_fwrite(const void *block, size_t size, size_t count,
                     void *stream);
size_t __real_fread(void *block, size_t size, size_t count, void *stream);
int __real_fputs(const char *text, void *stream);
void *__real_fopen(const char *path, const char *mode);
int __real_fclose(void *stream);
int __real_fflush(void *stream);

static void wiliwili_note_arguments(const char *name, const void *first,
                                    const void *second, size_t size,
                                    int first_is_argument) {
  if (!first_is_argument && first != NULL)
    return;

  static unsigned reported;
  if (reported >= 32)
    return;
  ++reported;

  char message[160];
  snprintf(message, sizeof(message), "libc-null: %s(%s%s%s, %s%s%s) size=%llu",
           name, first == NULL ? "NULL" : "ptr",
           first == NULL ? "" : "@", first == NULL ? "" : "0x",
           second == NULL ? "NULL" : "ptr", second == NULL ? "" : "@",
           second == NULL ? "" : "0x",
           (unsigned long long)size);
  (void)first_is_argument;
  wiliwili_boot_log(message);
  (void)first;
  (void)second;
}

void *__wrap_memcpy(void *destination, const void *source, size_t size) {
  if (destination == NULL || source == NULL) {
    wiliwili_note_arguments("memcpy", destination, source, size, 1);
    return destination; /* skipped: performing it would fault at address zero */
  }
  return __real_memcpy(destination, source, size);
}

void *__wrap_memmove(void *destination, const void *source, size_t size) {
  if (destination == NULL || source == NULL) {
    wiliwili_note_arguments("memmove", destination, source, size, 1);
    return destination;
  }
  return __real_memmove(destination, source, size);
}

int __wrap_memcmp(const void *left, const void *right, size_t size) {
  if (left == NULL || right == NULL) {
    wiliwili_note_arguments("memcmp", left, right, size, 1);
    return 0;
  }
  return __real_memcmp(left, right, size);
}

void *__wrap_memchr(const void *block, int value, size_t size) {
  if (block == NULL) {
    wiliwili_note_arguments("memchr", block, NULL, size, 1);
    return NULL;
  }
  return __real_memchr(block, value, size);
}

size_t __wrap_strlen(const char *text) {
  if (text == NULL) {
    wiliwili_note_arguments("strlen", NULL, NULL, 0, 1);
    return 0;
  }
  return __real_strlen(text);
}

int __wrap_strcmp(const char *left, const char *right) {
  if (left == NULL || right == NULL) {
    wiliwili_note_arguments("strcmp", left, right, 0, 1);
    if (left == NULL && right == NULL)
      return 0;
    return left == NULL ? -1 : 1;
  }
  return __real_strcmp(left, right);
}

int __wrap_strncmp(const char *left, const char *right, size_t size) {
  if (left == NULL || right == NULL) {
    wiliwili_note_arguments("strncmp", left, right, size, 1);
    return 0;
  }
  return __real_strncmp(left, right, size);
}

char *__wrap_strchr(const char *text, int character) {
  if (text == NULL) {
    wiliwili_note_arguments("strchr", NULL, NULL, (size_t)character, 1);
    return NULL;
  }
  return __real_strchr(text, character);
}

/* The software renderer dies with four cleared argument registers, which is the
 * shape of a stdio call made with a NULL stream or a NULL format. These wrappers
 * name it; the call itself is skipped so the process keeps running. */
int __wrap_vsnprintf(char *buffer, size_t size, const char *format,
                     va_list arguments) {
  if (format == NULL || (buffer == NULL && size != 0)) {
    wiliwili_note_arguments("vsnprintf", buffer, format, size, 1);
    return 0;
  }
  return __real_vsnprintf(buffer, size, format, arguments);
}

size_t __wrap_fwrite(const void *block, size_t size, size_t count, void *stream) {
  if (stream == NULL || (block == NULL && size != 0 && count != 0)) {
    wiliwili_note_arguments("fwrite", block, stream, size * count, 1);
    return 0;
  }
  return __real_fwrite(block, size, count, stream);
}

size_t __wrap_fread(void *block, size_t size, size_t count, void *stream) {
  if (stream == NULL || (block == NULL && size != 0 && count != 0)) {
    wiliwili_note_arguments("fread", block, stream, size * count, 1);
    return 0;
  }
  return __real_fread(block, size, count, stream);
}

int __wrap_fputs(const char *text, void *stream) {
  if (stream == NULL || text == NULL) {
    wiliwili_note_arguments("fputs", text, stream, 0, 1);
    return -1;
  }
  return __real_fputs(text, stream);
}

int __wrap_vfprintf(void *stream, const char *format, va_list arguments) {
  if (stream == NULL || format == NULL) {
    wiliwili_note_arguments("vfprintf", stream, format, 0, 1);
    return -1;
  }
  return __real_vfprintf(stream, format, arguments);
}

int __wrap_fprintf(void *stream, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  int result = __wrap_vfprintf(stream, format, arguments);
  va_end(arguments);
  return result;
}

/* 驱动的诊断输出走 printf（`[ps5-driver-cycles]` 相位计数、`[ps5-cpu-flush-summary]`
 * 等），而标题的 stdout 默认被丢掉（见 native_shims.c 的 freopen）：把这个引用
 * 接到日志管线上，驱动自己的数字就能在 UDP/文件日志里取到。仅诊断构建
 * （PS5_NATIVE_LIBC_TRACE=1）启用，正式包不带这个包装。 */
int __wrap_printf(const char *format, ...) {
  if (format == NULL)
    return -1;
  char message[256];
  va_list arguments;
  va_start(arguments, format);
  int written = vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  if (written > 0 && message[0] != '\0')
    wiliwili_boot_log(message);
  return written;
}

void *__wrap_fopen(const char *path, const char *mode) {
  if (path == NULL || mode == NULL) {
    wiliwili_note_arguments("fopen", path, mode, 0, 1);
    return NULL;
  }
  return __real_fopen(path, mode);
}

int __wrap_fclose(void *stream) {
  if (stream == NULL)
    return 0;
  return __real_fclose(stream);
}

int __wrap_fflush(void *stream) {
  /* fflush(NULL) is a legal flush-everything request. */
  if (stream == NULL)
    return 0;
  return __real_fflush(stream);
}

void *__wrap_memset(void *destination, int value, size_t size) {
  if (destination == NULL && size != 0) {
    wiliwili_note_arguments("memset", destination, NULL, size, 1);
    return destination;
  }
  return (void *)__real_memset(destination, value, size);
}

char *__wrap_strstr(const char *haystack, const char *needle) {
  if (haystack == NULL || needle == NULL) {
    wiliwili_note_arguments("strstr", haystack, needle, 0, 1);
    return NULL;
  }
  return __real_strstr(haystack, needle);
}

size_t __wrap_strnlen(const char *text, size_t limit) {
  if (text == NULL) {
    wiliwili_note_arguments("strnlen", NULL, NULL, limit, 1);
    return 0;
  }
  return __real_strnlen(text, limit);
}

size_t __wrap_strlcpy(char *destination, const char *source, size_t size) {
  if (source == NULL || destination == NULL) {
    wiliwili_note_arguments("strlcpy", destination, source, size, 1);
    return 0;
  }
  return __real_strlcpy(destination, source, size);
}

size_t __wrap_strlcat(char *destination, const char *source, size_t size) {
  if (source == NULL || destination == NULL) {
    wiliwili_note_arguments("strlcat", destination, source, size, 1);
    return 0;
  }
  return __real_strlcat(destination, source, size);
}

char *__wrap_strcpy(char *destination, const char *source) {
  if (source == NULL || destination == NULL) {
    wiliwili_note_arguments("strcpy", destination, source, 0, 1);
    return destination;
  }
  return __real_strcpy(destination, source);
}

char *__wrap_strncpy(char *destination, const char *source, size_t size) {
  if (source == NULL || destination == NULL) {
    wiliwili_note_arguments("strncpy", destination, source, size, 1);
    return destination;
  }
  return __real_strncpy(destination, source, size);
}

char *__wrap_strcat(char *destination, const char *source) {
  if (source == NULL || destination == NULL) {
    wiliwili_note_arguments("strcat", destination, source, 0, 1);
    return destination;
  }
  return __real_strcat(destination, source);
}

/* stdio internals reached by the getc/putc macros rather than by getc(). */
int __wrap___srget(void *stream) {
  if (stream == NULL) {
    wiliwili_note_arguments("__srget", NULL, NULL, 0, 1);
    return -1;
  }
  return __real___srget(stream);
}

int __wrap___swbuf(int character, void *stream) {
  if (stream == NULL) {
    wiliwili_note_arguments("__swbuf", NULL, NULL, (size_t)character, 1);
    return -1;
  }
  return __real___swbuf(character, stream);
}

/* Code pages must be flipped from writable to executable before they run; a
 * JIT that skips or fails that step executes a protected page and faults with
 * "read instruction". Report every transition so the JIT memory can be
 * followed. */
int __wrap_mprotect(void *address, size_t size, int protection) {
  int result = __real_mprotect(address, size, protection);
  static unsigned reported;
  if (reported < 48) {
    ++reported;
    char message[160];
    snprintf(message, sizeof(message),
             "mprotect(%p, %#llx, %#x) = %d errno=%d", address,
             (unsigned long long)size, protection, result, errno);
    wiliwili_boot_log(message);
  }
  return result;
}

int __wrap_munmap(void *address, size_t size) {
  int result = __real_munmap(address, size);
  static unsigned reported;
  if (reported < 24) {
    ++reported;
    char message[160];
    snprintf(message, sizeof(message), "munmap(%p, %#llx) = %d errno=%d", address,
             (unsigned long long)size, result, errno);
    wiliwili_boot_log(message);
  }
  return result;
}

/* The JIT asks for its code memory here; the protection it requests and the
 * address it gets are what the mprotect trace above has to be read against. */
void *__wrap_mmap(void *address, size_t size, int protection, int flags,
                  int descriptor, long offset) {
  void *result = __real_mmap(address, size, protection, flags, descriptor, offset);
  static unsigned reported;
  if (size >= 0x1000 && reported < 64) {
    ++reported;
    char message[192];
    snprintf(message, sizeof(message),
             "mmap(%p, %#llx, prot=%#x, flags=%#x, fd=%d) = %p errno=%d", address,
             (unsigned long long)size, protection, flags, descriptor, result, errno);
    wiliwili_boot_log(message);
  }
  return result;
}
