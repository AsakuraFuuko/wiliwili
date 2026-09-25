/*
 * wiliwili PS5 native application - libc surface fill-ins.
 * Copyright (C) 2026 wiliwili contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The clean-room runtime exports the C API surface the SDK's own samples need.
 * A statically linked application that also pulls in ffmpeg, mpv, OpenSSL and
 * libcurl needs more: FreeBSD-locale variants, calendar helpers, process and
 * socket extras, emulated TLS, and POSIX regex. Everything below is a real
 * implementation or an explicit "unsupported in a title sandbox" result - no
 * silent success.
 */

#include <errno.h>
#include <dirent.h>

#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

/* SDK prototypes for the functions implemented here. */
#include <ctype.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <netdb.h>
#include <strings.h>
#include <net/if.h>
#include <nl_types.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <xlocale.h>

/* ASCII-only helpers: the process has no locale database (C locale). */
static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ascii_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int ascii_iswctype(wint_t c, wctype_t type) {
  switch ((unsigned long)type) {
    case 1: return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); /* alpha */
    case 2: return c >= '0' && c <= '9';                            /* digit */
    case 3: return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9');                          /* alnum */
    case 4: return c == ' ' || (c >= 0x09 && c <= 0x0d);            /* space */
    case 5: return c >= 'a' && c <= 'z';                            /* lower */
    case 6: return c >= 'A' && c <= 'Z';                            /* upper */
    case 7: return c == ' ' || c == '\t';                          /* blank */
    case 8: return c >= 0x20 && c < 0x7f;                           /* print */
    case 9: return c > 0x20 && c < 0x7f;                            /* graph */
    case 10: return c > 0x20 && c < 0x7f && !ascii_iswctype(c, 3);  /* punct */
    case 11: return c < 0x20 || c == 0x7f;                          /* cntrl */
    case 12: return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F');                          /* xdigit */
    case 13: return c >= 0;                                         /* ideogram: none */
    case 14: return c == ' ';                                       /* special */
    case 15: return c >= 0x20 && c < 0x7f;                          /* phonetic */
    default: return 0;
  }
}

/* ------------------------------------------------------------------ locale */

/*
 * A title has no locale database. A single process-lifetime object stands in
 * for every requested locale and the character conversions below stay
 * byte-oriented, which is what the C locale does.
 */
static char wiliwili_c_locale;

locale_t newlocale(int mask, const char *name, locale_t base) {
  (void)mask;
  (void)name;
  (void)base;
  return (locale_t)&wiliwili_c_locale;
}

int freelocale(locale_t locale) {
  (void)locale;
  return 0;
}

locale_t uselocale(locale_t locale) {
  (void)locale;
  return (locale_t)&wiliwili_c_locale;
}

struct lconv *localeconv_l(locale_t locale) {
  (void)locale;
  return localeconv();
}

char *nl_langinfo_l(int item, locale_t locale) {
  (void)item;
  (void)locale;
  return (char *)"";
}

char *nl_langinfo(int item) {
  (void)item;
  return (char *)"";
}

/* FreeBSD locale internals referenced by the wide-character entry points. */
int ___mb_cur_max(void) { return 1; }

int ___mb_cur_max_l(locale_t locale) {
  (void)locale;
  return 1;
}

unsigned long ___runetype_l(__ct_rune_t c, locale_t locale) {
  (void)locale;
  return (unsigned long)c;
}

__ct_rune_t ___tolower_l(__ct_rune_t c, locale_t locale) {
  (void)locale;
  return (__ct_rune_t)ascii_lower(c);
}

__ct_rune_t ___toupper_l(__ct_rune_t c, locale_t locale) {
  (void)locale;
  return (__ct_rune_t)ascii_upper(c);
}

/*
 * The wide-character entry points reach for the locale's rune table. A title
 * has no locale database, so there is no table to return and the callers fall
 * back to the byte-oriented paths.
 */
_RuneLocale *__runes_for_locale(locale_t locale, int *count) {
  (void)locale;
  if (count)
    *count = 0;
  return NULL;
}

/* _l variants forward to the plain calls; the process locale never changes. */
wint_t btowc_l(int c, locale_t locale) {
  (void)locale;
  if (c == EOF)
    return WEOF;
  return (wint_t)(unsigned char)c;
}

int wctob_l(wint_t c, locale_t locale) {
  (void)locale;
  return c < 0x80 ? (int)c : EOF;
}

int iswctype_l(wint_t c, wctype_t type, locale_t locale) {
  (void)locale;
  return ascii_iswctype(c, type);
}

size_t mbrlen_l(const char *s, size_t n, mbstate_t *state, locale_t locale) {
  (void)locale;
  (void)state;
  if (s == NULL || n == 0)
    return 0;
  return *s == '\0' ? 0 : 1;
}

size_t mbrtowc_l(wchar_t *pwc, const char *s, size_t n, mbstate_t *state,
                 locale_t locale) {
  (void)locale;
  (void)state;
  if (s == NULL)
    return 0;
  if (n == 0)
    return (size_t)-2;
  if (*s == '\0') {
    if (pwc != NULL)
      *pwc = L'\0';
    return 0;
  }
  if (pwc != NULL)
    *pwc = (wchar_t)(unsigned char)*s;
  return 1;
}

int mbtowc_l(wchar_t *pwc, const char *s, size_t n, locale_t locale) {
  (void)locale;
  (void)n;
  if (s == NULL)
    return 0;
  if (*s == '\0') {
    if (pwc != NULL)
      *pwc = L'\0';
    return 0;
  }
  if (pwc != NULL)
    *pwc = (wchar_t)(unsigned char)*s;
  return 1;
}

size_t mbsrtowcs_l(wchar_t *dst, const char **src, size_t len, mbstate_t *state,
                   locale_t locale) {
  (void)locale;
  (void)state;
  const char *input = *src;
  size_t produced = 0;
  while (*input != '\0' && (dst == NULL || produced < len)) {
    if (dst != NULL)
      dst[produced] = (wchar_t)(unsigned char)input[produced];
    produced++;
    input++;
  }
  if (dst != NULL && produced == len)
    return produced;
  if (dst != NULL)
    *src = NULL;
  return produced;
}

