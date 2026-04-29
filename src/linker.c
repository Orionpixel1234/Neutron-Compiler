/*
 * linker.c — Neutron ELF static linker (i386 + x86-64)
 *
 * Usage: neutron-ld [-o output] [-m elf_i386|elf_x86_64] [-T script] file.o ...
 *
 * Supported ELF32 relocations (i386):
 *   R_386_32    absolute 32-bit
 *   R_386_PC32  PC-relative 32-bit
 *
 * Supported ELF64 relocations (x86-64):
 *   R_X86_64_64, R_X86_64_PC32, R_X86_64_PLT32, R_X86_64_32, R_X86_64_32S
 *
 * Linker script support: ENTRY(sym) and ". = addr" (sets load base).
 *
 * Output layout:
 *   file 0x0000 : ELF header + program headers
 *   file 0x1000 : .text  (merged, exec+read)
 *   file 0x1000+align : .rodata
 *   file page-align  : .data
 *   (virtual only)   : .bss
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>

/* ================================================================
 * ELF types — 64-bit
 * ================================================================ */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;
typedef int64_t  i64;

typedef struct { u8 e_ident[16]; u16 e_type; u16 e_machine; u32 e_version;
                 u64 e_entry; u64 e_phoff; u64 e_shoff; u32 e_flags;
                 u16 e_ehsize; u16 e_phentsize; u16 e_phnum;
                 u16 e_shentsize; u16 e_shnum; u16 e_shstrndx; } Elf64Ehdr;
typedef struct { u32 sh_name; u32 sh_type; u64 sh_flags; u64 sh_addr;
                 u64 sh_offset; u64 sh_size; u32 sh_link; u32 sh_info;
                 u64 sh_addralign; u64 sh_entsize; } Elf64Shdr;
typedef struct { u32 st_name; u8 st_info; u8 st_other; u16 st_shndx;
                 u64 st_value; u64 st_size; } Elf64Sym;
typedef struct { u64 r_offset; u64 r_info; i64 r_addend; } Elf64Rela;
typedef struct { u32 p_type; u32 p_flags; u64 p_offset;
                 u64 p_vaddr; u64 p_paddr; u64 p_filesz;
                 u64 p_memsz; u64 p_align; } Elf64Phdr;

/* ================================================================
 * ELF types — 32-bit
 * ================================================================ */

typedef struct { u8 e_ident[16]; u16 e_type; u16 e_machine; u32 e_version;
                 u32 e_entry; u32 e_phoff; u32 e_shoff; u32 e_flags;
                 u16 e_ehsize; u16 e_phentsize; u16 e_phnum;
                 u16 e_shentsize; u16 e_shnum; u16 e_shstrndx; } Elf32Ehdr;
typedef struct { u32 sh_name; u32 sh_type; u32 sh_flags; u32 sh_addr;
                 u32 sh_offset; u32 sh_size; u32 sh_link; u32 sh_info;
                 u32 sh_addralign; u32 sh_entsize; } Elf32Shdr;
typedef struct { u32 st_name; u32 st_value; u32 st_size;
                 u8  st_info; u8  st_other; u16 st_shndx; } Elf32Sym;
typedef struct { u32 r_offset; u32 r_info; } Elf32Rel;
typedef struct { u32 r_offset; u32 r_info; i32 r_addend; } Elf32Rela;
typedef struct { u32 p_type; u32 p_offset; u32 p_vaddr; u32 p_paddr;
                 u32 p_filesz; u32 p_memsz; u32 p_flags; u32 p_align; } Elf32Phdr;

/* ================================================================
 * ELF constants
 * ================================================================ */

#define ELFMAG0      0x7f
#define ELFCLASS32   1
#define ELFCLASS64   2
#define ELFDATA2LSB  1
#define ET_EXEC      2
#define EM_386       3
#define EM_X86_64    62
#define EV_CURRENT   1
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_REL      9
#define SHT_NOBITS   8
#define PT_LOAD      1
#define PF_X   0x1
#define PF_W   0x2
#define PF_R   0x4
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2
#define SHN_UNDEF  0
#define SHN_ABS    0xfff1

/* x86-64 reloc types */
#define R_X86_64_NONE  0
#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4
#define R_X86_64_32    10
#define R_X86_64_32S   11

/* i386 reloc types */
#define R_386_32   1
#define R_386_PC32 2

#define ELF64_R_SYM(i)    ((u32)((i) >> 32))
#define ELF64_R_TYPE(i)   ((u32)((i) & 0xffffffff))
#define ELF64_ST_BIND(i)  ((i) >> 4)
#define ELF32_R_SYM(i)    ((i) >> 8)
#define ELF32_R_TYPE(i)   ((i) & 0xff)
#define ELF32_ST_BIND(i)  ((i) >> 4)

/* ================================================================
 * Layout constants (configurable via linker script)
 * ================================================================ */

#define DEFAULT_LOAD_BASE_64  ((u64)0x400000)
#define DEFAULT_LOAD_BASE_32  ((u64)0x400000)
#define PAGE_SIZE  ((u64)0x1000)
#define HDR_SIZE   ((u64)0x1000)
#define ALIGN_UP(x,a) (((x)+((a)-1))&~((a)-1))

/* ================================================================
 * Global link state
 * ================================================================ */

static int    link_32bit  = 0;
static u64    load_base   = 0;          /* 0 = use default */
static char   entry_sym[256] = "_start";

