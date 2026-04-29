# Neutron Toolchain — Makefile
#
# Binaries produced:
#   build/bin/neutron           — unified driver (compile + assemble + link)
#   build/bin/neutron-compiler  — compiler only  (neutron brain)
#   build/bin/neutron-ld        — linker only
#
# Usage:
#   make                        — build everything
#   make run FILE=path/to/file.c
#   make test
#   make clean

CC      := gcc
CFLAGS  := -std=c99 -Wall -Wextra -g -O0 \
            -Wno-unused-parameter \
            -Wno-unused-function  \
            -D_GNU_SOURCE

AS      := ./tools/proton
ASFLAGS := -f elf64

BUILD   := build
BINDIR  := $(BUILD)/bin
OBJDIR  := $(BUILD)/obj
BOOTDIR := $(BUILD)/boot

# ----------------------------------------------------------------
# Source groups
# ----------------------------------------------------------------

# Core compiler modules (no main)
COMPILER_SRCS := neutron.c          \
                 src/preprocess.c   \
                 src/lex.c          \
                 src/parse.c        \
                 src/sema.c         \
                 src/codegen.c

# ----------------------------------------------------------------
# Targets
# ----------------------------------------------------------------

NEUTRON    := $(BINDIR)/neutron
NEUTRON_CC := $(BINDIR)/neutron-compiler
NEUTRON_LD := $(BINDIR)/neutron-ld

BOOT_OBJS  := $(BOOTDIR)/crt0.o     \
              $(BOOTDIR)/crti.o     \
              $(BOOTDIR)/crtn.o     \
              $(BOOTDIR)/printf.o   \
              $(BOOTDIR)/crt0_32.o  \
              $(BOOTDIR)/printf_32.o

all: $(NEUTRON) $(NEUTRON_CC) $(NEUTRON_LD) $(BOOT_OBJS)

# ---- neutron (unified driver) ----
# compiler modules + linker + driver (driver.c provides main)
NEUTRON_SRCS := $(COMPILER_SRCS) src/linker.c src/driver.c
NEUTRON_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(NEUTRON_SRCS))

$(NEUTRON): $(NEUTRON_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^
	@echo "  LD  $@"

# ---- neutron-compiler (compiler brain only) ----
# compiler modules + stub main
NEUTRON_CC_SRCS := $(COMPILER_SRCS) src/compiler_stub.c
NEUTRON_CC_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(NEUTRON_CC_SRCS))

$(NEUTRON_CC): $(NEUTRON_CC_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^
	@echo "  LD  $@"

# ---- neutron-ld (linker only) ----
NEUTRON_LD_SRCS := src/linker.c src/linker_stub.c
NEUTRON_LD_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(NEUTRON_LD_SRCS))

$(NEUTRON_LD): $(NEUTRON_LD_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^
	@echo "  LD  $@"

# ---- Generic compile rule ----
$(OBJDIR)/%.o: %.c neutron.h | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
	@echo "  CC  $<"

# src/linker.c and src/driver.c don't depend on neutron.h strictly,
# but the rule above covers them via the wildcard; linker.c has its
# own includes so this is fine.

# ---- Boot assembly objects ----
$(BOOTDIR)/crt0.o: boot/crt0.asm | $(BOOTDIR)
	$(AS) $(ASFLAGS) boot/crt0.asm -o $@
	@echo "  AS  boot/crt0.asm"

$(BOOTDIR)/crti.o: boot/crti.asm | $(BOOTDIR)
	$(AS) $(ASFLAGS) boot/crti.asm -o $@
	@echo "  AS  boot/crti.asm"

$(BOOTDIR)/crtn.o: boot/crtn.asm | $(BOOTDIR)
	$(AS) $(ASFLAGS) boot/crtn.asm -o $@
	@echo "  AS  boot/crtn.asm"

$(BOOTDIR)/printf.o: boot/printf.c | $(BOOTDIR)
	$(CC) -std=c99 -nostdlib -fno-builtin -Os -c -o $@ $<
	@echo "  CC  boot/printf.c"

$(BOOTDIR)/crt0_32.o: boot/crt0_32.asm | $(BOOTDIR)
	nasm -f elf32 boot/crt0_32.asm -o $@
	@echo "  AS  boot/crt0_32.asm"

GCC_INCLUDE := $(shell gcc -print-file-name=include)

$(BOOTDIR)/printf_32.o: boot/printf_32.c | $(BOOTDIR)
	$(CC) -std=c99 -m32 -nostdlib -nostdinc -I$(GCC_INCLUDE) -fno-builtin -Os -c -o $@ $<
	@echo "  CC  boot/printf_32.c"

# ---- Directory creation ----
$(BINDIR):
	mkdir -p $(BINDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BOOTDIR):
	mkdir -p $(BOOTDIR)

# ----------------------------------------------------------------
# Test suite
# ----------------------------------------------------------------
TEST_DIR := tests
TESTS    := $(wildcard $(TEST_DIR)/*.c)

.PHONY: test
test: $(NEUTRON) $(BOOT_OBJS)
	@echo "=== Running tests ==="
	@pass=0; fail=0; \
	for f in $(TESTS); do \
	    name=$$(basename $$f .c); \
	    $(NEUTRON) $$f -o $(BUILD)/$${name} 2>/dev/null; \
	    expected_file=$(TEST_DIR)/$${name}.expected; \
	    if [ -f "$$expected_file" ]; then \
	        actual=$$($(BUILD)/$${name} 2>&1); \
	        expected=$$(cat $$expected_file); \
	        if [ "$$actual" = "$$expected" ]; then \
	            echo "  PASS  $$name"; pass=$$((pass+1)); \
	        else \
	            echo "  FAIL  $$name"; \
	            echo "    expected: $$expected"; \
	            echo "    got:      $$actual"; \
	            fail=$$((fail+1)); \
	        fi; \
	    else \
	        echo "  BUILD $$name (no .expected file)"; \
	        pass=$$((pass+1)); \
	    fi; \
	done; \
	echo "=== $$pass passed, $$fail failed ==="

# ----------------------------------------------------------------
# Convenience targets
# ----------------------------------------------------------------
.PHONY: run
run: $(NEUTRON) $(BOOT_OBJS)
	$(NEUTRON) $(FILE) -o $(BUILD)/out
	$(BUILD)/out

.PHONY: tokens
tokens: $(NEUTRON_CC)
	$(NEUTRON_CC) -dump-tokens $(FILE)

.PHONY: ast
ast: $(NEUTRON_CC)
	$(NEUTRON_CC) -dump-ast $(FILE)

.PHONY: preprocess
preprocess: $(NEUTRON_CC)
	$(NEUTRON_CC) -E $(FILE)

# ----------------------------------------------------------------
clean:
	rm -rf $(BUILD)

.PHONY: all clean test run tokens ast preprocess
