/* libhyb: ARM 32-bit ELF loader for Android 11 libraries inside KitKat processes.
 * Handles: PT_LOAD and PT_GNU_RELRO segments, plain REL relocations, Android packed relocations
 * (APS2) and RELR, SysV and GNU hash tables, symbol versions (hidden ones ignored), DT_INIT and
 * DT_INIT_ARRAY, recursive DT_NEEDED, PT_ARM_EXIDX (C++ exception unwinding).
 * Symbol resolution, as in the Android linker: hooks (hooks.c) > local group (root of the dlopen
 * that loaded the library, then its dependencies breadth-first) > loaded libraries > KitKat.
 * The local group matters: a global variable defined in several libraries with different sizes
 * (g_cam_log of the camera modules) must bind to the copy of its own group. */
#define LOG_TAG "libhyb"
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <android/log.h>
#include "hyb.h"
#include "hyb_internal.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef DT_ANDROID_REL
#define DT_ANDROID_REL 0x6000000f
#define DT_ANDROID_RELSZ 0x60000010
#endif
/* explicit values: depending on the headers, DT_RELR is 36 (ELF) or 0x6fffe000 (Android) */
#define HYB_DT_RELRSZ 35
#define HYB_DT_RELR 36
#define HYB_DT_ANDROID_RELR 0x6fffe000
#define HYB_DT_ANDROID_RELRSZ 0x6fffe001
#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif
#ifndef DT_VERSYM
#define DT_VERSYM 0x6ffffff0
#endif
#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif
#ifndef PT_GNU_RELRO
#define PT_GNU_RELRO 0x6474e552
#endif
#ifndef R_ARM_IRELATIVE
#define R_ARM_IRELATIVE 160
#endif
#ifndef STT_GNU_IFUNC
#define STT_GNU_IFUNC 10
#endif

#define PAGE 4096u
#define PAGE_START(x) ((x) & ~(PAGE - 1))
#define PAGE_END(x) PAGE_START((x) + PAGE - 1)

static const char *s_root = "/system/a11";
static soinfo *s_libs, *s_libs_tail;
static pthread_mutex_t s_lock;
static pthread_once_t s_lock_once = PTHREAD_ONCE_INIT;
static void lock_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lock, &a);
}
static void lock(void) { pthread_once(&s_lock_once, lock_init); pthread_mutex_lock(&s_lock); }
static void unlock(void) { pthread_mutex_unlock(&s_lock); }
static char s_err[512];
static void *s_host[8];
static const char *const s_host_names[] = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libcutils.so", "libz.so", NULL };
/* Android 11 libraries never loaded: their functions come from the hooks */
static const char *const s_virtual_names[] = { "ld-android.so", "libdl_android.so", NULL };

static soinfo s_host_so = { .name = "kitkat", .is_host = 1 };  /* pseudo-library "KitKat + hooks" */

static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err, sizeof(s_err), fmt, ap);
    va_end(ap);
    LOGE("%s", s_err);
}

const char *hyb_dlerror(void)
{
    if (!s_err[0])
        return NULL;
    static char copy[512];
    strcpy(copy, s_err);
    s_err[0] = 0;
    return copy;
}

void hyb_set_root(const char *root) { s_root = strdup(root); }
const char *hyb_root(void) { return s_root; }

static void host_init(void)
{
    if (s_host[0])
        return;
    for (int i = 0; s_host_names[i]; i++)
        s_host[i] = dlopen(s_host_names[i], RTLD_NOW);
}