/* ================================================================
 * Error helper
 * ================================================================ */

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "neutron-ld: "); vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n"); va_end(ap); exit(1);
}

/* ================================================================
 * Merged section buffer
 * ================================================================ */

typedef struct {
    u8  *buf;
    u64  size, cap, align;
    u64  vaddr, file_off;
} Sec;

static Sec sec_text   = { .align = 1 };
static Sec sec_rodata = { .align = 1 };
static Sec sec_data   = { .align = 1 };
static u64 bss_size  = 0, bss_vaddr = 0, bss_align = 1;

static u64 sec_append(Sec *s, const u8 *src, u64 size, u64 align) {
    if (align < 1) align = 1;
    u64 pad  = (align - (s->size % align)) % align;
    u64 need = s->size + pad + size;
    if (need > s->cap) {
        s->cap = need < 65536 ? 65536 : need * 2;
        s->buf = realloc(s->buf, s->cap);
        if (!s->buf) die("out of memory");
    }
    memset(s->buf + s->size, 0, pad);
    s->size += pad;
    u64 off = s->size;
    if (src) memcpy(s->buf + s->size, src, size);
    else     memset(s->buf + s->size, 0, size);
    s->size += size;
    if (align > s->align) s->align = align;
    return off;
}

static Sec *sec_for(const char *n) {
    if (!strcmp(n,".text") || !strncmp(n,".text.",6) ||
        !strcmp(n,".init") || !strcmp(n,".fini"))  return &sec_text;
    if (!strcmp(n,".rodata") || !strncmp(n,".rodata.",8) ||
        !strcmp(n,".data.rel.ro") || !strncmp(n,".data.rel.ro.",13))
                                                    return &sec_rodata;
    if (!strcmp(n,".data") || !strncmp(n,".data.",6)) return &sec_data;
    return NULL;
}

/* ================================================================
 * Input object state
 * ================================================================ */

#define MAX_OBJS 64

typedef struct {
    const char *path;
    u8         *data;
    size_t      size;
    int         elf_class;   /* ELFCLASS32 or ELFCLASS64 */

    /* ELF64 fields */
    Elf64Ehdr  *ehdr64;
    Elf64Shdr  *shdrs64;
    Elf64Sym   *syms64;
    int         nsyms64;

    /* ELF32 fields */
    Elf32Ehdr  *ehdr32;
    Elf32Shdr  *shdrs32;
    Elf32Sym   *syms32;
    int         nsyms32;

    const char *shstrtab;
    const char *strtab;
    int         shnum;
    u64        *sec_out_off;
} Obj;

static Obj objs[MAX_OBJS];
static int nobj;

/* ================================================================
 * Global symbol table
 * ================================================================ */

#define MAX_GSYMS 8192

typedef struct { char name[256]; u64 vaddr; int defined; } GSym;
static GSym gsyms[MAX_GSYMS];
static int  ngsyms;

static GSym *gsym_find(const char *n) {
    for (int i = 0; i < ngsyms; i++)
        if (!strcmp(gsyms[i].name, n)) return &gsyms[i];
    return NULL;
}
static GSym *gsym_get(const char *n) {
    GSym *g = gsym_find(n);
    if (g) return g;
    if (ngsyms >= MAX_GSYMS) die("too many global symbols");
    g = &gsyms[ngsyms++];
    strncpy(g->name, n, sizeof g->name - 1);
    g->name[sizeof g->name - 1] = '\0';
    g->defined = 0; g->vaddr = 0;
    return g;
}

/* ================================================================
 * Minimal linker script parser
 * Handles: ENTRY(sym)  and  . = 0xADDR;
 * ================================================================ */

static void parse_linker_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open linker script '%s': %s", path, strerror(errno));

    char line[512];
    while (fgets(line, sizeof line, f)) {
        /* Strip block and line comments */
        char *cm = strstr(line, "/*");
        if (cm) *cm = '\0';
        cm = strstr(line, "//");
        if (cm) *cm = '\0';

        /* ENTRY(symbol) — scan anywhere in line */
        char *ep = strstr(line, "ENTRY");
        if (ep) {
            char *lp = strchr(ep, '(');
            char *rp = lp ? strchr(lp, ')') : NULL;
            if (lp && rp && rp > lp + 1) {
                int n = (int)(rp - lp - 1);
                if (n >= (int)sizeof entry_sym) n = (int)sizeof entry_sym - 1;
                strncpy(entry_sym, lp + 1, n);
                entry_sym[n] = '\0';
            }
        }

        /*
         * ". = addr" — scan anywhere in line (handles SECTIONS { . = 0x7E00; })
         * Only the first plain numeric assignment sets the load base.
         */
        if (load_base == 0) {
            char *p = line;
            while ((p = strchr(p, '.')) != NULL) {
                /* Must be ". =" or ".=" */
                char *q = p + 1;
                while (*q == ' ' || *q == '\t') q++;
                if (*q != '=') { p++; continue; }
                q++;
                while (*q == ' ' || *q == '\t') q++;
                /* Skip ALIGN() and non-numeric */
                if (strncmp(q, "ALIGN", 5) == 0) { p++; continue; }
                if (*q != '0' && (*q < '1' || *q > '9')) { p++; continue; }
                u64 addr = (u64)strtoull(q, NULL, 0);
                if (addr != 0) { load_base = addr; break; }
                p++;
            }
        }
    }
    fclose(f);
}

/* ================================================================
 * Pass 1 — load and parse objects
 * ================================================================ */