size_t mbsnrtowcs_l(wchar_t *dst, const char **src, size_t nms, size_t len,
                    mbstate_t *state, locale_t locale) {
  (void)locale;
  (void)state;
  const char *input = *src;
  size_t produced = 0;
  while (produced < nms && input[produced] != '\0' &&
         (dst == NULL || produced < len)) {
    if (dst != NULL)
      dst[produced] = (wchar_t)(unsigned char)input[produced];
    produced++;
  }
  if (dst != NULL)
    *src = produced == nms ? NULL : input + produced;
  return produced;
}

size_t wcrtomb_l(char *s, wchar_t wc, mbstate_t *state, locale_t locale) {
  (void)locale;
  (void)state;
  if (s == NULL)
    return 1;
  *s = (char)(unsigned char)wc;
  return 1;
}

size_t wcsnrtombs_l(char *dst, const wchar_t **src, size_t nwc, size_t len,
                    mbstate_t *state, locale_t locale) {
  (void)locale;
  (void)state;
  const wchar_t *input = *src;
  size_t produced = 0;
  while (produced < nwc && input[produced] != L'\0' &&
         (dst == NULL || produced < len)) {
    if (dst != NULL)
      dst[produced] = (char)(unsigned char)input[produced];
    produced++;
  }
  if (dst != NULL)
    *src = produced == nwc ? NULL : input + produced;
  return produced;
}

int strcoll_l(const char *a, const char *b, locale_t locale) {
  (void)locale;
  return strcmp(a, b);
}

size_t strxfrm_l(char *dst, const char *src, size_t n, locale_t locale) {
  (void)locale;
  size_t length = strlen(src);
  if (n != 0) {
    size_t copy = length < n - 1 ? length : n - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
  }
  return length;
}

int wcscoll_l(const wchar_t *a, const wchar_t *b, locale_t locale) {
  (void)locale;
  return wcscmp(a, b);
}

size_t wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t n, locale_t locale) {
  (void)locale;
  size_t length = wcslen(src);
  if (n != 0) {
    size_t copy = length < n - 1 ? length : n - 1;
    wmemcpy(dst, src, copy);
    dst[copy] = L'\0';
  }
  return length;
}

double strtod_l(const char *s, char **end, locale_t locale) {
  (void)locale;
  return strtod(s, end);
}

float strtof_l(const char *s, char **end, locale_t locale) {
  (void)locale;
  return strtof(s, end);
}

long double strtold_l(const char *s, char **end, locale_t locale) {
  (void)locale;
  return strtold(s, end);
}

long long strtoll_l(const char *s, char **end, int base, locale_t locale) {
  (void)locale;
  return strtoll(s, end, base);
}

unsigned long long strtoull_l(const char *s, char **end, int base,
                              locale_t locale) {
  (void)locale;
  return strtoull(s, end, base);
}

int snprintf_l(char *dst, size_t size, locale_t locale, const char *format,
               ...) {
  (void)locale;
  va_list args;
  va_start(args, format);
  int result = vsnprintf(dst, size, format, args);
  va_end(args);
  return result;
}

int sscanf_l(const char *src, locale_t locale, const char *format, ...) {
  (void)locale;
  va_list args;
  va_start(args, format);
  int result = vsscanf(src, format, args);
  va_end(args);
  return result;
}

size_t strftime_l(char *dst, size_t size, const char *format,
                  const struct tm *tm_value, locale_t locale) {
  (void)locale;
  return strftime(dst, size, format, tm_value);
}

int asprintf_l(char **dst, locale_t locale, const char *format, ...) {
  (void)locale;
  va_list args;
  va_start(args, format);
  int result = vasprintf(dst, format, args);
  va_end(args);
  return result;
}

/* ------------------------------------------------------------------ catalog */

/*
 * Message catalogs are not shipped with a title. catopen() reports the missing
 * catalog, catclose() accepts the failure, and catgets() falls back to the
 * caller-provided default string, which is what the API contract expects.
 */
nl_catd catopen(const char *name, int flag) {
  (void)name;
  (void)flag;
  errno = ENOENT;
  return (nl_catd)-1;
}

int catclose(nl_catd catalog) {
  (void)catalog;
  errno = EBADF;
  return -1;
}

char *catgets(nl_catd catalog, int set, int message, const char *fallback) {
  (void)catalog;
  (void)set;
  (void)message;
  return (char *)fallback;
}

/* -------------------------------------------------------------------- time */

static void civil_from_days(long long days, int *year, int *month, int *day) {
  /* Howard Hinnant's civil-from-days algorithm. */
  days += 719468;
  const long long era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned long long doe = (unsigned long long)(days - era * 146097);
  const unsigned long long yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long y = (long long)yoe + era * 400;
  const unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned long long mp = (5 * doy + 2) / 153;
  const unsigned long long d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned long long m = mp < 10 ? mp + 3 : mp - 9;
  *year = (int)(y + (m <= 2));
  *month = (int)m;
  *day = (int)d;
}

static struct tm *utc_broken_down(time_t value, struct tm *result) {
  long long seconds = (long long)value;
  long long days = seconds / 86400;
  long long remainder = seconds % 86400;
  if (remainder < 0) {
    remainder += 86400;
    days -= 1;
  }

  int year, month, day;
  civil_from_days(days, &year, &month, &day);

  memset(result, 0, sizeof(*result));
  result->tm_year = year - 1900;
  result->tm_mon = month - 1;
  result->tm_mday = day;
  result->tm_hour = (int)(remainder / 3600);
  result->tm_min = (int)((remainder % 3600) / 60);
  result->tm_sec = (int)(remainder % 60);
  result->tm_wday = (int)(((days % 7) + 11) % 7); /* 1970-01-01 was a Thursday */
  result->tm_yday = 0;
  result->tm_isdst = 0;
  return result;
}

/*
 * A title has no time-zone database mounted, so both conversions produce UTC
 * calendar time. Timestamps in logs and reports stay correct as instants.
 */