static int is_in(const char *name, const char *const *list)
{
    const char *b = strrchr(name, '/');
    b = b ? b + 1 : name;
    for (int i = 0; list[i]; i++)
        if (!strcmp(b, list[i]))
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ symbol lookup */
static uint32_t elf_hash(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    uint32_t h = 0, g;
    while (*p) {
        h = (h << 4) + *p++;
        g = h & 0xf0000000;
        h ^= g;
        h ^= g >> 24;
    }
    return h;
}

static uint32_t gnu_hash(const char *name)
{
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = h * 33 + *p;
    return h;
}

static int sym_ok(soinfo *si, uint32_t idx, const char *name)
{
    const Elf32_Sym *s = &si->symtab[idx];
    if (s->st_shndx == SHN_UNDEF)
        return 0;
    int bind = ELF32_ST_BIND(s->st_info);
    if (bind != STB_GLOBAL && bind != STB_WEAK)
        return 0;
    if (si->versym && (si->versym[idx] & 0x8000))  /* hidden version */
        return 0;
    if (si->versym && si->versym[idx] == 0)  /* VER_NDX_LOCAL */
        return 0;
    return strcmp(si->strtab + s->st_name, name) == 0;
}

const Elf32_Sym *hyb_lookup_in(soinfo *si, const char *name)
{
    if (si->is_host || !si->symtab)
        return NULL;
    if (si->gnu_nbucket) {
        uint32_t h = gnu_hash(name);
        uint32_t word = si->gnu_bloom[(h / 32) & si->gnu_maskwords];
        uint32_t mask = (1u << (h % 32)) | (1u << ((h >> si->gnu_shift2) % 32));
        if ((word & mask) != mask)
            return NULL;
        uint32_t n = si->gnu_bucket[h % si->gnu_nbucket];
        if (n == 0)
            return NULL;
        do {
            uint32_t ch = si->gnu_chain[n - si->gnu_symndx];
            if (((ch ^ h) >> 1) == 0 && sym_ok(si, n, name))
                return &si->symtab[n];
            if (ch & 1)
                break;
            n++;
        } while (1);
        return NULL;
    }
    if (si->nbucket) {
        uint32_t h = elf_hash(name);
        for (uint32_t n = si->bucket[h % si->nbucket]; n; n = si->chain[n])
            if (sym_ok(si, n, name))
                return &si->symtab[n];
    }
    return NULL;
}

static void *sym_addr(soinfo *si, const Elf32_Sym *s)
{
    uintptr_t a = si->bias + s->st_value;
    if (ELF32_ST_TYPE(s->st_info) == STT_GNU_IFUNC)
        a = ((uintptr_t(*)(void))a)();
    return (void *)a;
}

static void *host_lookup(const char *name)
{
    void *p = hyb_hook_lookup(name);
    if (p)
        return p;
    host_init();
    for (int i = 0; s_host_names[i]; i++)
        if (s_host[i] && (p = dlsym(s_host[i], name)))
            return p;
    return NULL;
}

/* global lookup: hooks, loaded Android 11 libraries, then KitKat */
static void *global_lookup(const char *name, soinfo **found)
{
    void *p = hyb_hook_lookup(name);
    if (p) {
        if (found) *found = &s_host_so;
        return p;
    }
    for (soinfo *si = s_libs; si; si = si->next) {
        const Elf32_Sym *s = hyb_lookup_in(si, name);
        if (s) {
            if (found) *found = si;
            return sym_addr(si, s);
        }
    }
    host_init();
    for (int i = 0; s_host_names[i]; i++)
        if (s_host[i] && (p = dlsym(s_host[i], name))) {
            if (found) *found = &s_host_so;
            return p;
        }
    return NULL;
}

/* lookup in the local group of si (breadth-first from its root), then global */
static void *group_lookup(soinfo *si, const char *name)
{
    void *p = hyb_hook_lookup(name);
    if (p)
        return p;
    static unsigned s_visit;
    static soinfo **q;
    static int qcap;
    int n = 0;
    for (soinfo *x = s_libs; x; x = x->next)
        n++;
    if (n > qcap) {
        qcap = n + 64;
        q = realloc(q, qcap * sizeof(*q));
    }
    unsigned v = ++s_visit;
    int head = 0, tail = 0;
    soinfo *root = si->root ? si->root : si;
    q[tail++] = root;
    root->visit = v;
    while (head < tail) {
        soinfo *x = q[head++];
        const Elf32_Sym *s = hyb_lookup_in(x, name);
        if (s)
            return sym_addr(x, s);
        for (int i = 0; i < x->nneeded; i++) {
            soinfo *d = x->needed[i];
            if (d && d->visit != v && tail < qcap) {
                d->visit = v;
                q[tail++] = d;
            }
        }
    }
    return global_lookup(name, NULL);
}

/* ------------------------------------------------------------------ missing symbols */
/* a strong symbol that cannot be found points to a small ARM sequence that logs its name
 * and aborts the process, instead of failing at load time */
static void missing_called(const char *name)
{
    LOGE("unprovided Android 11 symbol called: %s", name);
    abort();
}

static void *missing_thunk(const char *name)
{
    static uint32_t *page;
    static int used = 1024;  /* no page yet */
    if (used + 4 > 1024) {
        page = mmap(NULL, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED)
            return NULL;
        used = 0;
    }
    uint32_t *t = page + used;
    used += 4;
    t[0] = 0xe59f0000;           /* ldr r0, [pc, #0]  -> t[2] */
    t[1] = 0xe59ff000;           /* ldr pc, [pc, #0]  -> t[3] */
    t[2] = (uint32_t)strdup(name);
    t[3] = (uint32_t)missing_called;
    __builtin___clear_cache((char *)t, (char *)(t + 4));
    return t;
}

/* ------------------------------------------------------------------ relocations */
static int resolve(soinfo *si, uint32_t symidx, void **out)
{
    if (symidx == 0) {
        *out = NULL;
        return 0;
    }
    const Elf32_Sym *s = &si->symtab[symidx];
    const char *name = si->strtab + s->st_name;
    if (ELF32_ST_BIND(s->st_info) == STB_LOCAL) {
        *out = sym_addr(si, s);
        return 0;
    }
    void *p = group_lookup(si, name);
    static const char *trace;
    if (!trace)
        trace = getenv("HYB_TRACE_SYM") ? getenv("HYB_TRACE_SYM") : "";
    if (*trace && !strcmp(trace, name)) {
        soinfo *at = hyb_find_by_addr(p);
        LOGI("trace %s: wanted by %s (group %s) -> %s %p", name, si->name, si->root ? si->root->name : "?",
             at ? at->name : "host", p);
    }
    if (!p) {
        if (ELF32_ST_BIND(s->st_info) == STB_WEAK) {
            *out = NULL;
            return 0;
        }
        LOGE("%s: symbol not found %s", si->name, name);
        p = missing_thunk(name);
    }
    *out = p;
    return 0;
}

static int apply_rel(soinfo *si, uint32_t offset, uint32_t info)
{
    uint32_t type = ELF32_R_TYPE(info), idx = ELF32_R_SYM(info);
    uint32_t *where = (uint32_t *)(si->bias + offset);
    void *s = NULL;
    switch (type) {
    case R_ARM_NONE:
        return 0;
    case R_ARM_RELATIVE:
        *where += si->bias;
        return 0;
    case R_ARM_IRELATIVE:
        *where = ((uint32_t(*)(void))(si->bias + *where))();
        return 0;
    case R_ARM_ABS32:
        resolve(si, idx, &s);
        *where += (uint32_t)s;
        return 0;
    case R_ARM_REL32:
        resolve(si, idx, &s);
        *where += (uint32_t)s - (uint32_t)where;
        return 0;
    case R_ARM_GLOB_DAT:
    case R_ARM_JUMP_SLOT:
        resolve(si, idx, &s);
        *where = (uint32_t)s;
        return 0;
    default:
        set_err("%s: unsupported ARM relocation %u", si->name, type);
        return -1;
    }
}

static int sleb(const uint8_t **p, const uint8_t *end, int32_t *out)
{
    int32_t v = 0;
    int shift = 0;
    uint8_t b;
    do {
        if (*p >= end)
            return -1;
        b = *(*p)++;
        v |= (int32_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 32 && (b & 0x40))
        v |= -(1 << shift);
    *out = v;
    return 0;
}

/* Android packed relocations ("APS2") */
static int apply_android_rel(soinfo *si)
{
    const uint8_t *p = si->android_rel, *end = p + si->android_relsz;
    if (si->android_relsz < 4 || memcmp(p, "APS2", 4)) {
        set_err("%s: unknown packed relocations", si->name);
        return -1;
    }
    p += 4;
    int32_t count, offset;
    if (sleb(&p, end, &count) || sleb(&p, end, &offset))
        return -1;
    while (count > 0) {
        int32_t gsize, gflags, gdelta = 0, ginfo = 0;
        if (sleb(&p, end, &gsize) || sleb(&p, end, &gflags))
            return -1;
        if (gflags & 2 && sleb(&p, end, &gdelta))
            return -1;
        if (gflags & 1 && sleb(&p, end, &ginfo))
            return -1;
        if (gflags & 8) {
            set_err("%s: addends in packed REL relocations", si->name);
            return -1;
        }
        for (int32_t i = 0; i < gsize; i++) {
            int32_t d, info = ginfo;
            if (gflags & 2)
                d = gdelta;
            else if (sleb(&p, end, &d))
                return -1;
            offset += d;
            if (!(gflags & 1) && sleb(&p, end, &info))
                return -1;
            if (apply_rel(si, (uint32_t)offset, (uint32_t)info))
                return -1;
        }
        count -= gsize;
    }
    return 0;
}

static void apply_relr(soinfo *si)
{
    uint32_t *base = NULL;
    size_t n = si->relrsz / 4;
    for (size_t i = 0; i < n; i++) {
        uint32_t e = si->relr[i];
        if ((e & 1) == 0) {
            uint32_t *where = (uint32_t *)(si->bias + e);
            *where += si->bias;
            base = where + 1;
        } else {
            uint32_t *w = base;
            for (e >>= 1; e; e >>= 1, w++)
                if (e & 1)
                    *w += si->bias;
            base += 31;
        }
    }
}

/* ------------------------------------------------------------------ loading */
static int map_file(soinfo *si, int fd)
{
    Elf32_Ehdr eh;
    if (pread(fd, &eh, sizeof(eh), 0) != sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
        eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_machine != EM_ARM) {
        set_err("%s: not an ARM 32-bit ELF", si->path);
        return -1;
    }
    Elf32_Phdr *ph = malloc(eh.e_phnum * sizeof(*ph));
    if (pread(fd, ph, eh.e_phnum * sizeof(*ph), eh.e_phoff) != (ssize_t)(eh.e_phnum * sizeof(*ph))) {
        free(ph);
        set_err("%s: unreadable program headers", si->path);
        return -1;
    }
    uint32_t lo = UINT32_MAX, hi = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
    }
    lo = PAGE_START(lo);
    hi = PAGE_END(hi);
    void *base = mmap(NULL, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        free(ph);
        set_err("%s: cannot reserve %u bytes", si->path, hi - lo);
        return -1;
    }
    si->base = (uintptr_t)base;
    si->size = hi - lo;
    si->bias = (uintptr_t)base - lo;

    for (int i = 0; i < eh.e_phnum; i++) {
        Elf32_Phdr *p = &ph[i];
        if (p->p_type != PT_LOAD)
            continue;
        uint32_t seg_start = si->bias + p->p_vaddr, seg_end = seg_start + p->p_memsz;
        uint32_t page_start = PAGE_START(seg_start), file_end = seg_start + p->p_filesz;
        uint32_t file_page = PAGE_START(p->p_offset);
        int prot = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_W ? PROT_WRITE : 0) |
                   (p->p_flags & PF_X ? PROT_EXEC : 0);
        if (p->p_filesz) {
            void *m = mmap((void *)page_start, file_end - page_start, prot | PROT_WRITE,
                           MAP_FIXED | MAP_PRIVATE, fd, file_page);
            if (m == MAP_FAILED) {
                free(ph);
                set_err("%s: mmap of segment %d: %s", si->path, i, strerror(errno));
                return -1;
            }
        }
        /* zero the end of the last file page (start of .bss); a segment without file
         * contents (.bss on its own, as LLD emits it) is fully anonymous */
        if (p->p_filesz && (p->p_flags & PF_W) && (file_end & (PAGE - 1)))
            memset((void *)file_end, 0, PAGE - (file_end & (PAGE - 1)));
        uint32_t zero_start = p->p_filesz ? PAGE_END(file_end) : page_start;
        if (seg_end > zero_start) {
            void *m = mmap((void *)zero_start, PAGE_END(seg_end) - zero_start, prot,
                           MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m == MAP_FAILED) {
                free(ph);
                set_err("%s: bss: %s", si->path, strerror(errno));
                return -1;
            }
        }
        if (p->p_filesz && !(p->p_flags & PF_W))
            mprotect((void *)page_start, file_end - page_start, prot);
    }
    /* copy of the program headers (dl_iterate_phdr, dladdr) */
    si->phdr = ph;
    si->phnum = eh.e_phnum;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC)
            si->dynamic = (Elf32_Dyn *)(si->bias + ph[i].p_vaddr);
        if (ph[i].p_type == PT_ARM_EXIDX) {
            si->arm_exidx = (uint32_t *)(si->bias + ph[i].p_vaddr);
            si->arm_exidx_count = ph[i].p_memsz / 8;
        }
    }
    if (!si->dynamic) {
        set_err("%s: no PT_DYNAMIC", si->path);
        return -1;
    }
    return 0;
}