static void load_obj(const char *path) {
    if (nobj >= MAX_OBJS) die("too many input files");
    Obj *o = &objs[nobj++];
    o->path = path;

    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open '%s': %s", path, strerror(errno));
    fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
    o->size = (size_t)fsz;
    o->data = malloc(o->size);
    if (!o->data) die("out of memory");
    if (fread(o->data, 1, o->size, f) != o->size) die("read error: %s", path);
    fclose(f);

    if (o->size < 16) die("%s: too small to be ELF", path);
    if (o->data[0] != ELFMAG0 || o->data[1] != 'E' ||
        o->data[2] != 'L'     || o->data[3] != 'F')
        die("%s: not an ELF file", path);

    o->elf_class = o->data[4];

    if (o->elf_class == ELFCLASS64) {
        if (o->size < sizeof(Elf64Ehdr)) die("%s: truncated ELF64", path);
        o->ehdr64   = (Elf64Ehdr *)o->data;
        if (o->ehdr64->e_machine != EM_X86_64)
            die("%s: expected x86-64 machine", path);
        o->shdrs64  = (Elf64Shdr *)(o->data + o->ehdr64->e_shoff);
        o->shnum    = o->ehdr64->e_shnum;
        o->shstrtab = (const char *)(o->data +
                       o->shdrs64[o->ehdr64->e_shstrndx].sh_offset);
        for (int i = 0; i < o->shnum; i++) {
            Elf64Shdr *sh = &o->shdrs64[i];
            if (sh->sh_type == SHT_SYMTAB) {
                o->syms64  = (Elf64Sym *)(o->data + sh->sh_offset);
                o->nsyms64 = (int)(sh->sh_size / sizeof(Elf64Sym));
                o->strtab  = (const char *)(o->data +
                               o->shdrs64[sh->sh_link].sh_offset);
                break;
            }
        }

    } else if (o->elf_class == ELFCLASS32) {
        if (o->size < sizeof(Elf32Ehdr)) die("%s: truncated ELF32", path);
        o->ehdr32   = (Elf32Ehdr *)o->data;
        if (o->ehdr32->e_machine != EM_386)
            die("%s: expected i386 machine", path);
        o->shdrs32  = (Elf32Shdr *)(o->data + o->ehdr32->e_shoff);
        o->shnum    = o->ehdr32->e_shnum;
        o->shstrtab = (const char *)(o->data +
                       o->shdrs32[o->ehdr32->e_shstrndx].sh_offset);
        for (int i = 0; i < o->shnum; i++) {
            Elf32Shdr *sh = &o->shdrs32[i];
            if (sh->sh_type == SHT_SYMTAB) {
                o->syms32  = (Elf32Sym *)(o->data + sh->sh_offset);
                o->nsyms32 = (int)(sh->sh_size / sizeof(Elf32Sym));
                o->strtab  = (const char *)(o->data +
                               o->shdrs32[sh->sh_link].sh_offset);
                break;
            }
        }
    } else {
        die("%s: unsupported ELF class %d", path, o->elf_class);
    }

    o->sec_out_off = malloc(o->shnum * sizeof(u64));
    for (int i = 0; i < o->shnum; i++) o->sec_out_off[i] = (u64)-1;
}

/* ================================================================
 * Pass 2 — merge sections
 * ================================================================ */

static void merge_sections(void) {
    for (int oi = 0; oi < nobj; oi++) {
        Obj *o = &objs[oi];

        if (o->elf_class == ELFCLASS64) {
            for (int si = 0; si < o->shnum; si++) {
                Elf64Shdr  *sh = &o->shdrs64[si];
                const char *nm = o->shstrtab + sh->sh_name;
                u64 align = sh->sh_addralign ? sh->sh_addralign : 1;

                if (sh->sh_type == SHT_NOBITS &&
                    (!strcmp(nm,".bss") || !strncmp(nm,".bss.",5))) {
                    u64 pad = (align - (bss_size % align)) % align;
                    o->sec_out_off[si] = bss_size + pad;
                    bss_size += pad + sh->sh_size;
                    if (align > bss_align) bss_align = align;
                    continue;
                }
                if (sh->sh_type != SHT_PROGBITS) continue;
                Sec *dst = sec_for(nm);
                if (!dst) continue;
                o->sec_out_off[si] = sec_append(dst,
                    o->data + sh->sh_offset, sh->sh_size, align);
            }

        } else { /* ELFCLASS32 */
            for (int si = 0; si < o->shnum; si++) {
                Elf32Shdr  *sh = &o->shdrs32[si];
                const char *nm = o->shstrtab + sh->sh_name;
                u64 align = sh->sh_addralign ? sh->sh_addralign : 1;

                if (sh->sh_type == SHT_NOBITS &&
                    (!strcmp(nm,".bss") || !strncmp(nm,".bss.",5))) {
                    u64 pad = (align - (bss_size % align)) % align;
                    o->sec_out_off[si] = bss_size + pad;
                    bss_size += pad + sh->sh_size;
                    if (align > bss_align) bss_align = align;
                    continue;
                }
                if (sh->sh_type != SHT_PROGBITS) continue;
                Sec *dst = sec_for(nm);
                if (!dst) continue;
                o->sec_out_off[si] = sec_append(dst,
                    o->data + sh->sh_offset, sh->sh_size, align);
            }
        }
    }
}

/* ================================================================
 * Pass 3 — layout
 * ================================================================ */