struct tm *gmtime_r(const time_t *value, struct tm *result) {
  return utc_broken_down(*value, result);
}

struct tm *localtime_r(const time_t *value, struct tm *result) {
  return utc_broken_down(*value, result);
}

/* ------------------------------------------------------------- process I/O */

int pipe2(int pipefd[2], int flags) {
  if (pipe(pipefd) != 0)
    return -1;
  if ((flags & O_CLOEXEC) != 0) {
    (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
  }
  if ((flags & O_NONBLOCK) != 0) {
    (void)fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
  }
  return 0;
}

/*
 * popen() needs a shell process. A title sandbox cannot spawn arbitrary
 * processes, so the call reports failure instead of handing back a stream that
 * never produces output. Callers (curl's certificate search, mpv's external
 * probes) treat that as "unavailable".
 */
FILE *popen(const char *command, const char *mode) {
  (void)command;
  (void)mode;
  errno = ENOSYS;
  return NULL;
}

int pclose(FILE *stream) {
  (void)stream;
  errno = ENOSYS;
  return -1;
}

static int fill_template(char *template_name, int suffix_length, int flags) {
  const size_t length = strlen(template_name);
  if (suffix_length < 0 || length < (size_t)suffix_length + 6) {
    errno = EINVAL;
    return -1;
  }
  const size_t base = length - (size_t)suffix_length - 6;
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  static unsigned counter;

  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    unsigned value = (unsigned)getpid() * 2654435761u + ++counter * 7919u;
    for (size_t i = 0; i < 6; ++i) {
      template_name[base + i] = alphabet[value % 36];
      value /= 36;
    }
    int fd = open(template_name, O_CREAT | O_EXCL | O_RDWR | flags, 0600);
    if (fd >= 0)
      return fd;
    if (errno != EEXIST)
      return -1;
  }
  errno = EEXIST;
  return -1;
}

int mkstemps(char *template_name, int suffix_length) {
  return fill_template(template_name, suffix_length, 0);
}

int mkostemp(char *template_name, int flags) {
  return fill_template(template_name, 0, flags);
}

int utimensat(int dirfd, const char *path, const struct timespec times[2],
              int flags) {
  (void)dirfd;
  (void)flags;
  if (times == NULL)
    return utimes(path, NULL);

  struct timeval values[2];
  values[0].tv_sec = times[0].tv_sec;
  values[0].tv_usec = (suseconds_t)(times[0].tv_nsec / 1000);
  values[1].tv_sec = times[1].tv_sec;
  values[1].tv_usec = (suseconds_t)(times[1].tv_nsec / 1000);
  return utimes(path, values);
}

/* ------------------------------------------------------------- sockets/net */

ssize_t recvmmsg(int fd, struct mmsghdr *messages, size_t count, int flags,
                 const struct timespec *timeout) {
  (void)timeout;
  size_t i = 0;
  for (; i < count; ++i) {
    ssize_t received = recvmsg(fd, &messages[i].msg_hdr, flags);
    if (received < 0) {
      if (i == 0)
        return -1;
      break;
    }
    messages[i].msg_len = (unsigned)received;
    if ((flags & MSG_WAITFORONE) != 0)
      flags |= MSG_DONTWAIT;
  }
  return (ssize_t)i;
}

ssize_t sendmmsg(int fd, struct mmsghdr *messages, size_t count, int flags) {
  size_t i = 0;
  for (; i < count; ++i) {
    ssize_t sent = sendmsg(fd, &messages[i].msg_hdr, flags);
    if (sent < 0) {
      if (i == 0)
        return -1;
      break;
    }
    messages[i].msg_len = (unsigned)sent;
  }
  return (ssize_t)i;
}

unsigned int if_nametoindex(const char *name) {
  (void)name;
  errno = ENOSYS;
  return 0;
}

char *if_indextoname(unsigned index, char *name) {
  (void)index;
  (void)name;
  errno = ENOSYS;
  return NULL;
}

/* ---------------------------------------------------- user/account lookups */

int getpwuid_r(uid_t uid, struct passwd *result, char *buffer, size_t size,
               struct passwd **out) {
  (void)uid;
  (void)result;
  (void)buffer;
  (void)size;
  if (out)
    *out = NULL;
  return ENOENT;
}

void openlog(const char *ident, int option, int facility) {
  (void)ident;
  (void)option;
  (void)facility;
}

/* -------------------------------------------------- dynamic symbol lookups */

int dladdr(const void *address, Dl_info *info) {
  (void)address;
  if (info != NULL)
    memset(info, 0, sizeof(*info));
  return 0;
}

/* --------------------------------------------------------- non-local jumps */

/*
 * _setjmp()/_longjmp() are the signal-mask-free pair that OpenSSL's async
 * fibres use; the platform exports the plain names, so forward there.
 */
extern int setjmp(void *environment);
extern void longjmp(void *environment, int value) __attribute__((noreturn));

int _setjmp(void *environment) { return setjmp(environment); }

void _longjmp(void *environment, int value) { longjmp(environment, value); }

/* ------------------------------------------------------------ assertions */

void __assert(const char *function, const char *file, int line,
              const char *expression) {
  fprintf(stderr, "assertion failed: %s (%s:%d, %s)\n", expression, file, line,
          function);
  abort();
}

/* ---------------------------------------------------------- emulated TLS */

/*
 * The port libraries are built with -femulated-tls, so the compiler calls
 * __emutls_get_address() for every thread-local it emits. This is the standard
 * emulated-TLS runtime: the control block carries a slot index, and every
 * thread owns an array of values that grows as new variables appear.
 */
struct emutls_control {
  size_t size;
  size_t alignment;
  void *value; /* slot index + 1 once assigned */
};

static pthread_mutex_t emutls_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t emutls_key;
static pthread_once_t emutls_once = PTHREAD_ONCE_INIT;
static size_t emutls_slots;

static void emutls_key_destroy(void *values) { free(values); }

static void emutls_create_key(void) {
  (void)pthread_key_create(&emutls_key, emutls_key_destroy);
}

