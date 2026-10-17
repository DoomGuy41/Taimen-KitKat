/* libhyb hooks: functions imported by the Android 11 libraries that KitKat's bionic lacks (or
 * implements with different semantics). Everything else is taken as is from KitKat's
 * libc/libm/libdl/liblog/libcutils/libz (same 32-bit ABI). */
#define LOG_TAG "libhyb"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <android/log.h>
#include <cutils/native_handle.h>
#include <cutils/properties.h>
#include "hyb.h"
#include "hyb_internal.h"

#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

/* ------------------------------------------------------------------ libdl */
static int s_trace = -1;
static int tracing(void)
{
    if (s_trace < 0) s_trace = getenv("HYB_TRACE") != NULL;
    return s_trace;
}
static void *h_dlopen(const char *n, int f)
{
    void *h = hyb_dlopen(n, f);
    if (tracing()) __android_log_print(ANDROID_LOG_INFO, "libhyb", "dlopen(%s) = %p", n, h);
    return h;
}
static void *h_android_dlopen_ext(const char *n, int f, const void *ext) { return hyb_dlopen(n, f); }
static void *h_dlsym(void *h, const char *s) { return hyb_dlsym(h, s); }
static int h_dlclose(void *h) { return hyb_dlclose(h); }
static char *h_dlerror(void) { return (char *)hyb_dlerror(); }
static int h_target_sdk(void) { return 29; }
static int h_device_api_level(void) { return 30; }
static void *h_null(void) { return NULL; }
static int h_zero(void) { return 0; }
static void h_noop(void) {}

/* ------------------------------------------------------------------ paths */
/* libhardware checks that modules exist with access(): Android library paths
 * (/vendor/lib..., /odm/lib..., /system/lib...) mean the Android 11 root */
static int lib_path(const char *p)
{
    return !strncmp(p, "/vendor/lib", 11) || !strncmp(p, "/odm/lib", 8) || !strncmp(p, "/system/lib", 11);
}
static int h_access(const char *p, int mode)
{
    if (p && lib_path(p)) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s%s", hyb_root(), p);
        int r = access(buf, mode);
        if (tracing()) __android_log_print(ANDROID_LOG_INFO, "libhyb", "access(%s) = %d", buf, r);
        return r;
    }
    return access(p, mode);
}

/* libhardware checks with realpath() that the module really is under /vendor/lib/hw (etc.) */
static char *h_realpath(const char *p, char *out)
{
    if (p && lib_path(p)) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s%s", hyb_root(), p);
        if (access(buf, F_OK)) {
            errno = ENOENT;
            return NULL;
        }
        if (!out) out = malloc(PATH_MAX);
        snprintf(out, PATH_MAX, "%s", p);
        return out;
    }
    return realpath(p, out);
}

/* ------------------------------------------------------------------ stdio */
FILE *h_stdin, *h_stdout, *h_stderr;
static int h_dprintf(int fd, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    return n > 0 ? write(fd, buf, n) : n;
}
static int h_fgetc_unlocked(FILE *f) { return fgetc(f); }
static char *h_fgets_unlocked(char *s, int n, FILE *f) { return fgets(s, n, f); }
static int h_fputc_unlocked(int c, FILE *f) { return fputc(c, f); }
static size_t h_fread_unlocked(void *p, size_t s, size_t n, FILE *f) { return fread(p, s, n, f); }
static size_t h_fwrite_unlocked(const void *p, size_t s, size_t n, FILE *f) { return fwrite(p, s, n, f); }

/* ------------------------------------------------------------------ fortify */
static size_t h_fwrite_chk(const void *p, size_t s, size_t n, FILE *f, size_t bs) { return fwrite(p, s, n, f); }
static ssize_t h_read_chk(int fd, void *b, size_t c, size_t bs) { return read(fd, b, c); }
static int h_poll_chk(struct pollfd *f, nfds_t n, int t, size_t fl) { return poll(f, n, t); }
/* Android 11 binder services: KitKat has no hwservicemanager and no compatible servicemanager;
 * getService would wait forever (power client of the camera HAL). Report the service missing. */