static void parse_dynamic(soinfo *si)
{
    for (Elf32_Dyn *d = si->dynamic; d->d_tag != DT_NULL; d++) {
        uint32_t v = d->d_un.d_val, a = si->bias + d->d_un.d_ptr;
        switch (d->d_tag) {
        case DT_STRTAB: si->strtab = (const char *)a; break;
        case DT_SYMTAB: si->symtab = (Elf32_Sym *)a; break;
        case DT_HASH:
            si->nbucket = ((uint32_t *)a)[0];
            si->nchain = ((uint32_t *)a)[1];
            si->bucket = (uint32_t *)a + 2;
            si->chain = si->bucket + si->nbucket;
            break;
        case DT_GNU_HASH: {
            uint32_t *g = (uint32_t *)a;
            si->gnu_nbucket = g[0];
            si->gnu_symndx = g[1];
            si->gnu_maskwords = g[2] - 1;  /* mask (power of 2) */
            si->gnu_shift2 = g[3];
            si->gnu_bloom = g + 4;
            si->gnu_bucket = si->gnu_bloom + g[2];
            si->gnu_chain = si->gnu_bucket + si->gnu_nbucket;
            break;
        }
        case DT_VERSYM: si->versym = (uint16_t *)a; break;
        case DT_REL: si->rel = (Elf32_Rel *)a; break;
        case DT_RELSZ: si->reln = v / sizeof(Elf32_Rel); break;
        case DT_JMPREL: si->plt_rel = (Elf32_Rel *)a; break;
        case DT_PLTRELSZ: si->plt_reln = v / sizeof(Elf32_Rel); break;
        case DT_ANDROID_REL: si->android_rel = (uint8_t *)a; break;
        case DT_ANDROID_RELSZ: si->android_relsz = v; break;
        case HYB_DT_RELR: case HYB_DT_ANDROID_RELR: si->relr = (uint32_t *)a; break;
        case HYB_DT_RELRSZ: case HYB_DT_ANDROID_RELRSZ: si->relrsz = v; break;
        case DT_INIT: si->init = (void (*)(void))a; break;
        case DT_INIT_ARRAY: si->init_array = (void (**)(int, char **, char **))a; break;
        case DT_INIT_ARRAYSZ: si->init_array_count = v / 4; break;
        case DT_FINI_ARRAY: si->fini_array = (void (**)(void))a; break;
        case DT_FINI_ARRAYSZ: si->fini_array_count = v / 4; break;
        }
    }
}