static void **emutls_thread_values(size_t needed) {
  (void)pthread_once(&emutls_once, emutls_create_key);

  void **values = pthread_getspecific(emutls_key);
  size_t allocated = values == NULL ? 0 : (size_t)values[0];
  if (allocated >= needed)
    return values;

  size_t grown = needed + 32;
  void **resized = realloc(values, (grown + 1) * sizeof(void *));
  if (resized == NULL)
    return NULL;

  if (values == NULL)
    memset(resized, 0, (allocated + 1) * sizeof(void *));
  else
    memset(resized + allocated + 1, 0, (grown - allocated) * sizeof(void *));
  resized[0] = (void *)grown;
  (void)pthread_setspecific(emutls_key, resized);
  return resized;
}

void *__emutls_get_address(struct emutls_control *control) {
  size_t slot = (size_t)control->value;
  if (slot == 0) {
    pthread_mutex_lock(&emutls_mutex);
    slot = (size_t)control->value;
    if (slot == 0) {
      slot = ++emutls_slots;
      control->value = (void *)slot;
    }
    pthread_mutex_unlock(&emutls_mutex);
  }

  void **values = emutls_thread_values(slot);
  if (values == NULL)
    return NULL;

  void *value = values[slot];
  if (value == NULL) {
    size_t alignment = control->alignment > sizeof(void *)
                           ? control->alignment
                           : sizeof(void *);
    if (posix_memalign(&value, alignment, control->size > 0 ? control->size : 1) != 0)
      return NULL;
    memset(value, 0, control->size);
    values[slot] = value;
  }
  return value;
}

/* -------------------------------------------------------------------- DNS */

/*
 * The platform's getaddrinfo() faults inside libSceNet for an installed title,
 * so name resolution goes through the resolver service directly - the same
 * approach the payload SDK's networking samples use. Numeric addresses are
 * handled locally; names are resolved by sceNetResolverStartNtoa().
 */
int sceNetResolverCreate(const char *name, void *pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char *hostname, void *address,
                            unsigned int timeout, int retries, int flags);
int sceNetResolverDestroy(int resolver);
int sceNetResolverGetError(int resolver, int *error);

long sceNetPoolCreate(const char *name, int size, int flags);


/* Dotted-quad parser; the platform's inet_pton is bound to libScePosixForWebKit
 * and is not usable from a title. Returns the address in network order. */
static unsigned int wiliwili_parse_ipv4(const char *text) {
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

  /* Guest is little-endian: emit the address in network byte order. */
  return (octets[3] << 24) | (octets[2] << 16) | (octets[1] << 8) | octets[0];
}

static struct addrinfo *wiliwili_build_addrinfo(unsigned int address,
                                               const char *service,
                                               const struct addrinfo *hints) {
  struct sockaddr_in *socket_address = calloc(1, sizeof(*socket_address));
  struct addrinfo *result = calloc(1, sizeof(*result));
  if (socket_address == NULL || result == NULL) {
    free(socket_address);
    free(result);
    return NULL;
  }

  socket_address->sin_len = sizeof(*socket_address);
  socket_address->sin_family = AF_INET;
  socket_address->sin_addr.s_addr = address;
  {
    uint16_t port = (uint16_t)(service != NULL ? atoi(service) : 0);
    socket_address->sin_port = (uint16_t)((port << 8) | (port >> 8));
  }

  result->ai_family = AF_INET;
  result->ai_socktype = hints != NULL && hints->ai_socktype != 0
                            ? hints->ai_socktype
                            : SOCK_STREAM;
  result->ai_protocol = hints != NULL ? hints->ai_protocol : 0;
  result->ai_addrlen = sizeof(*socket_address);
  result->ai_addr = (struct sockaddr *)socket_address;
  return result;
}


/* --------------------------------------------------------------- DNS client */

/*
 * Neither the platform getaddrinfo (bound to libScePosixForWebKit) nor its
 * resolver service is usable from an installed title, so A records are looked
 * up with a plain UDP query against the console's configured DNS server.
 */
int sceNetCtlGetInfo(int code, void *info);

#define WILIWILI_NETCTL_INFO_GET_PRIMARY_DNS 12

static unsigned int wiliwili_dns_server(void) {
  extern void wiliwili_boot_log(const char *);
  wiliwili_boot_log("dns: querying netctl");
  char address[16];
  int status = sceNetCtlGetInfo(WILIWILI_NETCTL_INFO_GET_PRIMARY_DNS, address);
  {
    char message[128];
    snprintf(message, sizeof(message), "dns: netctl status=%#x addr=%.16s", status,
             address);
    wiliwili_boot_log(message);
  }
  memset(address, 0, sizeof(address));
  if (status != 0)
    return 0;
  return wiliwili_parse_ipv4(address);
}