static void h_hidl_no_service(void **sp_ret, const void *a, const void *b, int retry, int stub) { *sp_ret = NULL; }
static void *h_aservice_none(const char *name) { return NULL; }
extern int __cxa_atexit(void (*f)(void *), void *obj, void *dso);
static int h_aeabi_atexit(void *obj, void (*dtor)(void *), void *dso) { return __cxa_atexit(dtor, obj, dso); }
static size_t h_fread_chk(void *p, size_t bs, size_t s, size_t n, FILE *f) { return fread(p, s, n, f); }
/* KitKat defines the sigset functions inline (signal.h): 32-bit sigset_t */
static int h_sigemptyset(sigset_t *set) { memset(set, 0, sizeof(*set)); return 0; }
static int h_sigaddset(sigset_t *set, int sig)
{
    if (sig < 1 || sig > 32) { errno = EINVAL; return -1; }
    *(unsigned long *)set |= 1UL << (sig - 1);
    return 0;
}
static int h_rand_r(unsigned *seed)
{
    *seed = *seed * 1103515245 + 12345;
    return (int)((*seed >> 1) & 0x7fffffff);
}
static char *h_gnu_basename(const char *p)
{
    const char *b = strrchr(p, '/');
    return (char *)(b ? b + 1 : p);
}
static ssize_t h_process_vm_readv(pid_t pid, const struct iovec *l, unsigned long ln, const struct iovec *r,
                                  unsigned long rn, unsigned long fl)
{
    return syscall(376 /* __NR_process_vm_readv, ARM */, pid, l, ln, r, rn, fl);
}
static int h_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abs)
{
    for (;;) {
        int e = pthread_mutex_trylock(m);
        if (e != EBUSY) return e;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > abs->tv_sec || (now.tv_sec == abs->tv_sec && now.tv_nsec >= abs->tv_nsec))
            return ETIMEDOUT;
        usleep(1000);
    }
}
static ssize_t h_write_chk(int fd, const void *b, size_t c, size_t bs) { return write(fd, b, c); }
static ssize_t h_readlink_chk(const char *p, char *b, size_t s, size_t bs) { return readlink(p, b, s); }
static char *h_strncpy_chk2(char *d, const char *s, size_t n, size_t dl, size_t sl) { return strncpy(d, s, n); }

static void h_fd_set_chk(int fd, fd_set *set, size_t sz) { FD_SET(fd, set); }
static void h_fd_clr_chk(int fd, fd_set *set, size_t sz) { FD_CLR(fd, set); }
static int h_fd_isset_chk(int fd, const fd_set *set, size_t sz) { return FD_ISSET(fd, set); }

/* ------------------------------------------------------------------ misc libc */
static int h_rand(void) { return (int)(lrand48() & 0x7fffffff); }
static void h_srand(unsigned s) { srand48(s); }
static double h_atof(const char *s) { return strtod(s, NULL); }
static int h_getpagesize(void) { return 4096; }
static int h_epoll_create1(int flags) { return syscall(__NR_epoll_create1, flags); }
static void *h_mmap64(void *a, size_t l, int p, int f, int fd, int64_t off)
{
    if (off & 4095) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    return (void *)syscall(__NR_mmap2, a, l, p, f, fd, (long)(off >> 12));
}
static size_t h_mb_cur_max(void) { return 1; }
static char *h_gnu_strerror_r(int e, char *buf, size_t n)
{
    strerror_r(e, buf, n);
    return buf;
}
static struct cmsghdr *h_cmsg_nxthdr(struct msghdr *m, struct cmsghdr *c)
{
    struct cmsghdr *n = (struct cmsghdr *)((char *)c + CMSG_ALIGN(c->cmsg_len));
    if ((char *)(n + 1) > (char *)m->msg_control + m->msg_controllen ||
        (char *)n + CMSG_ALIGN(n->cmsg_len) > (char *)m->msg_control + m->msg_controllen)
        return NULL;
    return n;
}
static const char *h_getprogname(void)
{
    static char name[64];
    if (!name[0]) {
        int fd = open("/proc/self/cmdline", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, name, sizeof(name) - 1);
            close(fd);
            if (n > 0) name[n] = 0;
        }
        if (!name[0]) strcpy(name, "?");
    }
    const char *b = strrchr(name, '/');
    return b ? b + 1 : name;
}
static pid_t h_pthread_gettid_np(pthread_t t)
{
    static pid_t (*f)(pthread_t);
    if (!f) f = (pid_t (*)(pthread_t))dlsym(RTLD_DEFAULT, "__pthread_gettid");
    return f ? f(t) : gettid();
}
static int h_register_atfork(void (*pre)(void), void (*par)(void), void (*chi)(void), void *dso)
{
    return pthread_atfork(pre, par, chi);
}
static void h_set_abort_message(const char *m) { __android_log_print(ANDROID_LOG_FATAL, "libhyb", "abort: %s", m); }
static int h_fdsan_close_with_tag(int fd, uint64_t tag) { return close(fd); }
static uint64_t h_fdsan_create_owner_tag(int type, uint64_t tag) { return 0; }
static void h_fdsan_exchange(int fd, uint64_t a, uint64_t b) {}

