/*
 * linker.c — Neutron ELF64 static linker
 *
 * Usage: neutron-ld [-o output] file1.o [file2.o ...]
 *
 * Merges ELF64 relocatable objects into a static x86-64 Linux executable.
 * No dynamic linking, no PLT/GOT, no shared libraries.
 *
 * Supported relocations:
 *   R_X86_64_64    absolute 64-bit
 *   R_X86_64_PC32  PC-relative 32-bit  (call/jmp)
 *   R_X86_64_PLT32 same as PC32 for static
 *   R_X86_64_32    absolute 32-bit zero-extended
 *   R_X86_64_32S   absolute 32-bit sign-extended
 *
 * Output layout (all mapped from LOAD_BASE = 0x400000):
 *   file 0x0000 : ELF header + program headers
 *   file 0x1000 : .text  (merged, exec+read)
 *   file 0x1000+align : .rodata (read-only)
 *   file page-align  : .data  (read-write)
 *   (virtual only)   : .bss   (zero-init, not in file)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>

/* ================================================================
 * ELF64 types
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

/* ELF constants */
#define ELFMAG0 0x7f
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define EM_X86_64   62
#define EV_CURRENT  1
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8
#define PT_LOAD      1
#define PF_X  0x1
#define PF_W  0x2
#define PF_R  0x4
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2
#define SHN_UNDEF  0
#define SHN_ABS    0xfff1
#define R_X86_64_NONE  0
#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4
#define R_X86_64_32    10
#define R_X86_64_32S   11
#define ELF64_R_SYM(i)   ((u32)((i) >> 32))
#define ELF64_R_TYPE(i)  ((u32)((i) & 0xffffffff))
#define ELF64_ST_BIND(i) ((i) >> 4)

/* ================================================================
 * Layout constants
 * ================================================================ */

#define LOAD_BASE  ((u64)0x400000)
#define PAGE_SIZE  ((u64)0x1000)
#define HDR_SIZE   ((u64)0x1000)   /* reserved at file start for ELF header */
#define ALIGN_UP(x,a) (((x)+((a)-1))&~((a)-1))

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
    u64  vaddr, file_off;   /* set during layout */
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

/* Which merged section a named input section belongs to */
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
    Elf64Ehdr  *ehdr;
    Elf64Shdr  *shdrs;
    const char *shstrtab;
    Elf64Sym   *syms;
    int         nsyms;
    const char *strtab;
    u64        *sec_out_off;  /* per-section offset in merged Sec; -1 = not merged */
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

    if (o->size < sizeof(Elf64Ehdr)) die("%s: too small to be ELF", path);
    o->ehdr = (Elf64Ehdr *)o->data;
    if (o->ehdr->e_ident[0] != ELFMAG0 || o->ehdr->e_ident[1] != 'E' ||
        o->ehdr->e_ident[2] != 'L'     || o->ehdr->e_ident[3] != 'F')
        die("%s: not an ELF file", path);
    if (o->ehdr->e_ident[4] != ELFCLASS64)  die("%s: not ELF64", path);
    if (o->ehdr->e_machine   != EM_X86_64)  die("%s: not x86-64", path);

    o->shdrs     = (Elf64Shdr *)(o->data + o->ehdr->e_shoff);
    o->shstrtab  = (const char *)(o->data +
                    o->shdrs[o->ehdr->e_shstrndx].sh_offset);

    o->sec_out_off = malloc(o->ehdr->e_shnum * sizeof(u64));
    for (int i = 0; i < o->ehdr->e_shnum; i++) o->sec_out_off[i] = (u64)-1;

    for (int i = 0; i < o->ehdr->e_shnum; i++) {
        Elf64Shdr *sh = &o->shdrs[i];
        if (sh->sh_type == SHT_SYMTAB) {
            o->syms   = (Elf64Sym *)(o->data + sh->sh_offset);
            o->nsyms  = (int)(sh->sh_size / sizeof(Elf64Sym));
            o->strtab = (const char *)(o->data +
                         o->shdrs[sh->sh_link].sh_offset);
            break;
        }
    }
}

/* ================================================================
 * Pass 2 — merge sections
 * ================================================================ */