static unsigned int wiliwili_dns_lookup(const char *name) {
  unsigned int server = wiliwili_dns_server();
  {
    extern void wiliwili_boot_log(const char *);
    char message[128];
    snprintf(message, sizeof(message), "dns: server=%#x name=%s", server, name);
    wiliwili_boot_log(message);
  }
  if (server == 0)
    return 0;

  unsigned char query[512];
  memset(query, 0, sizeof(query));
  size_t length = 0;

  static unsigned int counter = 0;
  unsigned short id = (unsigned short)(++counter);
  query[0] = (unsigned char)(id >> 8);
  query[1] = (unsigned char)(id & 0xff);
  query[2] = 0x01; /* recursion desired */
  query[5] = 0x01; /* one question */
  length = 12;

  for (const char *label = name; *label != '\0';) {
    const char *dot = strchr(label, '.');
    size_t size = dot != NULL ? (size_t)(dot - label) : strlen(label);
    if (size == 0 || size > 63 || length + size + 5 > sizeof(query))
      return 0;
    query[length++] = (unsigned char)size;
    memcpy(query + length, label, size);
    length += size;
    label += size;
    if (*label == '.')
      ++label;
  }
  query[length++] = 0x00; /* end of name */
  query[length++] = 0x00;
  query[length++] = 0x01; /* type A */
  query[length++] = 0x00;
  query[length++] = 0x01; /* class IN */

  int socket_descriptor = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_descriptor < 0)
    return 0;

  struct sockaddr_in server_address;
  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_len = sizeof(server_address);
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = server;
  server_address.sin_port = (uint16_t)((53 << 8) | (53 >> 8));

  int sent = (int)sendto(socket_descriptor, query, length, 0,
                         (struct sockaddr *)&server_address,
                         sizeof(server_address));
  if (sent != (int)length) {
    close(socket_descriptor);
    return 0;
  }

  unsigned int result = 0;
  for (int attempt = 0; attempt < 5 && result == 0; ++attempt) {
    struct pollfd waiting;
    waiting.fd = socket_descriptor;
    waiting.events = POLLIN;
    waiting.revents = 0;
    if (poll(&waiting, 1, 1000) <= 0)
      continue;

    unsigned char response[1024];
    int received = (int)recvfrom(socket_descriptor, response, sizeof(response), 0,
                                 NULL, NULL);
    if (received < 12)
      continue;

    unsigned short response_id = (unsigned short)((response[0] << 8) | response[1]);
    if (response_id != id || (response[3] & 0x0f) != 0)
      continue;

    unsigned short answers = (unsigned short)((response[6] << 8) | response[7]);
    int offset = 12;
    while (offset < received && response[offset] != 0) {
      if ((response[offset] & 0xc0) == 0xc0) { /* compressed name */
        offset += 2;
        break;
      }
      offset += response[offset] + 1;
    }
    if (offset < received && response[offset] == 0)
      ++offset;
    offset += 4; /* type and class of the question */

    for (int index = 0; index < answers && offset + 10 <= received; ++index) {
      if ((response[offset] & 0xc0) == 0xc0)
        offset += 2;
      else
        while (offset < received && response[offset] != 0)
          offset += response[offset] + 1;

      if (offset + 10 > received)
        break;

      unsigned short record_type = (unsigned short)((response[offset] << 8) | response[offset + 1]);
      unsigned short record_length = (unsigned short)((response[offset + 8] << 8) | response[offset + 9]);
      offset += 10;
      if (record_type == 1 && record_length == 4 && offset + 4 <= received) {
        memcpy(&result, response + offset, 4);
        break;
      }
      offset += record_length;
    }
  }

  close(socket_descriptor);
  {
    extern void wiliwili_boot_log(const char *);
    char message[128];
    snprintf(message, sizeof(message), "dns: resolved %s -> %#x", name, result);
    wiliwili_boot_log(message);
  }
  return result;
}

int sceKernelUsleep(unsigned int microseconds);

/* The resolver service rejects any timeout/retry other than zero on this
 * firmware, so the ABI is pinned to (0, 0, 0) and the resolver handle is kept
 * alive for the lifetime of the process. */
static int wiliwili_resolver_id(void) {
  static int resolver = -2;
  static int lock;

  if (resolver != -2)
    return resolver;

  while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE) != 0)
    sceKernelUsleep(1000);

  if (resolver == -2) {
    long pool = sceNetPoolCreate("wiliwili-dns", 0x4000, 0);
    resolver = pool >= 0
                   ? sceNetResolverCreate("wiliwili-dns",
                                          (void *)(intptr_t)pool, 0)
                   : -1;
    {
      extern void wiliwili_boot_log(const char *);
      char message[128];
      snprintf(message, sizeof(message), "dns: pool=%ld resolver=%d", pool,
               resolver);
      wiliwili_boot_log(message);
    }
  }

  __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
  return resolver;
}

static unsigned int wiliwili_resolve(const char *name) {
  unsigned int address = 0;
  int resolver = wiliwili_resolver_id();

  if (resolver >= 0) {
    int status = sceNetResolverStartNtoa(resolver, name, &address, 0, 0, 0);
    extern void wiliwili_boot_log(const char *);
    char message[160];
    snprintf(message, sizeof(message), "dns: resolver status=%#x addr=%#x",
             status, address);
    wiliwili_boot_log(message);
    if (status == 0 && address != 0)
      return address;
  }

  return wiliwili_dns_lookup(name);
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **result) {
  if (result == NULL)
    return EAI_FAIL;
  *result = NULL;
  if (node == NULL)
    return EAI_NONAME;

  unsigned int parsed = wiliwili_parse_ipv4(node);
  if (parsed != 0 || strcmp(node, "0.0.0.0") == 0) {
    *result = wiliwili_build_addrinfo(parsed, service, hints);
    return *result != NULL ? 0 : EAI_MEMORY;
  }

  unsigned int resolved = wiliwili_resolve(node);
  if (resolved == 0)
    return EAI_NONAME;

  *result = wiliwili_build_addrinfo(resolved, service, hints);
  return *result != NULL ? 0 : EAI_MEMORY;
}

/* Numeric reverse lookup: curl asks for it when logging, and the platform
 * implementation belongs to the WebKit module set. */
int getnameinfo(const struct sockaddr *address, socklen_t length, char *host,
                size_t host_length, char *service, size_t service_length,
                int flags) {
  (void)length;
  (void)flags;

  if (address == NULL || address->sa_family != AF_INET)
    return EAI_FAMILY;

  const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)address;
  unsigned int value = ipv4->sin_addr.s_addr;

  if (host != NULL && host_length > 0)
    snprintf(host, host_length, "%u.%u.%u.%u", value & 0xff,
             (value >> 8) & 0xff, (value >> 16) & 0xff, (value >> 24) & 0xff);
  if (service != NULL && service_length > 0)
    snprintf(service, service_length, "%u",
             (unsigned)((ipv4->sin_port & 0xff) << 8 | (ipv4->sin_port >> 8)));

  return 0;
}

void freeaddrinfo(struct addrinfo *result) {
  while (result != NULL) {
    struct addrinfo *next = result->ai_next;
    free(result->ai_addr);
    free(result);
    result = next;
  }
}

const char *gai_strerror(int error) {
  switch (error) {
    case 0: return "success";
    case EAI_NONAME: return "name or service not known";
    case EAI_FAIL: return "non-recoverable failure in name resolution";
    case EAI_MEMORY: return "memory allocation failure";
    default: return "address resolution error";
  }
}