static void layout(void) {
    u64 base = load_base ? load_base : (link_32bit ? DEFAULT_LOAD_BASE_32 : DEFAULT_LOAD_BASE_64);

    sec_text.file_off = HDR_SIZE;
    sec_text.vaddr    = base;

    u64 ra = sec_rodata.align > 1 ? sec_rodata.align : 16;
    sec_rodata.file_off = ALIGN_UP(sec_text.file_off + sec_text.size, ra);
    sec_rodata.vaddr    = base + (sec_rodata.file_off - HDR_SIZE);

    u64 rx_end = sec_rodata.file_off + sec_rodata.size;
    sec_data.file_off = ALIGN_UP(rx_end, PAGE_SIZE);
    sec_data.vaddr    = base + (sec_data.file_off - HDR_SIZE);

    u64 ba = bss_align > 1 ? bss_align : 8;
    bss_vaddr = ALIGN_UP(sec_data.vaddr + sec_data.size, ba);
}

/* ================================================================
 * Pass 4 — resolve symbol virtual addresses
 * ================================================================ */

static u64 sym_to_vaddr64(Obj *o, Elf64Sym *sym) {
    u16 idx = sym->st_shndx;
    if (idx == SHN_ABS)   return sym->st_value;
    if (idx == SHN_UNDEF || idx >= (u16)o->shnum) return (u64)-1;
    Elf64Shdr  *sh = &o->shdrs64[idx];
    const char *nm = o->shstrtab + sh->sh_name;
    u64 out_off    = o->sec_out_off[idx];
    if (out_off == (u64)-1) return (u64)-1;
    if (sh->sh_type == SHT_NOBITS &&
        (!strcmp(nm,".bss") || !strncmp(nm,".bss.",5)))
        return bss_vaddr + out_off + sym->st_value;
    Sec *s = sec_for(nm);
    if (!s) return (u64)-1;
    return s->vaddr + out_off + sym->st_value;
}

static u64 sym_to_vaddr32(Obj *o, Elf32Sym *sym) {
    u16 idx = sym->st_shndx;
    if (idx == SHN_ABS)   return (u64)sym->st_value;
    if (idx == SHN_UNDEF || idx >= (u16)o->shnum) return (u64)-1;
    Elf32Shdr  *sh = &o->shdrs32[idx];
    const char *nm = o->shstrtab + sh->sh_name;
    u64 out_off    = o->sec_out_off[idx];
    if (out_off == (u64)-1) return (u64)-1;
    if (sh->sh_type == SHT_NOBITS &&
        (!strcmp(nm,".bss") || !strncmp(nm,".bss.",5)))
        return bss_vaddr + out_off + sym->st_value;
    Sec *s = sec_for(nm);
    if (!s) return (u64)-1;
    return s->vaddr + out_off + sym->st_value;
}

static void build_symtab(void) {
    for (int oi = 0; oi < nobj; oi++) {
        Obj *o = &objs[oi];
        if (o->elf_class == ELFCLASS64) {
            if (!o->syms64) continue;
            for (int si = 0; si < o->nsyms64; si++) {
                Elf64Sym *sym = &o->syms64[si];
                if (ELF64_ST_BIND(sym->st_info) == STB_LOCAL) continue;
                if (sym->st_shndx == SHN_UNDEF) continue;
                const char *name = o->strtab + sym->st_name;
                if (!name[0]) continue;
                u64 va = sym_to_vaddr64(o, sym);
                if (va == (u64)-1) continue;
                GSym *g = gsym_get(name);
                if (!g->defined) { g->vaddr = va; g->defined = 1; }
            }
        } else {
            if (!o->syms32) continue;
            for (int si = 0; si < o->nsyms32; si++) {
                Elf32Sym *sym = &o->syms32[si];
                if (ELF32_ST_BIND(sym->st_info) == STB_LOCAL) continue;
                if (sym->st_shndx == SHN_UNDEF) continue;
                const char *name = o->strtab + sym->st_name;
                if (!name[0]) continue;
                u64 va = sym_to_vaddr32(o, sym);
                if (va == (u64)-1) continue;
                GSym *g = gsym_get(name);
                if (!g->defined) { g->vaddr = va; g->defined = 1; }
            }
        }
    }
}

/* ================================================================
 * Pass 5 — apply relocations
 * ================================================================ */

static u64 resolve64(Obj *o, u32 sym_idx) {
    if (!o->syms64 || (int)sym_idx >= o->nsyms64) die("bad symbol index");
    Elf64Sym *sym = &o->syms64[sym_idx];
    if (sym->st_shndx == SHN_ABS) return sym->st_value;
    if (ELF64_ST_BIND(sym->st_info) != STB_LOCAL &&
        sym->st_shndx == SHN_UNDEF) {
        const char *n = o->strtab + sym->st_name;
        GSym *g = gsym_find(n);
        if (!g || !g->defined) die("undefined symbol: %s", n);
        return g->vaddr;
    }
    u64 va = sym_to_vaddr64(o, sym);
    if (va == (u64)-1) die("unresolvable symbol in %s", o->path);
    return va;
}

