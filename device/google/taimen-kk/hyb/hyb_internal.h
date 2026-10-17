/* libhyb internal structures */
#ifndef HYB_INTERNAL_H
#define HYB_INTERNAL_H
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdint.h>

#define MAX_NEEDED 64

typedef struct soinfo {
    char name[128];
    char path[256];
    uintptr_t base, bias;
    size_t size;
    const Elf32_Phdr *phdr;
    int phnum;
    Elf32_Dyn *dynamic;
    const char *strtab;
    Elf32_Sym *symtab;
    uint32_t nbucket, nchain, *bucket, *chain;
    uint32_t gnu_nbucket, gnu_symndx, gnu_maskwords, gnu_shift2;
    uint32_t *gnu_bloom, *gnu_bucket, *gnu_chain;
    uint16_t *versym;
    Elf32_Rel *rel, *plt_rel;
    size_t reln, plt_reln;
    uint8_t *android_rel;
    size_t android_relsz;
    uint32_t *relr;
    size_t relrsz;
    void (*init)(void);
    void (**init_array)(int, char **, char **);
    size_t init_array_count;
    void (**fini_array)(void);
    size_t fini_array_count;
    uint32_t *arm_exidx;
    size_t arm_exidx_count;
    struct soinfo *needed[MAX_NEEDED];
    int nneeded;
    int refcount, inited, linked, is_host;
    struct soinfo *root;   /* root of the dlopen that loaded it: its resolution group */
    unsigned visit;        /* traversal mark (group_lookup) */
    struct soinfo *next;
} soinfo;

/* hooks.c: symbol provided by a hook (or NULL) */
void *hyb_hook_lookup(const char *name);

/* linker.c */
const char *hyb_root(void);
soinfo *hyb_find_by_addr(const void *addr);
int hyb_dladdr(const void *addr, Dl_info *info);
int hyb_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data);
uintptr_t *hyb_unwind_find_exidx(uintptr_t pc, int *count);
#endif
