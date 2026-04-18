/*
 * driver.c — neutron unified driver
 *
 * Orchestrates the full pipeline:
 *   .c  →(compiler)→  .asm  →(proton)→  .o  →(linker)→  executable
 *
 * Usage: neutron [options] input.c [-o output]
 *   -o <file>        output file (default: a.out)
 *   -S               stop after compilation (emit .asm)
 *   -c               stop after assembly    (emit .o)
 *   -E               preprocess only
 *   -I <path>        add include path
 *   -dump-tokens     print token stream and exit
 *   -dump-ast        print AST and exit
 *
 * Proton and boot objects are located relative to this binary:
 *   <bindir>/../../tools/proton
 *   <bindir>/../boot/crt0.o  etc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../neutron.h"

/* ----------------------------------------------------------------
 * Locate the directory containing this executable (Linux only).
 * ---------------------------------------------------------------- */
static void self_dir(char *buf, size_t n) {
    ssize_t len = readlink("/proc/self/exe", buf, n - 1);
    if (len > 0) {
        buf[len] = '\0';
        char *sl = strrchr(buf, '/');
        if (sl) sl[1] = '\0';
    } else {
        strncpy(buf, "./", n);
    }
}

/* ----------------------------------------------------------------
 * Spawn a child process and wait for it.
 * ---------------------------------------------------------------- */
static int spawn(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) { perror("neutron: fork"); return 1; }
    if (pid == 0) {
        execv(argv[0], argv);
        /* argv[0] failed — try searching PATH */
        execvp(argv[1] ? argv[1] : argv[0], argv + 1);
        fprintf(stderr, "neutron: cannot exec %s\n", argv[0]);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* ----------------------------------------------------------------
 * main
 * ---------------------------------------------------------------- */
int main(int argc, char **argv) {
    char bindir[512];
    self_dir(bindir, sizeof bindir);

    /* Paths derived from binary location */
    char proton[512], boot_dir[512];
    snprintf(proton,   sizeof proton,   "%s../../tools/proton", bindir);
    snprintf(boot_dir, sizeof boot_dir, "%s../boot",            bindir);

    /* ---- Parse arguments ---- */
    const char *input  = NULL;
    const char *output = NULL;
    int flag_S = 0, flag_c = 0;
    int flag_early = 0;   /* -E / -dump-tokens / -dump-ast: stop after compiler */

    /* args forwarded to compiler_main */
    char *cc[64];
    int   ncc = 0;
    cc[ncc++] = "neutron-compiler";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) { fprintf(stderr, "neutron: -o requires argument\n"); return 1; }
            output = argv[i];
        } else if (!strcmp(argv[i], "-S")) {
            flag_S = 1;
        } else if (!strcmp(argv[i], "-c")) {
            flag_c = 1;
        } else if (!strcmp(argv[i], "-E") ||
                   !strcmp(argv[i], "-dump-tokens") ||
                   !strcmp(argv[i], "-dump-ast")) {
            flag_early = 1;
            cc[ncc++] = argv[i];
        } else if (!strcmp(argv[i], "-I")) {
            cc[ncc++] = argv[i];
            if (++i < argc) cc[ncc++] = argv[i];
        } else if (!strncmp(argv[i], "-I", 2)) {
            cc[ncc++] = argv[i];
        } else if (argv[i][0] != '-') {
            input = argv[i];
        } else {
            fprintf(stderr, "neutron: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!input) {
        fprintf(stderr,
            "Usage: neutron [options] input.c [-o output]\n"
            "  -S           stop after compilation (.asm)\n"
            "  -c           stop after assembly    (.o)\n"
            "  -E           preprocess only\n"
            "  -o <file>    output filename\n"
            "  -I <path>    add include path\n");
        return 1;
    }
    if (!output) output = "a.out";

    /* ---- Temp file names ---- */
    char asm_tmp[64], obj_tmp[64];
    int  pid = (int)getpid();
    snprintf(asm_tmp, sizeof asm_tmp, "/tmp/neutron_%d.asm", pid);
    snprintf(obj_tmp, sizeof obj_tmp, "/tmp/neutron_%d.o",   pid);

    /* ================================================================
     * Stage 1 — Compile  .c → .asm
     * ================================================================ */
    const char *asm_out = flag_S ? output : asm_tmp;

    cc[ncc++] = (char*)input;
    cc[ncc++] = "-o";
    cc[ncc++] = (char*)asm_out;
    cc[ncc]   = NULL;

    int ret = compiler_main(ncc, cc);
    if (ret != 0 || flag_early || flag_S) return ret;

    /* ================================================================
     * Stage 2 — Assemble  .asm → .o   (via Proton subprocess)
     * ================================================================ */
    const char *obj_out = flag_c ? output : obj_tmp;

    char *as_args[] = { proton, (char*)"-f", (char*)"elf64",
                        (char*)asm_out, (char*)"-o", (char*)obj_out, NULL };
    ret = spawn(as_args);
    unlink(asm_out);
    if (ret != 0) { fprintf(stderr, "neutron: assembler failed\n"); return ret; }
    if (flag_c)   return 0;

    /* ================================================================
     * Stage 3 — Link   .o → executable   (neutron-ld logic, in-process)
     * ================================================================ */
    char crt0[512], crti[512], crtn[512], prtf[512];
    snprintf(crt0, sizeof crt0, "%s/crt0.o",   boot_dir);
    snprintf(crti, sizeof crti, "%s/crti.o",   boot_dir);
    snprintf(crtn, sizeof crtn, "%s/crtn.o",   boot_dir);
    snprintf(prtf, sizeof prtf, "%s/printf.o", boot_dir);

    char *ld_args[] = { (char*)"neutron-ld", (char*)"-o", (char*)output,
                        crt0, crti, crtn, prtf, (char*)obj_out, NULL };
    ret = linker_main(8, ld_args);
    unlink(obj_out);
    return ret;
}