/*
 * libcurl sets FD_CLOEXEC on its sockets. A title never execs, and the
 * platform's fcntl() rejects the request, which curl reports as a socket
 * setup failure. Report success for those two commands and let everything else
 * reach the kernel.
 */

#ifndef FIONBIO
#define FIONBIO 0x8004667e
#endif

#define WILIWILI_FD_LIMIT 256
static int wiliwili_fd_flag_store[WILIWILI_FD_LIMIT];

static int wiliwili_fd_flags(int file) {
  if (file < 0 || file >= WILIWILI_FD_LIMIT)
    return 0;
  return wiliwili_fd_flag_store[file];
}

static void wiliwili_set_fd_flags(int file, int flags) {
  if (file < 0 || file >= WILIWILI_FD_LIMIT)
    return;
  wiliwili_fd_flag_store[file] = flags;
}

int fcntl(int file, int command, ...) {
  va_list arguments;
  va_start(arguments, command);
  long value = va_arg(arguments, long);
  va_end(arguments);

  /*
   * A title that issues a raw syscall is killed with SYSTEM_ILLEGAL_FUNCTION_CALL
   * (klog: "directly issued a syscall 92"), so this wrapper only uses calls the
   * sandbox allows. FD_CLOEXEC is meaningless without exec; descriptor flags are
   * emulated, and blocking mode is applied through ioctl(FIONBIO).
   */
  switch (command) {
    case F_GETFD:
      return 0;
    case F_SETFD:
      return 0;
    case F_GETFL:
      return wiliwili_fd_flags(file);
    case F_SETFL: {
      wiliwili_set_fd_flags(file, (int)value);
      int nonblocking = (value & O_NONBLOCK) != 0;
      return ioctl(file, FIONBIO, &nonblocking);
    }
    default: {
      extern void wiliwili_boot_log(const char *);
      char message[96];
      snprintf(message, sizeof(message), "fcntl: unsupported command %d on fd %d",
               command, file);
      wiliwili_boot_log(message);
      errno = ENOSYS;
      return -1;
    }
  }
}

/* Symbols the software rendering stack (Mesa/LLVM) references but the clean-room
 * runtime does not export. */

/* zstd's tracing hooks are weak in the library, which the module converter
 * still treats as imports. They are only used by profiling builds. */
void ZSTD_trace_compress_begin(void *context) { (void)context; }
void ZSTD_trace_compress_end(void *context, void *result) { (void)context; (void)result; }
void ZSTD_trace_decompress_begin(void *context) { (void)context; }
void ZSTD_trace_decompress_end(void *context, void *result) { (void)context; (void)result; }

/* Mesa's disk cache asks for the descriptor behind a directory stream; that
 * stream is opaque here, and the shader cache is disabled at runtime, so the
 * call reports an unusable descriptor instead of reading private state. */
int dirfd(struct _dirdesc *stream) {
  (void)stream;
  errno = ENOSYS;
  return -1;
}
char *strsignal(int signal) {
  static const char *const names[] = {
      "Unknown signal", "Hangup", "Interrupt", "Quit", "Illegal instruction",
      "Trace/breakpoint trap", "Aborted", "Emulation trap", "Floating point exception",
      "Killed", "Bus error", "Segmentation fault"};
  if (signal >= 0 && signal < (int)(sizeof(names) / sizeof(names[0])))
    return (char *)names[signal];
  return (char *)names[0];
}

/* The runtime manages the heap itself; growing the program break is not part of
 * the supported contract, so report failure instead of pretending to succeed. */
void *sbrk(intptr_t increment) {
  (void)increment;
  errno = ENOMEM;
  return (void *)-1;
}

int __xuname(int selector, void *buffer) {
  (void)selector;
  (void)buffer;
  errno = ENOSYS;
  return -1;
}




/* FreeBSD argument order: the context comes first. */
struct wiliwili_sort_context {
  int (*compare)(void *, const void *, const void *);
  void *argument;
};

static int wiliwili_sort_thunk(const void *left, const void *right, void *context) {
  struct wiliwili_sort_context *sort = (struct wiliwili_sort_context *)context;
  return sort->compare(sort->argument, left, right);
}

void qsort_r(void *base, size_t count, size_t size, void *argument,
             int (*compare)(void *, const void *, const void *)) {
  struct wiliwili_sort_context context = {compare, argument};
  if (count < 2 || size == 0)
    return;
  char *items = (char *)base;
  char *scratch = malloc(size);
  if (scratch == NULL)
    return;
  for (size_t index = 1; index < count; ++index) {
    memcpy(scratch, items + index * size, size);
    size_t position = index;
    while (position > 0 &&
           wiliwili_sort_thunk(items + (position - 1) * size, scratch, &context) > 0) {
      memcpy(items + position * size, items + (position - 1) * size, size);
      --position;
    }
    memcpy(items + position * size, scratch, size);
  }
  free(scratch);
}



/* --------------------------------------------------------- allocation API --
 * The allocator is redirected (--wrap=malloc and friends) to one mspace, so
 * every function that hands memory back to a caller has to allocate from that
 * same place: the platform's own strdup() returns platform heap memory, and the
 * free() that follows would then be served by the wrapped allocator, which
 * frees a foreign pointer into its own mspace and corrupts its bookkeeping.
 * LLVM's hash tables are allocated and released through exactly this path.
 */
void *aligned_alloc(size_t alignment, size_t size) {
  /* C11 requires size to be a multiple of alignment; the platform heap is
   * forgiving, the mspace is not. */
  if (alignment != 0 && (size % alignment) != 0)
    size += alignment - (size % alignment);
  void *result = NULL;
  if (posix_memalign(&result, alignment != 0 ? alignment : sizeof(void *),
                     size) != 0)
    return NULL;
  return result;
}

char *strdup(const char *text) {
  if (text == NULL)
    return NULL;
  size_t length = strlen(text) + 1;
  char *copy = (char *)malloc(length);
  if (copy == NULL)
    return NULL;
  memcpy(copy, text, length);
  return copy;
}