static u64 resolve32(Obj *o, u32 sym_idx) {
    if (!o->syms32 || (int)sym_idx >= o->nsyms32) die("bad symbol index");
    Elf32Sym *sym = &o->syms32[sym_idx];
    if (sym->st_shndx == SHN_ABS) return (u64)sym->st_value;
    if (ELF32_ST_BIND(sym->st_info) != STB_LOCAL &&
        sym->st_shndx == SHN_UNDEF) {
        const char *n = o->strtab + sym->st_name;
        GSym *g = gsym_find(n);
        if (!g || !g->defined) die("undefined symbol: %s", n);
        return g->vaddr;
    }
    u64 va = sym_to_vaddr32(o, sym);
    if (va == (u64)-1) die("unresolvable symbol in %s", o->path);
    return va;
}

static void apply_relas(void) {
    for (int oi = 0; oi < nobj; oi++) {
        Obj *o = &objs[oi];

        if (o->elf_class == ELFCLASS64) {
            for (int si = 0; si < o->shnum; si++) {
                Elf64Shdr *rsh = &o->shdrs64[si];
                if (rsh->sh_type != SHT_RELA) continue;
                int tgt = rsh->sh_info;
                if (tgt >= o->shnum) continue;
                Elf64Shdr  *tsh = &o->shdrs64[tgt];
                const char *tnm = o->shstrtab + tsh->sh_name;
                Sec        *dst = sec_for(tnm);
                if (!dst) continue;
                u64 in_off = o->sec_out_off[tgt];
                if (in_off == (u64)-1) continue;

                Elf64Rela *relas = (Elf64Rela *)(o->data + rsh->sh_offset);
                int nr = (int)(rsh->sh_size / sizeof(Elf64Rela));
                for (int ri = 0; ri < nr; ri++) {
                    Elf64Rela *r  = &relas[ri];
                    u32  type     = ELF64_R_TYPE(r->r_info);
                    u64  S        = resolve64(o, ELF64_R_SYM(r->r_info));
                    i64  A        = r->r_addend;
                    u64  P        = dst->vaddr + in_off + r->r_offset;
                    u8  *loc      = dst->buf   + in_off + r->r_offset;
                    switch (type) {
                    case R_X86_64_NONE: break;
                    case R_X86_64_64:
                        *(u64*)loc = (u64)(S + A); break;
                    case R_X86_64_PC32:
                    case R_X86_64_PLT32: {
                        i64 v = (i64)(S + A - P);
                        if (v < -0x80000000LL || v > 0x7fffffffLL)
                            die("R_X86_64_PC32 overflow");
                        *(u32*)loc = (u32)(i32)v; break;
                    }
                    case R_X86_64_32: {
                        u64 v = S + A;
                        if (v > 0xffffffffULL) die("R_X86_64_32 overflow");
                        *(u32*)loc = (u32)v; break;
                    }
                    case R_X86_64_32S: {
                        i64 v = (i64)(S + A);
                        if (v < -0x80000000LL || v > 0x7fffffffLL)
                            die("R_X86_64_32S overflow");
                        *(u32*)loc = (u32)(i32)v; break;
                    }
                    default:
                        fprintf(stderr, "neutron-ld: warning: unhandled reloc type %u\n", type);
                    }
                }
            }

        } else { /* ELFCLASS32 */
            for (int si = 0; si < o->shnum; si++) {
                Elf32Shdr *rsh = &o->shdrs32[si];
                if (rsh->sh_type != SHT_REL && rsh->sh_type != SHT_RELA) continue;
                int tgt = rsh->sh_info;
                if (tgt >= o->shnum) continue;
                Elf32Shdr  *tsh = &o->shdrs32[tgt];
                const char *tnm = o->shstrtab + tsh->sh_name;
                Sec        *dst = sec_for(tnm);
                if (!dst) continue;
                u64 in_off = o->sec_out_off[tgt];
                if (in_off == (u64)-1) continue;

                if (rsh->sh_type == SHT_REL) {
                    Elf32Rel *rels = (Elf32Rel *)(o->data + rsh->sh_offset);
                    int nr = (int)(rsh->sh_size / sizeof(Elf32Rel));
                    for (int ri = 0; ri < nr; ri++) {
                        Elf32Rel *r = &rels[ri];
                        u32  type   = ELF32_R_TYPE(r->r_info);
                        u64  S      = resolve32(o, ELF32_R_SYM(r->r_info));
                        u8  *loc    = dst->buf + in_off + r->r_offset;
                        u64  P      = dst->vaddr + in_off + r->r_offset;
                        i32  A      = *(i32*)loc;  /* implicit addend in data */
                        switch (type) {
                        case R_386_32:
                            *(u32*)loc = (u32)(S + (u32)A); break;
                        case R_386_PC32:
                            *(u32*)loc = (u32)((S + (u32)A) - P); break;
                        default:
                            fprintf(stderr, "neutron-ld: warning: unhandled i386 reloc type %u\n", type);
                        }
                    }
                } else { /* SHT_RELA */
                    Elf32Rela *relas = (Elf32Rela *)(o->data + rsh->sh_offset);
                    int nr = (int)(rsh->sh_size / sizeof(Elf32Rela));
                    for (int ri = 0; ri < nr; ri++) {
                        Elf32Rela *r = &relas[ri];
                        u32  type    = ELF32_R_TYPE(r->r_info);
                        u64  S       = resolve32(o, ELF32_R_SYM(r->r_info));
                        i32  A       = r->r_addend;
                        u64  P       = dst->vaddr + in_off + r->r_offset;
                        u8  *loc     = dst->buf   + in_off + r->r_offset;
                        switch (type) {
                        case R_386_32:
                            *(u32*)loc = (u32)(S + (u32)A); break;
                        case R_386_PC32:
                            *(u32*)loc = (u32)((S + (u32)A) - P); break;
                        default:
                            fprintf(stderr, "neutron-ld: warning: unhandled i386 reloc type %u\n", type);
                        }
                    }
                }
            }
        }
    }
}