static soinfo *find_loaded(const char *name)
{
    const char *b = strrchr(name, '/');
    b = b ? b + 1 : name;
    for (soinfo *si = s_libs; si; si = si->next)
        if (!strcmp(si->name, b) || !strcmp(si->path, name))
            return si;
    return NULL;
}

/* real path: plain names are looked up in the Android 11 root, absolute Android paths rebased */
static int locate(const char *name, char *out, size_t len)
{
    struct stat st;
    if (name[0] == '/') {
        if (!strncmp(name, s_root, strlen(s_root)) && stat(name, &st) == 0) {
            snprintf(out, len, "%s", name);
            return 0;
        }
        snprintf(out, len, "%s%s", s_root, name);
        if (stat(out, &st) == 0)
            return 0;
        /* /system/lib/hw/... missing on the Android 11 side: try the bare name */
        name = strrchr(name, '/') + 1;
    }
    static const char *const dirs[] = { "/vendor/lib/egl", "/vendor/lib/hw", "/vendor/lib",
                                        "/system/lib/hw", "/system/lib", NULL };
    for (int i = 0; dirs[i]; i++) {
        snprintf(out, len, "%s%s/%s", s_root, dirs[i], name);
        if (stat(out, &st) == 0)
            return 0;
    }
    return -1;
}