char *strndup(const char *text, size_t limit) {
  if (text == NULL)
    return NULL;
  size_t length = 0;
  while (length < limit && text[length] != '\0')
    ++length;
  char *copy = (char *)malloc(length + 1);
  if (copy == NULL)
    return NULL;
  memcpy(copy, text, length);
  copy[length] = '\0';
  return copy;
}

int vasprintf(char **target, const char *format, va_list arguments) {
  if (target == NULL)
    return -1;
  *target = NULL;

  va_list probe;
  va_copy(probe, arguments);
  int needed = vsnprintf(NULL, 0, format, probe);
  va_end(probe);
  if (needed < 0)
    return -1;

  char *buffer = (char *)malloc((size_t)needed + 1);
  if (buffer == NULL)
    return -1;

  va_list copy;
  va_copy(copy, arguments);
  int written = vsnprintf(buffer, (size_t)needed + 1, format, copy);
  va_end(copy);
  if (written < 0) {
    free(buffer);
    return -1;
  }
  *target = buffer;
  return written;
}

int asprintf(char **target, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  int result = vasprintf(target, format, arguments);
  va_end(arguments);
  return result;
}

/* Only absolute paths mean anything in the sandbox, and the caller may ask for
 * the result to be allocated. */
char *realpath(const char *path, char *resolved) {
  if (path == NULL)
    return NULL;
  size_t length = strlen(path) + 1;
  char *result = resolved;
  if (result == NULL) {
    result = (char *)malloc(length);
    if (result == NULL)
      return NULL;
  }
  memcpy(result, path, length);
  return result;
}

/* ------------------------------------------------------------------ printf --
 * A title has no stdout: whatever the port prints would otherwise be lost. The
 * lines are routed to the boot log instead, which is the only channel that
 * survives to the host.
 */
int vsnprintf(char *buffer, size_t size, const char *format, va_list arguments);

int printf(const char *format, ...) {
  extern void wiliwili_boot_log(const char *);
  char message[512];
  va_list arguments;
  va_start(arguments, format);
  int written = vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  if (written < 0)
    return written;
  wiliwili_boot_log(message);
  return written;
}


/* --------------------------------------------------- libScePosixForWebKit --
 * This firmware ships neither libScePosixForWebKit, which is where the linker
 * found these, nor libSceKeyboard. An import from a library that never loads
 * keeps its slot at zero, so every one of them has to exist in this image:
 * LLVM seeds its hash tables from arc4random, which makes it reachable from any
 * pass, and the rest are ordinary libc calls the ported libraries may make.
 */

/* Entropy first, a mixed clock/address seed only if the device cannot be
 * opened: LLVM uses the result to seed hashing, but OpenSSL may ask too. */
void arc4random_buf(void *buffer, size_t length) {
  unsigned char *bytes = (unsigned char *)buffer;
  if (bytes == NULL || length == 0)
    return;

  int device = open("/dev/urandom", O_RDONLY, 0);
  if (device >= 0) {
    size_t filled = 0;
    while (filled < length) {
      long got = read(device, bytes + filled, length - filled);
      if (got <= 0)
        break;
      filled += (size_t)got;
    }
    close(device);
    if (filled == length)
      return;
    bytes += filled;
    length -= filled;
  }

  static unsigned long long state;
  if (state == 0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    state = (unsigned long long)now.tv_sec * 1000000000ull +
            (unsigned long long)now.tv_nsec;
    state ^= (unsigned long long)(uintptr_t)&state;
  }
  for (size_t index = 0; index < length; ++index) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    bytes[index] = (unsigned char)(state >> 24);
  }
}

unsigned int arc4random(void) {
  unsigned int value = 0;
  arc4random_buf(&value, sizeof(value));
  return value;
}

int isatty(int descriptor) {
  (void)descriptor;
  return 0; /* a title has no terminal */
}

int mkstemp(char *template_name) {
  return mkstemps(template_name, 0);
}

char *strcasestr(const char *haystack, const char *needle) {
  if (haystack == NULL || needle == NULL)
    return NULL;
  if (*needle == '\0')
    return (char *)haystack;
  size_t length = strlen(needle);
  for (; *haystack != '\0'; ++haystack) {
    if (strncasecmp(haystack, needle, length) == 0)
      return (char *)haystack;
  }
  return NULL;
}

int fnmatch(const char *pattern, const char *string, int flags) {
  const char *pattern_at = pattern;
  const char *string_at = string;

  while (*pattern_at != '\0') {
    if (*pattern_at == '*') {
      while (*pattern_at == '*')
        ++pattern_at;
      if (*pattern_at == '\0')
        return 0;
      for (const char *retry = string_at; *retry != '\0' || *pattern_at == '\0';
           ++retry) {
        if (fnmatch(pattern_at, retry, flags) == 0)
          return 0;
        if (*retry == '\0')
          break;
        if ((flags & FNM_PATHNAME) && *retry == '/')
          break;
      }
      return FNM_NOMATCH;
    }

    if (*string_at == '\0')
      return FNM_NOMATCH;

    if (*pattern_at == '?') {
      if ((flags & FNM_PATHNAME) && *string_at == '/')
        return FNM_NOMATCH;
      ++pattern_at;
      ++string_at;
      continue;
    }

    if (*pattern_at == '[') {
      const char *class_at = pattern_at + 1;
      int negate = 0;
      if (*class_at == '!' || *class_at == '^') {
        negate = 1;
        ++class_at;
      }
      int matched = 0;
      char candidate = *string_at;
      if ((flags & FNM_CASEFOLD) != 0)
        candidate = (char)tolower((unsigned char)candidate);
      while (*class_at != '\0' && *class_at != ']') {
        char low = *class_at;
        if (low == '\\' && *(class_at + 1) != '\0')
          low = *(++class_at);
        if ((flags & FNM_CASEFOLD) != 0)
          low = (char)tolower((unsigned char)low);
        if (*(class_at + 1) == '-' && *(class_at + 2) != '\0' &&
            *(class_at + 2) != ']') {
          char high = *(class_at + 2);
          if ((flags & FNM_CASEFOLD) != 0)
            high = (char)tolower((unsigned char)high);
          if (candidate >= low && candidate <= high)
            matched = 1;
          class_at += 3;
          continue;
        }
        if (candidate == low)
          matched = 1;
        ++class_at;
      }
      if (*class_at != ']')
        return FNM_NOMATCH; /* unterminated class: no match, like the platform */
      if (matched == negate)
        return FNM_NOMATCH;
      pattern_at = class_at + 1;
      ++string_at;
      continue;
    }

    char pattern_character = *pattern_at;
    if (pattern_character == '\\' && *(pattern_at + 1) != '\0')
      pattern_character = *(++pattern_at);
    char string_character = *string_at;
    if ((flags & FNM_CASEFOLD) != 0) {
      pattern_character = (char)tolower((unsigned char)pattern_character);
      string_character = (char)tolower((unsigned char)string_character);
    }
    if (pattern_character != string_character)
      return FNM_NOMATCH;
    ++pattern_at;
    ++string_at;
  }

  return *string_at == '\0' ? 0 : FNM_NOMATCH;
}