/* ================================================================
 * Pass 6 — write ELF executable (with section headers for objcopy)
 * ================================================================ */

/* Build a .shstrtab blob; return size. offsets[] receives each name's position. */
static int build_shstrtab(char *buf, int has_rodata, int has_data, int has_bss,
                          int *o_null, int *o_text, int *o_rodata,
                          int *o_data,  int *o_bss,  int *o_shstrtab)
{
    int n = 0;
    *o_null     = n; buf[n++] = '\0';
    *o_text     = n; memcpy(buf+n, ".text",     5); n += 5; buf[n++] = '\0';
    *o_rodata   = n; if (has_rodata) { memcpy(buf+n, ".rodata", 7); n += 7; buf[n++] = '\0'; }
    *o_data     = n; if (has_data)   { memcpy(buf+n, ".data",   5); n += 5; buf[n++] = '\0'; }
    *o_bss      = n; if (has_bss)    { memcpy(buf+n, ".bss",    4); n += 4; buf[n++] = '\0'; }
    *o_shstrtab = n; memcpy(buf+n, ".shstrtab", 9); n += 9; buf[n++] = '\0';
    return n;
}

static void write_elf64(const char *path) {
    int has_rodata = (sec_rodata.size > 0);
    int has_data   = (sec_data.size   > 0);
    int has_bss    = (bss_size        > 0);
    int has_rw     = has_data || has_bss;

    u64 rx_end   = has_rodata ? sec_rodata.file_off + sec_rodata.size
                              : sec_text.file_off   + sec_text.size;
    u64 data_end = has_data   ? sec_data.file_off   + sec_data.size : rx_end;

    /* shstrtab + section headers appended after data */
    char shnames[128];
    int o_null, o_text, o_rodata, o_data, o_bss, o_shstrtab;
    int shstr_size = build_shstrtab(shnames, has_rodata, has_data, has_bss,
                                    &o_null, &o_text, &o_rodata,
                                    &o_data, &o_bss,  &o_shstrtab);

    int n_secs = 2 + has_rodata + has_data + has_bss + 1; /* null+text+...+shstrtab */
    u64 shstr_off = ALIGN_UP(data_end, 8);
    u64 shdrs_off = ALIGN_UP(shstr_off + (u64)shstr_size, 8);
    u64 file_sz   = shdrs_off + (u64)n_secs * sizeof(Elf64Shdr);

    u8 *out = calloc(1, file_sz);
    if (!out) die("out of memory");

    /* ELF header */
    Elf64Ehdr *eh = (Elf64Ehdr *)out;
    eh->e_ident[0]=ELFMAG0; eh->e_ident[1]='E'; eh->e_ident[2]='L'; eh->e_ident[3]='F';
    eh->e_ident[4]=ELFCLASS64; eh->e_ident[5]=ELFDATA2LSB; eh->e_ident[6]=EV_CURRENT;
    eh->e_type       = ET_EXEC;
    eh->e_machine    = EM_X86_64;
    eh->e_version    = EV_CURRENT;
    eh->e_phoff      = sizeof(Elf64Ehdr);
    eh->e_ehsize     = sizeof(Elf64Ehdr);
    eh->e_phentsize  = sizeof(Elf64Phdr);
    eh->e_phnum      = has_rw ? 2 : 1;
    eh->e_shoff      = shdrs_off;
    eh->e_shentsize  = sizeof(Elf64Shdr);
    eh->e_shnum      = (u16)n_secs;
    eh->e_shstrndx   = (u16)(n_secs - 1);

    GSym *start = gsym_find(entry_sym);
    if (!start || !start->defined) die("undefined entry symbol: %s", entry_sym);
    eh->e_entry = start->vaddr;

    /* Program headers */
    Elf64Phdr *ph = (Elf64Phdr *)(out + sizeof(Elf64Ehdr));
    ph[0].p_type=PT_LOAD; ph[0].p_flags=PF_R|PF_X; ph[0].p_offset=0;
    ph[0].p_vaddr=sec_text.vaddr-HDR_SIZE; ph[0].p_paddr=ph[0].p_vaddr;
    ph[0].p_filesz=rx_end; ph[0].p_memsz=rx_end; ph[0].p_align=PAGE_SIZE;
    if (has_rw) {
        ph[1].p_type=PT_LOAD; ph[1].p_flags=PF_R|PF_W;
        ph[1].p_offset=sec_data.file_off; ph[1].p_vaddr=sec_data.vaddr;
        ph[1].p_paddr=sec_data.vaddr; ph[1].p_filesz=sec_data.size;
        ph[1].p_memsz=sec_data.size+bss_size; ph[1].p_align=PAGE_SIZE;
    }

    /* Section data */
    if (sec_text.size)   memcpy(out+sec_text.file_off,   sec_text.buf,   sec_text.size);
    if (sec_rodata.size) memcpy(out+sec_rodata.file_off, sec_rodata.buf, sec_rodata.size);
    if (sec_data.size)   memcpy(out+sec_data.file_off,   sec_data.buf,   sec_data.size);
    memcpy(out + shstr_off, shnames, shstr_size);

    /* Section headers */
    Elf64Shdr *sh = (Elf64Shdr *)(out + shdrs_off);
    int si = 0;

    /* SHN_NULL */ memset(&sh[si++], 0, sizeof(Elf64Shdr));

    sh[si].sh_name=o_text; sh[si].sh_type=SHT_PROGBITS;
    sh[si].sh_flags=SHF_ALLOC|SHF_EXECINSTR;
    sh[si].sh_addr=sec_text.vaddr; sh[si].sh_offset=sec_text.file_off;
    sh[si].sh_size=sec_text.size; sh[si].sh_addralign=sec_text.align; si++;

    if (has_rodata) {
        sh[si].sh_name=o_rodata; sh[si].sh_type=SHT_PROGBITS;
        sh[si].sh_flags=SHF_ALLOC;
        sh[si].sh_addr=sec_rodata.vaddr; sh[si].sh_offset=sec_rodata.file_off;
        sh[si].sh_size=sec_rodata.size; sh[si].sh_addralign=sec_rodata.align; si++;
    }
    if (has_data) {
        sh[si].sh_name=o_data; sh[si].sh_type=SHT_PROGBITS;
        sh[si].sh_flags=SHF_ALLOC|SHF_WRITE;
        sh[si].sh_addr=sec_data.vaddr; sh[si].sh_offset=sec_data.file_off;
        sh[si].sh_size=sec_data.size; sh[si].sh_addralign=sec_data.align; si++;
    }
    if (has_bss) {
        sh[si].sh_name=o_bss; sh[si].sh_type=SHT_NOBITS;
        sh[si].sh_flags=SHF_ALLOC|SHF_WRITE;
        sh[si].sh_addr=bss_vaddr; sh[si].sh_offset=data_end;
        sh[si].sh_size=bss_size; sh[si].sh_addralign=(u64)bss_align; si++;
    }
    /* .shstrtab */
    sh[si].sh_name=o_shstrtab; sh[si].sh_type=SHT_STRTAB;
    sh[si].sh_offset=shstr_off; sh[si].sh_size=(u64)shstr_size;
    sh[si].sh_addralign=1;

    FILE *fp = fopen(path, "wb");
    if (!fp) die("cannot open '%s': %s", path, strerror(errno));
    if (fwrite(out, 1, file_sz, fp) != file_sz) die("write error: %s", path);
    fclose(fp); free(out);
    chmod(path, 0755);
}