static void call_init(soinfo *si)
{
    if (si->inited)
        return;
    si->inited = 1;
    for (int i = 0; i < si->nneeded; i++)
        if (si->needed[i])
            call_init(si->needed[i]);
    static char *argv[] = { "hyb", NULL };
    extern char **environ;
    if (si->init)
        si->init();
    for (size_t i = 0; i < si->init_array_count; i++) {
        void (*f)(int, char **, char **) = si->init_array[i];
        if ((uintptr_t)f != 0 && (uintptr_t)f != (uintptr_t)-1)
            f(1, argv, environ);
    }
}

/* GL loaders of the Android 11 platform: they look for their driver through Android 11
 * properties and files, missing here, and abort on an assertion. Serve the Adreno driver
 * directly, it exports the same functions (camera: EIS, c2d on the GPU). */
static const char *gl_alias(const char *name)
{
    static const char *const map[][2] = {
        { "libEGL.so", "libEGL_adreno.so" }, { "libGLESv2.so", "libGLESv2_adreno.so" },
        { "libGLESv3.so", "libGLESv2_adreno.so" }, { "libGLESv1_CM.so", "libGLESv1_CM_adreno.so" },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(name, map[i][0]))
            return map[i][1];
    return name;
}

static void apply_patches(soinfo *si);