static void merge_sections(void) {
    for (int oi = 0; oi < nobj; oi++) {
        Obj *o = &objs[oi];
        for (int si = 0; si < o->ehdr->e_shnum; si++) {
            Elf64Shdr  *sh = &o->shdrs[si];
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

/* ================================================================
 * Pass 3 — layout: assign virtual addresses
 * ================================================================ */

static void layout(void) {
    sec_text.file_off = HDR_SIZE;
    sec_text.vaddr    = LOAD_BASE + HDR_SIZE;

    u64 ra = sec_rodata.align > 1 ? sec_rodata.align : 16;
    sec_rodata.file_off = ALIGN_UP(sec_text.file_off + sec_text.size, ra);
    sec_rodata.vaddr    = LOAD_BASE + sec_rodata.file_off;

    u64 rx_end = sec_rodata.file_off + sec_rodata.size;
    sec_data.file_off = ALIGN_UP(rx_end, PAGE_SIZE);
    sec_data.vaddr    = LOAD_BASE + sec_data.file_off;

    u64 ba = bss_align > 1 ? bss_align : 8;
    bss_vaddr = ALIGN_UP(sec_data.vaddr + sec_data.size, ba);
}

/* ================================================================
 * Pass 4 — resolve symbol virtual addresses
 * ================================================================ */

static u64 sym_to_vaddr(Obj *o, Elf64Sym *sym) {
    u16 idx = sym->st_shndx;
    if (idx == SHN_ABS)   return sym->st_value;
    if (idx == SHN_UNDEF || idx >= o->ehdr->e_shnum) return (u64)-1;

    Elf64Shdr  *sh = &o->shdrs[idx];
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
        if (!o->syms) continue;
        for (int si = 0; si < o->nsyms; si++) {
            Elf64Sym *sym = &o->syms[si];
            int bind = ELF64_ST_BIND(sym->st_info);
            if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
            if (sym->st_shndx == SHN_UNDEF) continue;
            const char *name = o->strtab + sym->st_name;
            if (!name[0]) continue;
            u64 va = sym_to_vaddr(o, sym);
            if (va == (u64)-1) continue;
            GSym *g = gsym_get(name);
            if (!g->defined) { g->vaddr = va; g->defined = 1; }
        }
    }
}

/* ================================================================
 * Pass 5 — apply relocations
 * ================================================================ */

static u64 resolve(Obj *o, u32 sym_idx) {
    if (!o->syms || (int)sym_idx >= o->nsyms) die("bad symbol index");
    Elf64Sym *sym  = &o->syms[sym_idx];
    int       bind = ELF64_ST_BIND(sym->st_info);

    if (sym->st_shndx == SHN_ABS) return sym->st_value;

    if ((bind == STB_GLOBAL || bind == STB_WEAK) &&
         sym->st_shndx == SHN_UNDEF) {
        const char *n = o->strtab + sym->st_name;
        GSym *g = gsym_find(n);
        if (!g || !g->defined) die("undefined symbol: %s", n);
        return g->vaddr;
    }
    u64 va = sym_to_vaddr(o, sym);
    if (va == (u64)-1) die("unresolvable symbol in %s", o->path);
    return va;
}

static void apply_relas(void) {
    for (int oi = 0; oi < nobj; oi++) {
        Obj *o = &objs[oi];
        for (int si = 0; si < o->ehdr->e_shnum; si++) {
            Elf64Shdr *rsh = &o->shdrs[si];
            if (rsh->sh_type != SHT_RELA) continue;
            int tgt = rsh->sh_info;
            if (tgt >= o->ehdr->e_shnum) continue;

            Elf64Shdr  *tsh  = &o->shdrs[tgt];
            const char *tnm  = o->shstrtab + tsh->sh_name;
            Sec        *dst  = sec_for(tnm);
            if (!dst) continue;
            u64 in_off = o->sec_out_off[tgt];
            if (in_off == (u64)-1) continue;

            Elf64Rela *relas = (Elf64Rela *)(o->data + rsh->sh_offset);
            int nr = (int)(rsh->sh_size / sizeof(Elf64Rela));

            for (int ri = 0; ri < nr; ri++) {
                Elf64Rela *r   = &relas[ri];
                u32  type      = ELF64_R_TYPE(r->r_info);
                u32  sym_idx   = ELF64_R_SYM(r->r_info);
                u64  S         = resolve(o, sym_idx);
                i64  A         = r->r_addend;
                u64  P         = dst->vaddr + in_off + r->r_offset;
                u8  *loc       = dst->buf   + in_off + r->r_offset;

                switch (type) {
                case R_X86_64_NONE: break;
                case R_X86_64_64:
                    *(u64 *)loc = (u64)(S + A);
                    break;
                case R_X86_64_PC32:
                case R_X86_64_PLT32: {
                    i64 v = (i64)(S + A - P);
                    if (v < -0x80000000LL || v > 0x7fffffffLL)
                        die("R_X86_64_PC32 overflow");
                    *(u32 *)loc = (u32)(i32)v;
                    break;
                }
                case R_X86_64_32: {
                    u64 v = S + A;
                    if (v > 0xffffffffULL) die("R_X86_64_32 overflow");
                    *(u32 *)loc = (u32)v;
                    break;
                }
                case R_X86_64_32S: {
                    i64 v = (i64)(S + A);
                    if (v < -0x80000000LL || v > 0x7fffffffLL)
                        die("R_X86_64_32S overflow");
                    *(u32 *)loc = (u32)(i32)v;
                    break;
                }
                default:
                    fprintf(stderr, "neutron-ld: warning: unhandled reloc type %u\n", type);
                    break;
                }
            }
        }
    }
}

/* ================================================================
 * Pass 6 — write ELF64 executable
 * ================================================================ */

static void write_elf(const char *path) {
    int has_rw = (sec_data.size > 0 || bss_size > 0);

    u64 rx_end  = (sec_rodata.size > 0)
                  ? sec_rodata.file_off + sec_rodata.size
                  : sec_text.file_off + sec_text.size;
    u64 file_sz = has_rw ? sec_data.file_off + sec_data.size : rx_end;

    u8 *out = calloc(1, file_sz);
    if (!out) die("out of memory");

    /* ELF header */
    Elf64Ehdr *eh = (Elf64Ehdr *)out;
    eh->e_ident[0] = ELFMAG0;  eh->e_ident[1] = 'E';
    eh->e_ident[2] = 'L';      eh->e_ident[3] = 'F';
    eh->e_ident[4] = ELFCLASS64;
    eh->e_ident[5] = ELFDATA2LSB;
    eh->e_ident[6] = EV_CURRENT;
    eh->e_type      = ET_EXEC;
    eh->e_machine   = EM_X86_64;
    eh->e_version   = EV_CURRENT;
    eh->e_phoff     = sizeof(Elf64Ehdr);
    eh->e_ehsize    = sizeof(Elf64Ehdr);
    eh->e_phentsize = sizeof(Elf64Phdr);
    eh->e_phnum     = has_rw ? 2 : 1;

    GSym *start = gsym_find("_start");
    if (!start || !start->defined) die("undefined symbol: _start");
    eh->e_entry = start->vaddr;

    /* Program headers */
    Elf64Phdr *ph = (Elf64Phdr *)(out + sizeof(Elf64Ehdr));
    ph[0].p_type   = PT_LOAD;
    ph[0].p_flags  = PF_R | PF_X;
    ph[0].p_offset = 0;
    ph[0].p_vaddr  = LOAD_BASE;
    ph[0].p_paddr  = LOAD_BASE;
    ph[0].p_filesz = rx_end;
    ph[0].p_memsz  = rx_end;
    ph[0].p_align  = PAGE_SIZE;

    if (has_rw) {
        ph[1].p_type   = PT_LOAD;
        ph[1].p_flags  = PF_R | PF_W;
        ph[1].p_offset = sec_data.file_off;
        ph[1].p_vaddr  = sec_data.vaddr;
        ph[1].p_paddr  = sec_data.vaddr;
        ph[1].p_filesz = sec_data.size;
        ph[1].p_memsz  = sec_data.size + bss_size;
        ph[1].p_align  = PAGE_SIZE;
    }

    /* Section data */
    if (sec_text.size)
        memcpy(out + sec_text.file_off,   sec_text.buf,   sec_text.size);
    if (sec_rodata.size)
        memcpy(out + sec_rodata.file_off, sec_rodata.buf, sec_rodata.size);
    if (sec_data.size)
        memcpy(out + sec_data.file_off,   sec_data.buf,   sec_data.size);

    FILE *f = fopen(path, "wb");
    if (!f) die("cannot open '%s': %s", path, strerror(errno));
    if (fwrite(out, 1, file_sz, f) != file_sz) die("write error: %s", path);
    fclose(f);
    free(out);
    chmod(path, 0755);
}

/* ================================================================
 * main
 * ================================================================ */

int linker_main(int argc, char **argv) {
    const char *output = "a.out";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) die("missing argument to -o");
            output = argv[i];
        } else if (argv[i][0] != '-') {
            load_obj(argv[i]);
        }
        /* unknown flags silently ignored */
    }

    if (nobj == 0) die("no input files");

    merge_sections();
    layout();
    build_symtab();
    apply_relas();
    write_elf(output);

    printf("neutron-ld: linked %s  (entry 0x%llx)\n",
           output, (unsigned long long)gsym_find("_start")->vaddr);
    return 0;
}