static void write_elf32(const char *path) {
    int has_rodata = (sec_rodata.size > 0);
    int has_data   = (sec_data.size   > 0);
    int has_bss    = (bss_size        > 0);
    int has_rw     = has_data || has_bss;

    u64 rx_end   = has_rodata ? sec_rodata.file_off + sec_rodata.size
                              : sec_text.file_off   + sec_text.size;
    u64 data_end = has_data   ? sec_data.file_off   + sec_data.size : rx_end;

    char shnames[128];
    int o_null, o_text, o_rodata, o_data, o_bss, o_shstrtab;
    int shstr_size = build_shstrtab(shnames, has_rodata, has_data, has_bss,
                                    &o_null, &o_text, &o_rodata,
                                    &o_data, &o_bss,  &o_shstrtab);

    int n_secs = 2 + has_rodata + has_data + has_bss + 1;
    u64 shstr_off = ALIGN_UP(data_end, 4);
    u64 shdrs_off = ALIGN_UP(shstr_off + (u64)shstr_size, 4);
    u64 file_sz   = shdrs_off + (u64)n_secs * sizeof(Elf32Shdr);

    u8 *out = calloc(1, (size_t)file_sz);
    if (!out) die("out of memory");

    /* ELF header */
    Elf32Ehdr *eh = (Elf32Ehdr *)out;
    eh->e_ident[0]=ELFMAG0; eh->e_ident[1]='E'; eh->e_ident[2]='L'; eh->e_ident[3]='F';
    eh->e_ident[4]=ELFCLASS32; eh->e_ident[5]=ELFDATA2LSB; eh->e_ident[6]=EV_CURRENT;
    eh->e_type       = ET_EXEC;
    eh->e_machine    = EM_386;
    eh->e_version    = EV_CURRENT;
    eh->e_phoff      = sizeof(Elf32Ehdr);
    eh->e_ehsize     = sizeof(Elf32Ehdr);
    eh->e_phentsize  = sizeof(Elf32Phdr);
    eh->e_phnum      = (u16)(has_rw ? 2 : 1);
    eh->e_shoff      = (u32)shdrs_off;
    eh->e_shentsize  = sizeof(Elf32Shdr);
    eh->e_shnum      = (u16)n_secs;
    eh->e_shstrndx   = (u16)(n_secs - 1);

    GSym *start = gsym_find(entry_sym);
    if (!start || !start->defined) die("undefined entry symbol: %s", entry_sym);
    eh->e_entry = (u32)start->vaddr;

    /* Program headers */
    Elf32Phdr *ph = (Elf32Phdr *)(out + sizeof(Elf32Ehdr));
    ph[0].p_type=(u32)PT_LOAD; ph[0].p_flags=PF_R|PF_X; ph[0].p_offset=0;
    ph[0].p_vaddr=(u32)(sec_text.vaddr-HDR_SIZE); ph[0].p_paddr=ph[0].p_vaddr;
    ph[0].p_filesz=(u32)rx_end; ph[0].p_memsz=(u32)rx_end;
    ph[0].p_align=(u32)PAGE_SIZE;
    if (has_rw) {
        ph[1].p_type=(u32)PT_LOAD; ph[1].p_flags=PF_R|PF_W;
        ph[1].p_offset=(u32)sec_data.file_off; ph[1].p_vaddr=(u32)sec_data.vaddr;
        ph[1].p_paddr=(u32)sec_data.vaddr; ph[1].p_filesz=(u32)sec_data.size;
        ph[1].p_memsz=(u32)(sec_data.size+bss_size); ph[1].p_align=(u32)PAGE_SIZE;
    }

    /* Section data */
    if (sec_text.size)   memcpy(out+sec_text.file_off,   sec_text.buf,   sec_text.size);
    if (sec_rodata.size) memcpy(out+sec_rodata.file_off, sec_rodata.buf, sec_rodata.size);
    if (sec_data.size)   memcpy(out+sec_data.file_off,   sec_data.buf,   sec_data.size);
    memcpy(out + shstr_off, shnames, shstr_size);

    /* Section headers */
    Elf32Shdr *sh = (Elf32Shdr *)(out + shdrs_off);
    int si = 0;

    /* SHN_NULL */ memset(&sh[si++], 0, sizeof(Elf32Shdr));

    sh[si].sh_name=(u32)o_text; sh[si].sh_type=SHT_PROGBITS;
    sh[si].sh_flags=SHF_ALLOC|SHF_EXECINSTR;
    sh[si].sh_addr=(u32)sec_text.vaddr; sh[si].sh_offset=(u32)sec_text.file_off;
    sh[si].sh_size=(u32)sec_text.size; sh[si].sh_addralign=(u32)sec_text.align; si++;

    if (has_rodata) {
        sh[si].sh_name=(u32)o_rodata; sh[si].sh_type=SHT_PROGBITS;
        sh[si].sh_flags=SHF_ALLOC;
        sh[si].sh_addr=(u32)sec_rodata.vaddr; sh[si].sh_offset=(u32)sec_rodata.file_off;
        sh[si].sh_size=(u32)sec_rodata.size; sh[si].sh_addralign=(u32)sec_rodata.align; si++;
    }
    if (has_data) {
        sh[si].sh_name=(u32)o_data; sh[si].sh_type=SHT_PROGBITS;
        sh[si].sh_flags=SHF_ALLOC|SHF_WRITE;
        sh[si].sh_addr=(u32)sec_data.vaddr; sh[si].sh_offset=(u32)sec_data.file_off;
        sh[si].sh_size=(u32)sec_data.size; sh[si].sh_addralign=(u32)sec_data.align; si++;
    }
    if (has_bss) {
        sh[si].sh_name=(u32)o_bss; sh[si].sh_type=SHT_NOBITS;
        sh[si].sh_flags=SHF_ALLOC|SHF_WRITE;
        sh[si].sh_addr=(u32)bss_vaddr; sh[si].sh_offset=(u32)data_end;
        sh[si].sh_size=(u32)bss_size; sh[si].sh_addralign=(u32)bss_align; si++;
    }
    /* .shstrtab */
    sh[si].sh_name=(u32)o_shstrtab; sh[si].sh_type=SHT_STRTAB;
    sh[si].sh_offset=(u32)shstr_off; sh[si].sh_size=(u32)shstr_size;
    sh[si].sh_addralign=1;

    FILE *fp = fopen(path, "wb");
    if (!fp) die("cannot open '%s': %s", path, strerror(errno));
    if (fwrite(out, 1, (size_t)file_sz, fp) != (size_t)file_sz)
        die("write error: %s", path);
    fclose(fp); free(out);
    chmod(path, 0755);
}