static soinfo *load(const char *name, soinfo *root)
{
    name = gl_alias(name);
    if (is_in(name, s_host_names) || is_in(name, s_virtual_names))
        return &s_host_so;
    soinfo *si = find_loaded(name);
    if (si)
        return si;
    char path[512];
    if (locate(name, path, sizeof(path))) {
        set_err("library not found: %s", name);
        return NULL;
    }
    if ((si = find_loaded(path)))
        return si;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_err("%s : %s", path, strerror(errno));
        return NULL;
    }
    si = calloc(1, sizeof(*si));
    si->root = root ? root : si;
    snprintf(si->path, sizeof(si->path), "%s", path);
    snprintf(si->name, sizeof(si->name), "%s", strrchr(path, '/') + 1);
    int r = map_file(si, fd);
    close(fd);
    if (r) {
        free(si);
        return NULL;
    }
    parse_dynamic(si);
    apply_patches(si);
    /* registered before its dependencies: cycles resolve to it */
    if (s_libs_tail) s_libs_tail->next = si; else s_libs = si;
    s_libs_tail = si;

    for (Elf32_Dyn *d = si->dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED || si->nneeded >= MAX_NEEDED)
            continue;
        const char *dep = si->strtab + d->d_un.d_val;
        soinfo *ds = load(dep, si->root);
        if (!ds) {
            LOGE("%s: dependency %s: %s", si->name, dep, s_err);
            return NULL;
        }
        si->needed[si->nneeded++] = ds == &s_host_so ? NULL : ds;
    }
    LOGI("loaded: %s at %#x", si->path, si->base);
    return si;
}