/* thread_local destructors (libc++) */
struct dtor { void (*f)(void *); void *obj; struct dtor *next; };
static pthread_key_t s_dtor_key;
static pthread_once_t s_dtor_once = PTHREAD_ONCE_INIT;
static void run_dtors(void *p)
{
    for (struct dtor *d = p, *n; d; d = n) {
        n = d->next;
        d->f(d->obj);
        free(d);
    }
}
static void dtor_key_init(void) { pthread_key_create(&s_dtor_key, run_dtors); }
static int h_cxa_thread_atexit_impl(void (*f)(void *), void *obj, void *dso)
{
    pthread_once(&s_dtor_once, dtor_key_init);
    struct dtor *d = malloc(sizeof(*d));
    d->f = f;
    d->obj = obj;
    d->next = pthread_getspecific(s_dtor_key);
    pthread_setspecific(s_dtor_key, d);
    return 0;
}

/* ------------------------------------------------------------------ monotonic conditions */
/* KitKat's pthread_condattr_t is a long (bit 0: shared). The clock is stored in bit 8, then
 * monotonic condition variables are remembered to route pthread_cond_timedwait. */
#define COND_MONO 0x100
static pthread_mutex_t s_cond_lock = PTHREAD_MUTEX_INITIALIZER;
static void *s_mono_conds[1024];
static int cond_slot(void *c, int insert)
{
    unsigned h = ((uintptr_t)c >> 2) % 1024;
    for (int i = 0; i < 1024; i++, h = (h + 1) % 1024) {
        if (s_mono_conds[h] == c) return h;
        if (!s_mono_conds[h]) return insert ? (int)h : -1;
    }
    return -1;
}
static int h_condattr_setclock(pthread_condattr_t *a, clockid_t c)
{
    if (c == CLOCK_MONOTONIC) *a |= COND_MONO; else *a &= ~COND_MONO;
    return 0;
}
static int h_condattr_getclock(const pthread_condattr_t *a, clockid_t *c)
{
    *c = (*a & COND_MONO) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    return 0;
}
static int h_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    pthread_condattr_t clean = a ? (*a & ~COND_MONO) : 0;
    int r = pthread_cond_init(c, a ? &clean : NULL);
    pthread_mutex_lock(&s_cond_lock);
    int s = cond_slot(c, a && (*a & COND_MONO));
    if (a && (*a & COND_MONO)) { if (s >= 0) s_mono_conds[s] = c; }
    else if (s >= 0 && s_mono_conds[s] == c) s_mono_conds[s] = NULL;
    pthread_mutex_unlock(&s_cond_lock);
    return r;
}
static int is_mono(pthread_cond_t *c)
{
    pthread_mutex_lock(&s_cond_lock);
    int s = cond_slot(c, 0);
    int r = s >= 0 && s_mono_conds[s] == c;
    pthread_mutex_unlock(&s_cond_lock);
    return r;
}
static int h_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t)
{
    static int (*mono)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
    if (!mono)
        mono = dlsym(RTLD_DEFAULT, "pthread_cond_timedwait_monotonic_np");
    if (mono && is_mono(c))
        return mono(c, m, t);
    return pthread_cond_timedwait(c, m, t);
}
static int h_cond_destroy(pthread_cond_t *c)
{
    pthread_mutex_lock(&s_cond_lock);
    int s = cond_slot(c, 0);
    if (s >= 0 && s_mono_conds[s] == c) {
        /* removal, reinserting the following cluster (open addressing) */
        s_mono_conds[s] = NULL;
        for (int i = (s + 1) % 1024; s_mono_conds[i]; i = (i + 1) % 1024) {
            void *x = s_mono_conds[i];
            s_mono_conds[i] = NULL;
            int n = cond_slot(x, 1);
            if (n >= 0) s_mono_conds[n] = x;
        }
    }
    pthread_mutex_unlock(&s_cond_lock);
    return pthread_cond_destroy(c);
}
static int h_sem_timedwait_mono(sem_t *s, const struct timespec *abs_mono)
{
    struct timespec now_m, now_r, abs_r;
    clock_gettime(CLOCK_MONOTONIC, &now_m);
    clock_gettime(CLOCK_REALTIME, &now_r);
    long long d = (abs_mono->tv_sec - now_m.tv_sec) * 1000000000LL + (abs_mono->tv_nsec - now_m.tv_nsec);
    if (d < 0) d = 0;
    long long r = now_r.tv_sec * 1000000000LL + now_r.tv_nsec + d;
    abs_r.tv_sec = r / 1000000000LL;
    abs_r.tv_nsec = r % 1000000000LL;
    return sem_timedwait(s, &abs_r);
}