/* ================================================================
 * main
 * ================================================================ */

int linker_main(int argc, char **argv) {
    const char *output    = "a.out";
    const char *ld_script = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) die("missing argument to -o");
            output = argv[i];
        } else if (!strcmp(argv[i], "-m")) {
            if (++i >= argc) die("missing argument to -m");
            if (!strcmp(argv[i], "elf_i386") || !strcmp(argv[i], "elf32"))
                link_32bit = 1;
            /* elf_x86_64 / other: keep 64-bit default */
        } else if (!strncmp(argv[i], "-m", 2)) {
            /* -melf_i386 compact form */
            const char *em = argv[i] + 2;
            if (!strcmp(em, "elf_i386") || !strcmp(em, "elf32"))
                link_32bit = 1;
        } else if (!strcmp(argv[i], "-T")) {
            if (++i >= argc) die("missing argument to -T");
            ld_script = argv[i];
        } else if (!strncmp(argv[i], "-T", 2) && argv[i][2]) {
            ld_script = argv[i] + 2;
        } else if (!strcmp(argv[i], "-e")) {
            if (++i >= argc) die("missing argument to -e");
            strncpy(entry_sym, argv[i], sizeof entry_sym - 1);
        } else if (argv[i][0] != '-') {
            load_obj(argv[i]);
        }
        /* other flags (--build-id, -z, etc.) silently ignored */
    }

    if (ld_script) parse_linker_script(ld_script);

    if (nobj == 0) die("no input files");

    merge_sections();
    layout();
    build_symtab();
    apply_relas();

    if (link_32bit)
        write_elf32(output);
    else
        write_elf64(output);

    GSym *entry = gsym_find(entry_sym);
    printf("neutron-ld: linked %s  (entry %s @ 0x%llx)\n",
           output, entry_sym,
           entry ? (unsigned long long)entry->vaddr : 0ULL);
    return 0;
}