/* binary patches applied at load time, only when the original code matches */
struct patch { const char *lib; uint32_t vaddr; uint8_t old[4], new_[4]; const char *why; };
static const struct patch s_patches[] = {
    /* ServiceManagerShim::getService: waits 5 s for a missing service (KitKat's servicemanager
     * is incompatible) on every camera open (display.qservice): movw r5, #5000 -> #0 */
    { "libbinder.so", 0x3c6de, { 0x41, 0xf2, 0x88, 0x35 }, { 0x40, 0xf2, 0x00, 0x05 }, "getService without waiting" },
};

static void apply_patches(soinfo *si)
{
    for (unsigned i = 0; i < sizeof(s_patches) / sizeof(s_patches[0]); i++) {
        const struct patch *p = &s_patches[i];
        if (strcmp(si->name, p->lib))
            continue;
        uint8_t *at = (uint8_t *)(si->bias + p->vaddr);
        if (memcmp(at, p->old, 4)) {
            LOGE("%s: patch \"%s\" not applied (different code)", si->name, p->why);
            continue;
        }
        uintptr_t pg = PAGE_START((uintptr_t)at);
        mprotect((void *)pg, PAGE * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
        memcpy(at, p->new_, 4);
        mprotect((void *)pg, PAGE * 2, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)at, (char *)at + 4);
        LOGI("%s: patch \"%s\"", si->name, p->why);
    }
}

/* relocations, once the whole dlopen tree is loaded: as in the Android linker, so that the
 * local group (root, then dependencies breadth-first) is complete when symbols are resolved */
static int link_one(soinfo *si)
{
    if (si->android_rel && apply_android_rel(si))
        return -1;
    for (size_t i = 0; i < si->reln; i++)
        if (apply_rel(si, si->rel[i].r_offset, si->rel[i].r_info))
            return -1;
    if (si->relr)
        apply_relr(si);
    for (size_t i = 0; i < si->plt_reln; i++)
        if (apply_rel(si, si->plt_rel[i].r_offset, si->plt_rel[i].r_info))
            return -1;
    for (int i = 0; i < si->phnum; i++)
        if (si->phdr[i].p_type == PT_GNU_RELRO) {
            uint32_t s = PAGE_START(si->bias + si->phdr[i].p_vaddr);
            uint32_t e = PAGE_END(si->bias + si->phdr[i].p_vaddr + si->phdr[i].p_memsz);
            mprotect((void *)s, e - s, PROT_READ);
        }
    si->linked = 1;
    return 0;
}

static int link_new(void)
{
    for (soinfo *si = s_libs; si; si = si->next)
        if (!si->linked && link_one(si)) {
            LOGE("%s: relocation: %s", si->name, s_err);
            return -1;
        }
    return 0;
}

void *hyb_dlopen(const char *name, int flags)
{
    if (!name)
        return &s_host_so;
    lock();
    soinfo *si = load(name, NULL);
    if (si && link_new())
        si = NULL;
    if (si && si != &s_host_so)
        call_init(si);
    if (si)
        si->refcount++;
    unlock();
    return si;
}