/* ------------------------------------------------------------------ minimal "C" locale */
struct a11_lconv {
    char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol,
         *mon_decimal_point, *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes,
         n_sep_by_space, p_sign_posn, n_sign_posn, int_p_cs_precedes, int_p_sep_by_space,
         int_n_cs_precedes, int_n_sep_by_space, int_p_sign_posn, int_n_sign_posn;
};
static struct a11_lconv s_lconv = { ".", "", "", "", "", "", "", "", "", "",
                                    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127 };
static void *h_newlocale(int mask, const char *name, void *base) { return (void *)&s_lconv; }
static void h_freelocale(void *l) {}
static void *h_uselocale(void *l) { return (void *)-1L; /* LC_GLOBAL_LOCALE */ }
static struct a11_lconv *h_localeconv(void) { return &s_lconv; }
static double h_strtod_l(const char *s, char **e, void *l) { return strtod(s, e); }
static float h_strtof(const char *s, char **e) { return (float)strtod(s, e); }
static float h_strtof_l(const char *s, char **e, void *l) { return (float)strtod(s, e); }
static long long h_strtoll_l(const char *s, char **e, int b, void *l) { return strtoll(s, e, b); }
static unsigned long long h_strtoull_l(const char *s, char **e, int b, void *l) { return strtoull(s, e, b); }
static int h_iswblank(wint_t c) { return c == ' ' || c == '\t'; }
static int h_mbtowc(wchar_t *w, const char *s, size_t n)
{
    if (!s) return 0;
    if (!n) return -1;
    if (w) *w = (unsigned char)*s;
    return *s ? 1 : 0;
}
static size_t h_mbsnrtowcs(wchar_t *d, const char **s, size_t nms, size_t len, void *ps)
{
    size_t i = 0;
    const char *p = *s;
    for (; i < nms && (!d || i < len); i++) {
        if (d) d[i] = (unsigned char)p[i];
        if (!p[i]) { *s = d ? NULL : *s; return i; }
    }
    if (d) *s = p + i;
    return i;
}
static size_t h_wcsnrtombs(char *d, const wchar_t **s, size_t nwc, size_t len, void *ps)
{
    size_t i = 0;
    const wchar_t *p = *s;
    for (; i < nwc && (!d || i < len); i++) {
        if (p[i] > 0xff) { errno = EILSEQ; return (size_t)-1; }
        if (d) d[i] = (char)p[i];
        if (!p[i]) { if (d) *s = NULL; return i; }
    }
    if (d) *s = p + i;
    return i;
}
static double wcs_to_d(const wchar_t *s, wchar_t **e)
{
    char buf[128];
    size_t i = 0;
    for (; s[i] && i < sizeof(buf) - 1; i++) buf[i] = s[i] < 128 ? (char)s[i] : '?';
    buf[i] = 0;
    char *ce;
    double v = strtod(buf, &ce);
    if (e) *e = (wchar_t *)s + (ce - buf);
    return v;
}
static float h_wcstof(const wchar_t *s, wchar_t **e) { return (float)wcs_to_d(s, e); }
static long long h_wcstoll(const wchar_t *s, wchar_t **e, int b)
{
    char buf[128];
    size_t i = 0;
    for (; s[i] && i < sizeof(buf) - 1; i++) buf[i] = s[i] < 128 ? (char)s[i] : '?';
    buf[i] = 0;
    char *ce;
    long long v = strtoll(buf, &ce, b);
    if (e) *e = (wchar_t *)s + (ce - buf);
    return v;
}
static unsigned long long h_wcstoull(const wchar_t *s, wchar_t **e, int b) { return (unsigned long long)h_wcstoll(s, e, b); }