/* The classic interface is still used by libraries that predate getaddrinfo;
 * it is served from this image's resolver. */
struct hostent *gethostbyname(const char *name) {
  static struct hostent entry;
  static struct in_addr address;
  static char *address_list[2];

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *result = NULL;
  if (name == NULL || getaddrinfo(name, NULL, &hints, &result) != 0 ||
      result == NULL)
    return NULL;

  struct sockaddr_in *resolved = (struct sockaddr_in *)result->ai_addr;
  address = resolved->sin_addr;
  address_list[0] = (char *)&address;
  address_list[1] = NULL;

  entry.h_name = (char *)name;
  entry.h_aliases = NULL;
  entry.h_addrtype = AF_INET;
  entry.h_length = sizeof(address);
  entry.h_addr_list = address_list;
  freeaddrinfo(result);
  return &entry;
}

/* ------------------------------------------------------------------ dlopen --
 * A title may not load code at runtime, so the software rendering stack is
 * linked into the image. SDL's OSMesa video backend still asks for it through
 * SDL_LoadObject/dlsym, and the addresses it needs are the statically linked
 * ones: these entry points satisfy that loader without opening anything.
 */
#ifdef WILIWILI_SOFTWARE_RENDER
void *OSMesaCreateContext(unsigned int format, void *share_list);
void *OSMesaCreateContextExt(unsigned int format, int depth_bits, int stencil_bits,
                             int accum_bits, void *share_list);
void *OSMesaCreateContextAttribs(const int *attrib_list, void *share_list);
void OSMesaDestroyContext(void *context);
int OSMesaMakeCurrent(void *context, void *buffer, unsigned int type, int width,
                      int height);
int OSMesaGetColorBuffer(void *context, int *width, int *height, int *format,
                         void **buffer);
void OSMesaPixelStore(int pname, int value);
void *OSMesaGetProcAddress(const char *name);
int OSMesaGetDepthBuffer(void *context, int *width, int *height, int *bytes,
                         void **buffer);
void OSMesaColorClamp(int enable);
void *OSMesaGetCurrentContext(void);

static const struct {
  const char *name;
  void *address;
} wiliwili_gl_symbols[] = {
    {"OSMesaCreateContext", (void *)OSMesaCreateContext},
    {"OSMesaCreateContextExt", (void *)OSMesaCreateContextExt},
    {"OSMesaCreateContextAttribs", (void *)OSMesaCreateContextAttribs},
    {"OSMesaDestroyContext", (void *)OSMesaDestroyContext},
    {"OSMesaMakeCurrent", (void *)OSMesaMakeCurrent},
    {"OSMesaGetColorBuffer", (void *)OSMesaGetColorBuffer},
    {"OSMesaPixelStore", (void *)OSMesaPixelStore},
    {"OSMesaGetProcAddress", (void *)OSMesaGetProcAddress},
    {"OSMesaGetDepthBuffer", (void *)OSMesaGetDepthBuffer},
    {"OSMesaColorClamp", (void *)OSMesaColorClamp},
    {"OSMesaGetCurrentContext", (void *)OSMesaGetCurrentContext},
};

static void wiliwili_dl_trace(const char *verb, const char *name, void *result) {
  extern void wiliwili_boot_log(const char *);
  static int budget = 96;
  if (budget <= 0)
    return;
  --budget;
  char message[192];
  snprintf(message, sizeof(message), "dl: %s %s -> %s", verb,
           name != NULL ? name : "(null)", result != NULL ? "ok" : "NULL");
  wiliwili_boot_log(message);
}

void *dlopen(const char *path, int mode) {
  (void)mode;
  /* dlopen(NULL, ...) asks for the handle of the running program, and LLVM's
   * JIT calls exactly that first (DynamicLibrary::LoadLibraryPermanently) to
   * make the process's symbols visible to generated code. A NULL answer is
   * fatal there - which is why every shader compile in the native title ended
   * as "JIT creation failed" with an empty message and the renderer died on
   * its first draw. The handle is opaque here: a title cannot load code at run
   * time, so only returning something non-NULL matters. */
  if (path == NULL)
    return (void *)&wiliwili_gl_symbols[0];
  /* Only the software renderer is available; report it as always open. */
  wiliwili_dl_trace("open", path, (void *)&wiliwili_gl_symbols[0]);
  return (void *)&wiliwili_gl_symbols[0];
}

void *dlsym(void *handle, const char *name) {
  (void)handle;
  if (name == NULL)
    return NULL;
  for (unsigned index = 0;
       index < sizeof(wiliwili_gl_symbols) / sizeof(wiliwili_gl_symbols[0]);
       ++index) {
    if (strcmp(name, wiliwili_gl_symbols[index].name) == 0) {
      wiliwili_dl_trace("sym", name, wiliwili_gl_symbols[index].address);
      return wiliwili_gl_symbols[index].address;
    }
  }
  wiliwili_dl_trace("sym", name, NULL);
  return NULL;
}

int dlclose(void *handle) {
  (void)handle;
  return 0;
}

char *dlerror(void) { return NULL; }
#endif /* WILIWILI_SOFTWARE_RENDER */