/* dlsym on a library: the library, then its dependencies (breadth-first) */
void *hyb_dlsym(void *handle, const char *name)
{
    if (handle == RTLD_DEFAULT || handle == &s_host_so || handle == NULL) {
        void *p = global_lookup(name, NULL);
        if (!p)
            set_err("symbol not found: %s", name);
        return p;
    }
    if (handle == RTLD_NEXT)
        return global_lookup(name, NULL);
    soinfo *q[256];
    int head = 0, tail = 0;
    q[tail++] = handle;
    lock();
    while (head < tail) {
        soinfo *si = q[head++];
        const Elf32_Sym *s = hyb_lookup_in(si, name);
        if (s) {
            unlock();
            return sym_addr(si, s);
        }
        for (int i = 0; i < si->nneeded && tail < 256; i++) {
            int dup = 0;
            for (int j = 0; j < tail; j++) dup |= q[j] == si->needed[i];
            if (si->needed[i] && !dup)
                q[tail++] = si->needed[i];
        }
    }
    unlock();
    void *p = host_lookup(name);
    if (!p)
        set_err("symbol not found: %s", name);
    return p;
}

int hyb_dlclose(void *handle)
{
    /* never unloaded (like bionic does for most drivers) */
    return 0;
}

/* ------------------------------------------------------------------ introspection */
soinfo *hyb_find_by_addr(const void *addr)
{
    uintptr_t a = (uintptr_t)addr;
    for (soinfo *si = s_libs; si; si = si->next)
        if (a >= si->base && a < si->base + si->size)
            return si;
    return NULL;
}

int hyb_dladdr(const void *addr, Dl_info *info)
{
    soinfo *si = hyb_find_by_addr(addr);
    if (!si)
        return dladdr(addr, info);
    memset(info, 0, sizeof(*info));
    info->dli_fname = si->path;
    info->dli_fbase = (void *)si->base;
    /* nearest symbol below */
    uintptr_t best = 0;
    uint32_t nsyms = si->nchain;
    if (!nsyms && si->gnu_nbucket) {
        /* number of symbols from the GNU table: highest chain index */
        uint32_t maxi = 0;
        for (uint32_t b = 0; b < si->gnu_nbucket; b++)
            if (si->gnu_bucket[b] > maxi) maxi = si->gnu_bucket[b];
        if (maxi) {
            while (!(si->gnu_chain[maxi - si->gnu_symndx] & 1)) maxi++;
            nsyms = maxi + 1;
        }
    }
    for (uint32_t i = 1; i < nsyms; i++) {
        const Elf32_Sym *s = &si->symtab[i];
        if (s->st_shndx == SHN_UNDEF || !s->st_value)
            continue;
        uintptr_t sa = si->bias + s->st_value;
        if (sa <= (uintptr_t)addr && sa > best && (uintptr_t)addr < sa + (s->st_size ? s->st_size : 1)) {
            best = sa;
            info->dli_sname = si->strtab + s->st_name;
            info->dli_saddr = (void *)sa;
        }
    }
    return 1;
}

int hyb_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data)
{
    for (soinfo *si = s_libs; si; si = si->next) {
        if (!si->linked)
            continue;
        struct dl_phdr_info info;
        memset(&info, 0, sizeof(info));
        info.dlpi_addr = si->bias;
        info.dlpi_name = si->path;
        info.dlpi_phdr = si->phdr;
        info.dlpi_phnum = si->phnum;
        int r = cb(&info, sizeof(info), data);
        if (r)
            return r;
    }
    /* KitKat libraries (dl_iterate_phdr is not in the NDK API 19 headers) */
    typedef int (*iter_t)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
    static iter_t host;
    if (!host)
        host = (iter_t)dlsym(RTLD_DEFAULT, "dl_iterate_phdr");
    return host ? host(cb, data) : 0;
}

uintptr_t *hyb_unwind_find_exidx(uintptr_t pc, int *count)
{
    soinfo *si = hyb_find_by_addr((void *)pc);
    if (si) {
        *count = si->arm_exidx_count;
        return (uintptr_t *)si->arm_exidx;
    }
    typedef uintptr_t *(*find_t)(uintptr_t, int *);
    static find_t host;
    if (!host) {
        host_init();
        host = (find_t)dlsym(s_host[2], "dl_unwind_find_exidx");
    }
    if (host)
        return host(pc, count);
    *count = 0;
    return NULL;
}