/* ------------------------------------------------------------------ properties, libcutils */
static uint32_t h_prop_area_serial(void) { return 1; }
static void h_prop_read_callback(const prop_info *pi, void (*cb)(void *, const char *, const char *, uint32_t), void *cookie)
{
    char name[PROP_NAME_MAX], value[PROP_VALUE_MAX];
    __system_property_read(pi, name, value);
    cb(cookie, name, value, 0);
}
static int8_t h_property_get_bool(const char *key, int8_t def)
{
    char v[PROPERTY_VALUE_MAX];
    if (property_get(key, v, NULL) <= 0) return def;
    if (!strcmp(v, "1") || !strcmp(v, "y") || !strcmp(v, "yes") || !strcmp(v, "on") || !strcmp(v, "true")) return 1;
    if (!strcmp(v, "0") || !strcmp(v, "n") || !strcmp(v, "no") || !strcmp(v, "off") || !strcmp(v, "false")) return 0;
    return def;
}
/* property_get: a NULL key (some camera modules) crashes KitKat's property_get */
static int h_property_get(const char *key, char *value, const char *def)
{
    if ((uintptr_t)key < 0x10000) {
        static int warned;
        if (warned++ < 8)
            __android_log_print(ANDROID_LOG_WARN, "libhyb", "property_get(%p) from %p", key, __builtin_return_address(0));
        int n = def ? (int)strlen(def) : 0;
        if (n >= PROPERTY_VALUE_MAX) n = PROPERTY_VALUE_MAX - 1;
        if (n) memcpy(value, def, n);
        value[n] = 0;
        return n;
    }
    return property_get(key, value, def);
}
static int32_t h_property_get_int32(const char *key, int32_t def)
{
    char v[PROPERTY_VALUE_MAX], *e;
    if (property_get(key, v, NULL) <= 0) return def;
    long r = strtol(v, &e, 0);
    return *e ? def : (int32_t)r;
}
static int64_t h_property_get_int64(const char *key, int64_t def)
{
    char v[PROPERTY_VALUE_MAX], *e;
    if (property_get(key, v, NULL) <= 0) return def;
    long long r = strtoll(v, &e, 0);
    return *e ? def : (int64_t)r;
}
/* native_handle_init: handle header in storage provided by the caller */
static native_handle_t *h_native_handle_init(char *storage, int numFds, int numInts)
{
    native_handle_t *h = (native_handle_t *)storage;
    h->version = sizeof(native_handle_t);
    h->numFds = numFds;
    h->numInts = numInts;
    return h;
}
/* ashmem_valid: true if fd is an ashmem region (ASHMEM_GET_SIZE answers) */
static int h_ashmem_valid(int fd) { return ioctl(fd, _IO(0x77, 4), NULL) >= 0; }
static native_handle_t *h_native_handle_clone(const native_handle_t *h)
{
    native_handle_t *c = native_handle_create(h->numFds, h->numInts);
    if (!c) return NULL;
    for (int i = 0; i < h->numFds; i++) c->data[i] = dup(h->data[i]);
    memcpy(&c->data[h->numFds], &h->data[h->numFds], h->numInts * sizeof(int));
    return c;
}
static uint64_t h_atrace_tags(void) { return 0; }

/* ------------------------------------------------------------------ recent liblog */
struct a11_log_message {
    size_t struct_size;
    int32_t buffer_id, priority;
    const char *tag, *file;
    uint32_t line;
    const char *message;
};
static int s_min_prio = ANDROID_LOG_INFO;
static int h_log_is_loggable(int prio, const char *tag, int def) { return prio >= s_min_prio; }
static void h_log_write_message(const struct a11_log_message *m)
{
    static int (*buf_write)(int, int, const char *, const char *);
    if (!buf_write)
        buf_write = dlsym(RTLD_DEFAULT, "__android_log_buf_write");
    if (!buf_write) {
        __android_log_write(m->priority, m->tag ? m->tag : "", m->message);
        return;
    }
    buf_write(m->buffer_id < 0 ? 0 : m->buffer_id, m->priority, m->tag ? m->tag : "", m->message);
}
static int h_log_get_min(void) { return s_min_prio; }
static int h_log_set_min(int p) { int o = s_min_prio; s_min_prio = p; return o; }
static void h_log_aborter(const char *m) { __android_log_print(ANDROID_LOG_FATAL, "libhyb", "%s", m); abort(); }
static void h_log_logd_logger(const struct a11_log_message *m) { h_log_write_message(m); }
static void h_log_stderr_logger(const struct a11_log_message *m) { fprintf(stderr, "%s: %s\n", m->tag ? m->tag : "", m->message); }

/* ------------------------------------------------------------------ hook table */
struct hook { const char *name; void *addr; };
static struct hook s_hooks[] = {
    /* libdl / linker */
    { "dlopen", h_dlopen }, { "dlsym", h_dlsym }, { "dlclose", h_dlclose }, { "dlerror", h_dlerror },
    { "dladdr", hyb_dladdr }, { "dl_iterate_phdr", hyb_dl_iterate_phdr },
    { "dl_unwind_find_exidx", hyb_unwind_find_exidx }, { "android_dlopen_ext", h_android_dlopen_ext },
    { "android_get_application_target_sdk_version", h_target_sdk },
    { "android_get_device_api_level", h_device_api_level },
    { "android_get_exported_namespace", h_null }, { "android_create_namespace", h_null },
    { "android_link_namespaces", h_zero }, { "android_init_anonymous_namespace", h_zero },
    { "android_set_application_target_sdk_version", h_noop },
    { "access", h_access }, { "realpath", h_realpath },
    /* stdio and fortify */
    { "stdin", &h_stdin }, { "stdout", &h_stdout }, { "stderr", &h_stderr },
    { "dprintf", h_dprintf }, { "fgets_unlocked", h_fgets_unlocked }, { "fgetc_unlocked", h_fgetc_unlocked }, { "fputc_unlocked", h_fputc_unlocked },
    { "fread_unlocked", h_fread_unlocked }, { "fwrite_unlocked", h_fwrite_unlocked },
    { "__fwrite_chk", h_fwrite_chk }, { "__read_chk", h_read_chk }, { "__poll_chk", h_poll_chk },
    { "__fread_chk", h_fread_chk },
    { "_ZN7android8hardware7details21getRawServiceInternalERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_bb", h_hidl_no_service },
    { "AServiceManager_getService", h_aservice_none }, { "AServiceManager_checkService", h_aservice_none }, { "__aeabi_atexit", h_aeabi_atexit }, { "sigemptyset", h_sigemptyset }, { "sigaddset", h_sigaddset },
    { "rand_r", h_rand_r }, { "__gnu_basename", h_gnu_basename }, { "process_vm_readv", h_process_vm_readv },
    { "pthread_mutex_timedlock", h_mutex_timedlock }, { "__write_chk", h_write_chk },
    { "__readlink_chk", h_readlink_chk }, { "__FD_SET_chk", h_fd_set_chk },
    { "__FD_CLR_chk", h_fd_clr_chk }, { "__FD_ISSET_chk", h_fd_isset_chk }, { "__strncpy_chk2", h_strncpy_chk2 },
    /* libc */
    { "rand", h_rand }, { "srand", h_srand }, { "atof", h_atof }, { "getpagesize", h_getpagesize },
    { "epoll_create1", h_epoll_create1 }, { "mmap64", h_mmap64 },
    { "__ctype_get_mb_cur_max", h_mb_cur_max }, { "__gnu_strerror_r", h_gnu_strerror_r },
    { "__cmsg_nxthdr", h_cmsg_nxthdr }, { "getprogname", h_getprogname },
    { "pthread_gettid_np", h_pthread_gettid_np }, { "__register_atfork", h_register_atfork },
    { "__cxa_thread_atexit_impl", h_cxa_thread_atexit_impl },
    { "android_set_abort_message", h_set_abort_message },
    { "android_fdsan_close_with_tag", h_fdsan_close_with_tag },
    { "android_fdsan_create_owner_tag", h_fdsan_create_owner_tag },
    { "android_fdsan_exchange_owner_tag", h_fdsan_exchange },
    { "pthread_condattr_setclock", h_condattr_setclock }, { "pthread_condattr_getclock", h_condattr_getclock },
    { "pthread_cond_init", h_cond_init }, { "pthread_cond_timedwait", h_cond_timedwait },
    { "pthread_cond_destroy", h_cond_destroy }, { "sem_timedwait_monotonic_np", h_sem_timedwait_mono },
    /* locale */
    { "newlocale", h_newlocale }, { "freelocale", h_freelocale }, { "uselocale", h_uselocale },
    { "localeconv", h_localeconv }, { "strtod_l", h_strtod_l }, { "strtold_l", h_strtod_l },
    { "strtof", h_strtof }, { "strtof_l", h_strtof_l }, { "strtoll_l", h_strtoll_l },
    { "strtoull_l", h_strtoull_l }, { "iswblank", h_iswblank }, { "mbtowc", h_mbtowc },
    { "mbsnrtowcs", h_mbsnrtowcs }, { "wcsnrtombs", h_wcsnrtombs }, { "wcstof", h_wcstof },
    { "wcstoll", h_wcstoll }, { "wcstoull", h_wcstoull },
    /* properties and libcutils */
    { "__system_property_area_serial", h_prop_area_serial },
    { "__system_property_read_callback", h_prop_read_callback },
    { "property_get_bool", h_property_get_bool }, { "native_handle_clone", h_native_handle_clone },
    { "property_get", h_property_get }, { "property_get_int32", h_property_get_int32 }, { "property_get_int64", h_property_get_int64 },
    { "native_handle_init", h_native_handle_init }, { "ashmem_valid", h_ashmem_valid },
    { "atrace_get_enabled_tags", h_atrace_tags }, { "atrace_begin_body", h_noop },
    { "atrace_end_body", h_noop }, { "atrace_async_begin_body", h_noop },
    { "atrace_async_end_body", h_noop }, { "atrace_int_body", h_noop },
    /* liblog */
    { "__android_log_is_loggable", h_log_is_loggable },
    { "__android_log_write_log_message", h_log_write_message },
    { "__android_log_get_minimum_priority", h_log_get_min },
    { "__android_log_set_minimum_priority", h_log_set_min },
    { "__android_log_set_logger", h_noop }, { "__android_log_set_aborter", h_noop },
    { "__android_log_set_default_tag", h_noop }, { "__android_log_call_aborter", h_log_aborter },
    { "__android_log_default_aborter", h_log_aborter }, { "__android_log_logd_logger", h_log_logd_logger },
    { "__android_log_stderr_logger", h_log_stderr_logger }, { "__android_log_error_write", h_zero },
    /* libutilscallstack (debug call stacks) */
    { "_ZN7android9CallStack18getCurrentInternalEi", h_noop },
    { "_ZN7android9CallStack16logStackInternalEPKcPKS0_19android_LogPriority", h_noop },
    { "_ZN7android9CallStack11deleteStackEPS0_", h_noop },
};

static int cmp_hook(const void *a, const void *b)
{
    return strcmp(((const struct hook *)a)->name, ((const struct hook *)b)->name);
}

void *hyb_hook_lookup(const char *name)
{
    static int sorted;
    const int n = sizeof(s_hooks) / sizeof(s_hooks[0]);
    if (!sorted) {
        h_stdin = stdin;
        h_stdout = stdout;
        h_stderr = stderr;
        qsort(s_hooks, n, sizeof(s_hooks[0]), cmp_hook);
        sorted = 1;
    }
    struct hook key = { name, NULL };
    struct hook *h = bsearch(&key, s_hooks, n, sizeof(s_hooks[0]), cmp_hook);
    return h ? h->addr : NULL;
}
