# ============================================================================
# X OS - root build system
#   make setup     fetch Limine bootloader + bitmap font (run once, needs net)
#   make           build the bootable ISO (x-os.iso)
#   make run       boot in QEMU via legacy BIOS (SeaBIOS)
#   make run-uefi  boot in QEMU via UEFI (OVMF/edk2)
#   make clean     remove build artifacts
#   make distclean also remove fetched Limine + font
# ============================================================================

ISO          := x-os.iso
BUILD_DIR    := build
OBJ_DIR      := $(BUILD_DIR)/obj
ISO_ROOT     := $(BUILD_DIR)/iso_root
KERNEL       := $(BUILD_DIR)/x-os.elf

LIMINE_DIR   := boot/uefi/limine
LIMINE       := $(LIMINE_DIR)/limine

# Apple clang can emit x86_64 ELF; ld.lld comes from the brew 'lld' formula.
CC           := clang
LD           := $(shell brew --prefix lld 2>/dev/null)/bin/ld.lld
ifeq ($(wildcard $(LD)),)
LD           := ld.lld
endif
QEMU_VIRGL   := $(shell brew --prefix qemu-virgl 2>/dev/null)/bin/qemu-system-x86_64
QEMU         := $(QEMU_VIRGL)
ifeq ($(wildcard $(QEMU)),)
QEMU         := /opt/qemu-head/bin/qemu-system-x86_64
endif
ifeq ($(wildcard $(QEMU)),)
  QEMU       := $(shell brew --prefix qemu 2>/dev/null)/bin/qemu-system-x86_64
endif
ifeq ($(wildcard $(QEMU)),)
  QEMU       := qemu-system-x86_64
endif
OVMF         := $(shell brew --prefix qemu 2>/dev/null)/share/qemu/edk2-x86_64-code.fd

SRC_DIRS     := boot kernel userspace
# Exclude Limine, ring-3 userspace sources, generated blobs, and bsd/ from normal kernel build.
# bsd/ files are handled separately with FreeBSD compat flags.
# kernel/bsd/syscalls.c is also handled separately (needs BSD include paths).
CFILES       := $(shell find . -type f -name '*.c' -not -path '*/limine/*' -not -path './userspace/*' -not -path './build-qemu/*' -not -name '*_blob.c' -not -path './bsd/*' -not -path './kernel/bsd/*' 2>/dev/null)
SFILES       := $(shell find . -type f -name '*.S' -not -path '*/limine/*' -not -path './userspace/*' -not -path './build-qemu/*' -not -name '*_blob.S' 2>/dev/null)

# FreeBSD network stack source files
BSD_CFILES   := $(shell find bsd -type f -name '*.c' -not -path 'bsd/compat/*' 2>/dev/null)
BSD_COMPAT_CFILES := bsd/compat/compat_shims.c bsd/compat/atomic_stubs.c

OBJS         := $(patsubst %.c,$(OBJ_DIR)/%.o,$(CFILES)) $(patsubst %.S,$(OBJ_DIR)/%.o,$(SFILES)) \
               $(patsubst %.c,$(OBJ_DIR)/%.o,$(BSD_CFILES)) \
               $(patsubst %.c,$(OBJ_DIR)/%.o,$(BSD_COMPAT_CFILES)) \
               $(OBJ_DIR)/kernel/bsd/syscalls.o
DEPS         := $(patsubst %.c,$(OBJ_DIR)/%.d,$(CFILES)) $(patsubst %.c,$(OBJ_DIR)/%.d,$(BSD_CFILES)) $(patsubst %.c,$(OBJ_DIR)/%.d,$(BSD_COMPAT_CFILES)) $(OBJ_DIR)/kernel/bsd/syscalls.d

CFLAGS := \
  --target=x86_64-unknown-none-elf \
  -ffreestanding -fno-stack-protector -fno-stack-check \
  -fno-pic -fno-pie -m64 -march=x86-64 \
  -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
  -O2 -pipe -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -I. -I$(LIMINE_DIR) \
  -MMD -MP

# CFLAGS for FreeBSD network stack files - adds _KERNEL and compat include paths
BSD_CFLAGS := \
  --target=x86_64-unknown-none-elf \
  -ffreestanding -fno-stack-protector -fno-stack-check \
  -fno-pic -fno-pie -m64 -march=x86-64 \
  -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
  -O2 -pipe -std=gnu11 -Wno-all \
  -D_KERNEL -DTCP_RFC7413 -DTCP_RFC7413_MAX_KEYS=10 -DTCP_RFC7413_MAX_PSKS=10 -DTCP_BLACKBOX -DSTATS \
  -I. -Ibsd/compat -Ibsd \
  -MMD -MP

LDFLAGS := \
  -nostdlib -static -no-pie -z max-page-size=0x1000 \
  -m elf_x86_64 -T kernel/linker.ld

# ---- userspace init ELF --------------------------------------------------
USERSPACE_CFLAGS := \
  --target=x86_64-unknown-none-elf \
  -ffreestanding -fno-stack-protector -fno-stack-check \
  -fno-pic -fno-pie -m64 -march=x86-64 -mno-red-zone \
  -O2 -pipe -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memmove \
  -I. -I$(LIMINE_DIR)

USERSPACE_LDFLAGS := \
  -nostdlib -static -no-pie -z max-page-size=0x1000 \
  -m elf_x86_64 -T userspace/init/init.ld

INIT_ELF    := $(BUILD_DIR)/userspace/init/init.elf
INIT_BLOB_C := kernel/proc/init_elf_blob.c
INIT_BLOB_O := $(OBJ_DIR)/kernel/proc/init_elf_blob.o

COMPOSER_ELF   := $(BUILD_DIR)/userspace/services/composer/composer.elf
COMPOSER_BLOB_C := kernel/proc/composer_elf_blob.c
COMPOSER_BLOB_O := $(OBJ_DIR)/kernel/proc/composer_elf_blob.o

ZSH_ELF        := $(BUILD_DIR)/userspace/shell/zsh.elf
ZSH_BLOB_C     := kernel/proc/zsh_elf_blob.c
ZSH_BLOB_O     := $(OBJ_DIR)/kernel/proc/zsh_elf_blob.o

MENUBAR_ELF    := $(BUILD_DIR)/userspace/services/menubar/menubar.elf
MENUBAR_BLOB_C := kernel/proc/menubar_elf_blob.c
MENUBAR_BLOB_O := $(OBJ_DIR)/kernel/proc/menubar_elf_blob.o
MENUBAR_SVG_H  := $(BUILD_DIR)/userspace/services/menubar/svg_data.h

DOCK_ELF       := $(BUILD_DIR)/userspace/services/dock/dock.elf
DOCK_BLOB_C    := kernel/proc/dock_elf_blob.c
DOCK_BLOB_O    := $(OBJ_DIR)/kernel/proc/dock_elf_blob.o
DOCK_SVG_H     := $(BUILD_DIR)/userspace/services/dock/svg_data.h

CMDS_ELF       := $(BUILD_DIR)/userspace/cmds/cmds.elf
CMDS_BLOB_C    := kernel/proc/cmds_elf_blob.c
CMDS_BLOB_O    := $(OBJ_DIR)/kernel/proc/cmds_elf_blob.o

# Context menu service
MENU_ELF       := $(BUILD_DIR)/userspace/services/menu/menu.elf
MENU_BLOB_C    := kernel/proc/menu_elf_blob.c
MENU_BLOB_O    := $(OBJ_DIR)/kernel/proc/menu_elf_blob.o
# Menu is now built with Rust + egui (no SVG)
MENU_RUST_LIB  := $(BUILD_DIR)/userspace/services/menu/libxos_context_menu.a

# Terminal app (egui on xos_egui_platform)
TERMINAL_ELF      := $(BUILD_DIR)/userspace/apps/terminal/terminal.elf
TERMINAL_BLOB_C   := kernel/proc/terminal_elf_blob.c
TERMINAL_BLOB_O   := $(OBJ_DIR)/kernel/proc/terminal_elf_blob.o
TERMINAL_RUST_LIB := $(BUILD_DIR)/userspace/apps/terminal/libxos_terminal.a

# GPU rendering backend + platform layer for egui apps
EGUI_VIRGL_LIB   := $(BUILD_DIR)/third_party/egui_virgl_backend/libegui_virgl_backend.a
EGUI_PLATFORM_LIB := $(BUILD_DIR)/userspace/lib/egui_platform/libxos_egui_platform.rlib

# Add generated blob objects explicitly to kernel link
OBJS += $(INIT_BLOB_O) $(COMPOSER_BLOB_O) $(ZSH_BLOB_O) $(MENUBAR_BLOB_O) $(DOCK_BLOB_O) $(CMDS_BLOB_O) $(MENU_BLOB_O) $(TERMINAL_BLOB_O)

# ---- newlib paths --------------------------------------------------------
NEWLIB_PREFIX  := /opt/x-os-newlib/x86_64-elf
NEWLIB_CFLAGS  := -I$(NEWLIB_PREFIX)/include
NEWLIB_LIBS    := $(NEWLIB_PREFIX)/lib/libc.a $(NEWLIB_PREFIX)/lib/libm.a

# ---- libc++ static library ------------------------------------------------
LIBCXX_CONFIG     := userspace/lib/libcxx_config
# Vendored persistently under third_party/ (NOT /tmp) so the source survives
# reboots/`/tmp` cleanup. Fetched on demand by the $(LIBCXX_SRC_MARKER) rule.
LIBCXX_TAG        := llvmorg-22.1.8
LIBCXX_VENDOR_DIR := third_party/llvm-project-libcxx
LIBCXX_SRC        := $(LIBCXX_VENDOR_DIR)/libcxx/src
LIBCXX_SRC_MARKER := $(LIBCXX_VENDOR_DIR)/.fetched
LLVM_CXX      := /opt/homebrew/opt/llvm/bin/clang++
LLVM_AR       := /opt/homebrew/opt/llvm/bin/llvm-ar

# ---- zlib static library ---------------------------------------------------
ZLIB_DIR      := userspace/lib/zlib
ZLIB_SRCS     := adler32.c compress.c crc32.c deflate.c infback.c inffast.c \
                 inflate.c inftrees.c trees.c uncompr.c zutil.c \
                 gzclose.c gzlib.c gzread.c gzwrite.c
ZLIB_OBJS     := $(patsubst %.c,$(BUILD_DIR)/zlib/%.o,$(ZLIB_SRCS))
ZLIB_A        := $(BUILD_DIR)/zlib/libz.a

# ---- ThorVG static library -------------------------------------------------
THORVG_DIR    := userspace/lib/thorvg
THORVG_A      := $(BUILD_DIR)/thorvg/thorvg.a

LIBCXX_SRCS   := string.cpp new.cpp memory.cpp verbose_abort.cpp exception.cpp stdexcept.cpp functional.cpp hash.cpp algorithm.cpp bind.cpp error_category.cpp system_error.cpp call_once.cpp typeinfo.cpp new_handler.cpp new_helpers.cpp
LIBCXX_OBJS   := $(patsubst %.cpp,$(BUILD_DIR)/libcxx/%.o,$(LIBCXX_SRCS))
LIBCXX_A      := $(BUILD_DIR)/libcxx/libc++.a

LIBCXX_CXXFLAGS := \
  --target=x86_64-unknown-none-elf -nostdlib -nostdinc++ \
  -I$(LIBCXX_CONFIG) -I/opt/homebrew/opt/llvm/include/c++/v1 \
  -I$(LIBCXX_SRC) \
  -isystem $(NEWLIB_PREFIX)/include \
  -fno-exceptions -fno-rtti -fno-threadsafe-statics -O2 -std=c++20 \
  -D_LIBCPP_BUILDING_LIBRARY

# CFLAGS for newlib-linked userspace programs
LIBC_CFLAGS := \
  --target=x86_64-unknown-none-elf \
  -ffreestanding -fno-stack-protector -fno-stack-check \
  -fno-pic -fno-pie -m64 -march=x86-64 -mno-red-zone \
  -mno-sse -mno-sse2 -mno-mmx \
  -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memmove \
  -O2 -pipe -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -I. -I$(LIMINE_DIR) $(NEWLIB_CFLAGS) \
  -D_POSIX_TIMERS=1 -D_POSIX_MONOTONIC_CLOCK=1

LIBC_LDFLAGS := \
  -nostdlib -static -no-pie -z max-page-size=0x1000 \
  -m elf_x86_64 -T userspace/libc/xos-libc.ld

# newlib test program
TEST_LIBC_ELF := $(BUILD_DIR)/userspace/libc/test_libc.elf

QEMU_BASE  := -M q35 -m 512M -smp 1 -no-reboot -rtc base=localtime -name "X OS" -vga none -device virtio-gpu-gl-pci,max_outputs=1,xres=2560,yres=1600 -display cocoa,show-cursor=off,gl=es

.PHONY: all run run-uefi clean distclean setup limine cmds thorvg zlib libcxx-src

all: $(ISO)

# ---- compile -------------------------------------------------------------
$(OBJ_DIR)/kernel/lib/string.o: kernel/lib/string.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memmove -c $< -o $@

# FreeBSD network stack files use BSD_CFLAGS
$(OBJ_DIR)/bsd/%.o: bsd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BSD_CFLAGS) -c $< -o $@

# syscalls.c bridges X OS and FreeBSD — needs _KERNEL but NOT -Ibsd (avoids header conflicts)
$(OBJ_DIR)/kernel/bsd/syscalls.o: kernel/bsd/syscalls.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -D_KERNEL -Wno-incompatible-library-redeclaration -c $< -o $@

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Build userspace init ELF (start.S + init.c + syscall wrappers)
$(INIT_ELF): userspace/init/start.S userspace/init/init.c userspace/runtime/syscall.c userspace/init/init.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/init/start.S -o $(BUILD_DIR)/userspace/init/start.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/init/init.c -o $(BUILD_DIR)/userspace/init/init.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/init/syscall.o
	$(LD) $(USERSPACE_LDFLAGS) \
	  $(BUILD_DIR)/userspace/init/start.o \
	  $(BUILD_DIR)/userspace/init/init.o \
	  $(BUILD_DIR)/userspace/init/syscall.o \
	  -o $@
	@echo ">> linked $@"

# Generate C blob from ELF binary so the kernel can embed it
$(INIT_BLOB_C): $(INIT_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t init_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *init_elf_data = init_elf_bytes;', 'size_t init_elf_len = sizeof(init_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

# Build composer service ELF
$(COMPOSER_ELF): userspace/services/composer/start.S userspace/services/composer/main.c userspace/services/composer/gpu_composite.c userspace/services/composer/gpu_composite.h userspace/services/composer/cursor_data.c userspace/lib/xgfx/xgfx.c userspace/lib/xgfx/xgfx_font_terminus.c userspace/lib/wm/wm.h userspace/runtime/syscall.c userspace/services/composer/composer.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/composer/start.S -o $(BUILD_DIR)/userspace/services/composer/start.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/composer/main.c -o $(BUILD_DIR)/userspace/services/composer/main.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/composer/gpu_composite.c -o $(BUILD_DIR)/userspace/services/composer/gpu_composite.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/lib/xgfx/xgfx.c -o $(BUILD_DIR)/userspace/services/composer/xgfx.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/lib/xgfx/xgfx_font_terminus.c -o $(BUILD_DIR)/userspace/services/composer/xgfx_font.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/services/composer/syscall.o
	$(LD) -nostdlib -static -no-pie -z max-page-size=0x1000 -m elf_x86_64 -T userspace/services/composer/composer.ld \
	  $(BUILD_DIR)/userspace/services/composer/start.o \
	  $(BUILD_DIR)/userspace/services/composer/main.o \
	  $(BUILD_DIR)/userspace/services/composer/gpu_composite.o \
	  $(BUILD_DIR)/userspace/services/composer/xgfx.o \
	  $(BUILD_DIR)/userspace/services/composer/xgfx_font.o \
	  $(BUILD_DIR)/userspace/services/composer/syscall.o \
	  -o $@
	@echo ">> linked $@"

$(COMPOSER_BLOB_C): $(COMPOSER_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t composer_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *composer_elf_data = composer_elf_bytes;', 'size_t composer_elf_len = sizeof(composer_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

# Explicit blob object rules so Make knows to generate the .c first
$(INIT_BLOB_O): $(INIT_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMPOSER_BLOB_O): $(COMPOSER_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ZSH_BLOB_C): $(ZSH_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t zsh_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *zsh_elf_data = zsh_elf_bytes;', 'size_t zsh_elf_len = sizeof(zsh_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

$(ZSH_BLOB_O): $(ZSH_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- newlib-linked userspace programs ------------------------------------
# These link against newlib libc.a + our syscall stubs instead of raw syscalls
$(TEST_LIBC_ELF): userspace/libc/test_libc.c userspace/libc/syscalls.c userspace/runtime/syscall.c userspace/libc/xos-libc.ld
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -c userspace/libc/test_libc.c -o $(BUILD_DIR)/userspace/libc/test_libc.o
	$(CC) $(LIBC_CFLAGS) -c userspace/libc/syscalls.c -o $(BUILD_DIR)/userspace/libc/syscalls.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/libc/xos_syscall.o
	$(LD) $(LIBC_LDFLAGS) \
	  $(BUILD_DIR)/userspace/libc/test_libc.o \
	  $(BUILD_DIR)/userspace/libc/syscalls.o \
	  $(BUILD_DIR)/userspace/libc/xos_syscall.o \
	  $(NEWLIB_LIBS) \
	  -o $@
	@echo ">> linked $@"

test-libc: $(TEST_LIBC_ELF)

# ---- zsh shell (Apple OSS zsh-118 / zsh 5.9 + X OS bridge) -----------------
ZSH_SRC := $(CURDIR)/userspace/shell/zsh-src
ZSH_STUBS := $(BUILD_DIR)/userspace/shell/stubs
ZSH_CURSES_A := $(ZSH_STUBS)/libcurses.a

zsh: $(ZSH_ELF)

$(ZSH_CURSES_A): userspace/shell/curses_stubs.c
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -c userspace/shell/curses_stubs.c -o $(BUILD_DIR)/userspace/shell/curses_stubs.o
	/opt/homebrew/opt/llvm/bin/llvm-ar rcs $@ $(BUILD_DIR)/userspace/shell/curses_stubs.o

$(ZSH_ELF): userspace/shell/zsh_start.S userspace/shell/zsh_entry.c userspace/libc/syscalls.c userspace/runtime/syscall.c userspace/libc/xos-libc.ld $(ZSH_CURSES_A)
	@mkdir -p $(dir $@)
	@# Prebuilt Src/*.o only — never let Make's implicit %.o: %.c rule rebuild them.
	@test -f $(ZSH_SRC)/Src/init.o -a -f $(ZSH_SRC)/Src/builtin.o \
	  -a -f $(ZSH_SRC)/Src/exec.o -a -f $(ZSH_SRC)/Src/hashtable.o \
	  -a -f $(ZSH_SRC)/Src/jobs.o -a -f $(ZSH_SRC)/Src/signals.o \
	  || { echo "error: missing zsh Src/*.o — restore from userspace/shell/zsh-obj-backup/"; exit 1; }
	@echo ">> Building zsh (Apple zsh-118 lineage → zsh_main)..."
	$(CC) $(USERSPACE_CFLAGS) -c userspace/shell/zsh_start.S -o $(BUILD_DIR)/userspace/shell/zsh_start.o
	$(CC) $(LIBC_CFLAGS) -c userspace/shell/zsh_entry.c -o $(BUILD_DIR)/userspace/shell/zsh_entry.o
	$(CC) $(LIBC_CFLAGS) -c userspace/libc/syscalls.c -o $(BUILD_DIR)/userspace/shell/zsh_syscalls.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/shell/zsh_xos_syscall.o
	cd $(ZSH_SRC)/Src && \
	  $(LD) -nostdlib -static -m elf_x86_64 -T $(CURDIR)/userspace/libc/xos-libc.ld \
	    $(CURDIR)/$(BUILD_DIR)/userspace/shell/zsh_start.o \
	    $(CURDIR)/$(BUILD_DIR)/userspace/shell/zsh_entry.o \
	    main.o $$(cat stamp-modobjs) \
	    $(CURDIR)/$(ZSH_CURSES_A) \
	    $(CURDIR)/$(BUILD_DIR)/userspace/shell/zsh_syscalls.o \
	    $(CURDIR)/$(BUILD_DIR)/userspace/shell/zsh_xos_syscall.o \
	    $(NEWLIB_LIBS) \
	    -o $(CURDIR)/$(ZSH_ELF)
	/opt/homebrew/opt/llvm/bin/llvm-strip $(ZSH_ELF)
	@echo ">> linked $@"

# Build menubar service: generate svg_data.h from SVG, compile ELF
$(MENUBAR_SVG_H): userspace/services/menubar/menubar.svg scripts/svg_to_header.py
	@mkdir -p $(dir $@)
	python3 scripts/svg_to_header.py userspace/services/menubar/menubar.svg $(MENUBAR_SVG_H)
	@echo ">> generated $@"

$(MENUBAR_ELF): userspace/services/menubar/start.S userspace/services/menubar/main.c $(MENUBAR_SVG_H) $(THORVG_A) $(LIBCXX_A) userspace/lib/wm/wm.h userspace/runtime/syscall.c userspace/libc/syscalls.c userspace/services/menubar/menubar.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/menubar/start.S -o $(BUILD_DIR)/userspace/services/menubar/start.o
	$(CC) $(LIBC_CFLAGS) -msse -msse2 -I$(BUILD_DIR)/userspace/services/menubar -Iuserspace/lib/thorvg -Iuserspace/lib/wm -c userspace/services/menubar/main.c -o $(BUILD_DIR)/userspace/services/menubar/main.o
	$(CC) $(LIBC_CFLAGS) -c userspace/libc/syscalls.c -o $(BUILD_DIR)/userspace/services/menubar/menubar_syscalls.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/services/menubar/syscall.o
	$(LD) -nostdlib -static -no-pie -z max-page-size=0x1000 -m elf_x86_64 -T userspace/services/menubar/menubar.ld \
	  $(BUILD_DIR)/userspace/services/menubar/start.o \
	  $(BUILD_DIR)/userspace/services/menubar/main.o \
	  $(BUILD_DIR)/userspace/services/menubar/menubar_syscalls.o \
	  $(BUILD_DIR)/userspace/services/menubar/syscall.o \
	  $(THORVG_A) $(LIBCXX_A) $(NEWLIB_LIBS) \
	  -o $@
	@/opt/homebrew/opt/llvm/bin/llvm-strip $@ 2>/dev/null || true
	@echo ">> linked $@"

$(MENUBAR_BLOB_C): $(MENUBAR_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t menubar_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *menubar_elf_data = menubar_elf_bytes;', 'size_t menubar_elf_len = sizeof(menubar_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

$(MENUBAR_BLOB_O): $(MENUBAR_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- dock service (SVG-based, ThorVG) -------------------------------------
dock: $(DOCK_ELF)

$(DOCK_SVG_H): userspace/services/dock/dock.svg scripts/svg_to_header.py
	@mkdir -p $(dir $@)
	python3 scripts/svg_to_header.py userspace/services/dock/dock.svg $(DOCK_SVG_H)
	@echo ">> generated $@"

$(DOCK_ELF): userspace/services/dock/start.S userspace/services/dock/main.c $(DOCK_SVG_H) $(THORVG_A) $(LIBCXX_A) userspace/lib/wm/wm.h userspace/runtime/syscall.c userspace/libc/syscalls.c userspace/services/dock/dock.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/dock/start.S -o $(BUILD_DIR)/userspace/services/dock/start.o
	$(CC) $(LIBC_CFLAGS) -msse -msse2 -I$(BUILD_DIR)/userspace/services/dock -Iuserspace/lib/thorvg -Iuserspace/lib/wm -c userspace/services/dock/main.c -o $(BUILD_DIR)/userspace/services/dock/main.o
	$(CC) $(LIBC_CFLAGS) -c userspace/libc/syscalls.c -o $(BUILD_DIR)/userspace/services/dock/dock_syscalls.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/services/dock/syscall.o
	$(LD) -nostdlib -static -no-pie -z max-page-size=0x1000 -m elf_x86_64 -T userspace/services/dock/dock.ld \
	  $(BUILD_DIR)/userspace/services/dock/start.o \
	  $(BUILD_DIR)/userspace/services/dock/main.o \
	  $(BUILD_DIR)/userspace/services/dock/dock_syscalls.o \
	  $(BUILD_DIR)/userspace/services/dock/syscall.o \
	  $(THORVG_A) $(LIBCXX_A) $(NEWLIB_LIBS) \
	  -o $@
	@/opt/homebrew/opt/llvm/bin/llvm-strip $@ 2>/dev/null || true
	@echo ">> linked $@"

$(DOCK_BLOB_C): $(DOCK_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t dock_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *dock_elf_data = dock_elf_bytes;', 'size_t dock_elf_len = sizeof(dock_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

$(DOCK_BLOB_O): $(DOCK_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- shell commands (multi-call binary, busybox-style) -------------------
cmds: $(CMDS_ELF)

$(CMDS_ELF): userspace/cmds/cmds_start.S userspace/cmds/cmds_main.c userspace/libc/syscalls.c userspace/runtime/syscall.c userspace/cmds/cmds.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/cmds/cmds_start.S -o $(BUILD_DIR)/userspace/cmds/cmds_start.o
	$(CC) $(LIBC_CFLAGS) -msse -msse2 -c userspace/cmds/cmds_main.c -o $(BUILD_DIR)/userspace/cmds/cmds_main.o
	$(CC) $(LIBC_CFLAGS) -c userspace/libc/syscalls.c -o $(BUILD_DIR)/userspace/cmds/cmds_syscalls.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/cmds/cmds_xos_syscall.o
	$(LD) -nostdlib -static -no-pie -z max-page-size=0x1000 -m elf_x86_64 -T userspace/cmds/cmds.ld \
	  $(BUILD_DIR)/userspace/cmds/cmds_start.o \
	  $(BUILD_DIR)/userspace/cmds/cmds_main.o \
	  $(BUILD_DIR)/userspace/cmds/cmds_syscalls.o \
	  $(BUILD_DIR)/userspace/cmds/cmds_xos_syscall.o \
	  $(NEWLIB_LIBS) \
	  -o $@
	@/opt/homebrew/opt/llvm/bin/llvm-strip $@ 2>/dev/null || true
	@echo ">> linked $@"

$(CMDS_BLOB_C): $(CMDS_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t cmds_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *cmds_elf_data = cmds_elf_bytes;', 'size_t cmds_elf_len = sizeof(cmds_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

$(CMDS_BLOB_O): $(CMDS_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- libc++ static library ------------------------------------------------
# Fetch just the libcxx/ subtree of llvm-project at LIBCXX_TAG via a sparse,
# blobless, depth-1 clone (avoids pulling the ~1GB+ monorepo). Vendored under
# third_party/ instead of /tmp so it survives reboots.
libcxx-src: $(LIBCXX_SRC_MARKER)
$(LIBCXX_SRC_MARKER):
	@echo ">> fetching llvm-project libcxx source ($(LIBCXX_TAG))"
	rm -rf $(LIBCXX_VENDOR_DIR)
	git clone --depth=1 --filter=blob:none --sparse --branch=$(LIBCXX_TAG) \
	  https://github.com/llvm/llvm-project.git $(LIBCXX_VENDOR_DIR)
	cd $(LIBCXX_VENDOR_DIR) && git sparse-checkout set libcxx
	touch $@
	@echo ">> fetched $(LIBCXX_VENDOR_DIR)"

$(BUILD_DIR)/libcxx/%.o: $(LIBCXX_SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(LLVM_CXX) $(LIBCXX_CXXFLAGS) -c $< -o $@

# $(LIBCXX_SRC_MARKER) is listed first so the source is fetched (and its
# .cpp files exist on disk) before make tries to apply the pattern rule above.
$(LIBCXX_A): $(LIBCXX_SRC_MARKER) $(LIBCXX_OBJS)
	$(LLVM_AR) rcs $@ $(LIBCXX_OBJS)
	@echo ">> built $@"

# ---- ThorVG static library -------------------------------------------------
thorvg: $(THORVG_A)

$(THORVG_A): scripts/build_thorvg.sh $(wildcard $(THORVG_DIR)/src/**/*.cpp) $(wildcard $(THORVG_DIR)/src/*.cpp) $(THORVG_DIR)/thorvg_xos.cpp $(THORVG_DIR)/thorvg_config.h $(THORVG_DIR)/src/common/config.h $(THORVG_DIR)/src/common/tvgLock.h
	@mkdir -p $(BUILD_DIR)/thorvg
	bash scripts/build_thorvg.sh

# ---- zlib static library ---------------------------------------------------
zlib: $(ZLIB_A)

$(BUILD_DIR)/zlib/%.o: $(ZLIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -DHAVE_HIDDEN -include unistd.h -I$(ZLIB_DIR) -c $< -o $@

$(ZLIB_A): $(ZLIB_OBJS)
	$(LLVM_AR) rcs $@ $(ZLIB_OBJS)
	@echo ">> built $@"

# ---- context menu service (Rust + egui) -----------------------------------
menu: $(MENU_ELF)

# Build the Rust staticlib for the context menu (GPU backend)
$(MENU_RUST_LIB): userspace/services/menu/Cargo.toml userspace/services/menu/src/lib.rs $(EGUI_VIRGL_LIB)
	@mkdir -p $(dir $@)
	cd userspace/services/menu && CARGO_TARGET_DIR="$(CURDIR)/userspace/services/menu/target" cargo build --release --target x86_64-unknown-none
	cp userspace/services/menu/target/x86_64-unknown-none/release/libxos_context_menu.a $@
	@echo ">> built $@"

$(MENU_ELF): userspace/services/menu/shim.c userspace/services/menu/start.S $(MENU_RUST_LIB) $(EGUI_VIRGL_LIB) userspace/lib/wm/wm.h userspace/runtime/syscall.c userspace/libc/syscalls.c userspace/services/menu/menu.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/menu/start.S -o $(BUILD_DIR)/userspace/services/menu/start.o
	$(CC) $(LIBC_CFLAGS) -msse -msse2 -Iuserspace/lib/wm -c userspace/services/menu/shim.c -o $(BUILD_DIR)/userspace/services/menu/shim.o
	$(CC) $(LIBC_CFLAGS) -c userspace/libc/syscalls.c -o $(BUILD_DIR)/userspace/services/menu/menu_syscalls.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/services/menu/menu_xos_syscall.o
	$(LD) -nostdlib -static -no-pie -z max-page-size=0x1000 -m elf_x86_64 -T userspace/services/menu/menu.ld \
	  $(BUILD_DIR)/userspace/services/menu/start.o \
	  $(BUILD_DIR)/userspace/services/menu/shim.o \
	  $(BUILD_DIR)/userspace/services/menu/menu_syscalls.o \
	  $(BUILD_DIR)/userspace/services/menu/menu_xos_syscall.o \
	  $(MENU_RUST_LIB) $(EGUI_VIRGL_LIB) $(NEWLIB_LIBS) \
	  -o $@
	@/opt/homebrew/opt/llvm/bin/llvm-strip $@ 2>/dev/null || true
	@echo ">> linked $@"

$(MENU_BLOB_C): $(MENU_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t menu_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *menu_elf_data = menu_elf_bytes;', 'size_t menu_elf_len = sizeof(menu_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

$(MENU_BLOB_O): $(MENU_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- egui GPU backend + platform layer (Rust) ----------------------------
egui-gpu: $(EGUI_VIRGL_LIB)

$(EGUI_VIRGL_LIB): third_party/egui_virgl_backend/Cargo.toml third_party/egui_virgl_backend/src/lib.rs third_party/egui_virgl_backend/src/runtime.rs third_party/egui_software_backend/src/lib.rs
	@mkdir -p $(dir $@)
	# Force a local target dir — some environments redirect CARGO_TARGET_DIR
	# to a cache, which left make linking a stale .a (old VirGL draw path).
	cd third_party/egui_virgl_backend && CARGO_TARGET_DIR="$(CURDIR)/third_party/egui_virgl_backend/target" cargo build --release --target x86_64-unknown-none
	cp third_party/egui_virgl_backend/target/x86_64-unknown-none/release/libegui_virgl_backend.a $@
	@echo ">> built $@"

# ---- terminal app (Rust + egui_platform) ---------------------------------
terminal: $(TERMINAL_ELF)

$(TERMINAL_RUST_LIB): userspace/apps/terminal/Cargo.toml userspace/apps/terminal/src/lib.rs userspace/lib/egui_platform/src/lib.rs $(EGUI_VIRGL_LIB)
	@mkdir -p $(dir $@)
	cd userspace/apps/terminal && CARGO_TARGET_DIR="$(CURDIR)/userspace/apps/terminal/target" cargo build --release --target x86_64-unknown-none
	cp userspace/apps/terminal/target/x86_64-unknown-none/release/libxos_terminal.a $@
	@echo ">> built $@"

$(TERMINAL_ELF): userspace/apps/terminal/shim.c userspace/apps/terminal/start.S $(TERMINAL_RUST_LIB) $(EGUI_VIRGL_LIB) userspace/lib/wm/wm.h userspace/runtime/syscall.c userspace/libc/syscalls.c userspace/apps/terminal/terminal.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/apps/terminal/start.S -o $(BUILD_DIR)/userspace/apps/terminal/start.o
	$(CC) $(LIBC_CFLAGS) -msse -msse2 -Iuserspace/lib/wm -Iuserspace/apps/terminal -c userspace/apps/terminal/shim.c -o $(BUILD_DIR)/userspace/apps/terminal/shim.o
	$(CC) $(LIBC_CFLAGS) -c userspace/libc/syscalls.c -o $(BUILD_DIR)/userspace/apps/terminal/term_syscalls.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/apps/terminal/term_xos_syscall.o
	$(LD) -nostdlib -static -no-pie -z max-page-size=0x1000 -m elf_x86_64 -T userspace/apps/terminal/terminal.ld \
	  $(BUILD_DIR)/userspace/apps/terminal/start.o \
	  $(BUILD_DIR)/userspace/apps/terminal/shim.o \
	  $(BUILD_DIR)/userspace/apps/terminal/term_syscalls.o \
	  $(BUILD_DIR)/userspace/apps/terminal/term_xos_syscall.o \
	  $(TERMINAL_RUST_LIB) $(EGUI_VIRGL_LIB) $(NEWLIB_LIBS) \
	  -o $@
	@/opt/homebrew/opt/llvm/bin/llvm-strip $@ 2>/dev/null || true
	@echo ">> linked $@"

$(TERMINAL_BLOB_C): $(TERMINAL_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t terminal_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *terminal_elf_data = terminal_elf_bytes;', 'size_t terminal_elf_len = sizeof(terminal_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

$(TERMINAL_BLOB_O): $(TERMINAL_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- link ----------------------------------------------------------------
$(KERNEL): $(OBJS) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	@echo ">> linked $@"

# ---- bootable hybrid (BIOS+UEFI) ISO ------------------------------------
$(ISO): $(KERNEL) boot/uefi/limine.conf $(LIMINE)
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL) $(ISO_ROOT)/boot/x-os.elf
	cp boot/uefi/limine.conf $(ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys       $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin    $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin    $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI           $(ISO_ROOT)/EFI/BOOT/
	-cp $(LIMINE_DIR)/BOOTIA32.EFI         $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs \
	  -b boot/limine/limine-bios-cd.bin \
	  -no-emul-boot -boot-load-size 4 -boot-info-table \
	  --efi-boot boot/limine/limine-uefi-cd.bin \
	  -efi-boot-part --efi-boot-image --protective-msdos-label \
	  $(ISO_ROOT) -o $(ISO)
	$(LIMINE) bios-install $(ISO)
	@echo ">> built $(ISO)"

# ---- run -----------------------------------------------------------------
DISK_IMG    := disk.img
NVME_DRIVE  := -drive file=$(DISK_IMG),if=none,id=nvme0,format=raw
NVME_DEV    := -device nvme,drive=nvme0,serial=deadbeef

run: $(ISO) $(DISK_IMG)
	$(QEMU) $(QEMU_BASE) -serial stdio -cdrom $(ISO) -boot d $(NVME_DRIVE) $(NVME_DEV) \
	  -netdev user,id=net0 -device virtio-net-pci,netdev=net0

run-uefi: $(ISO) $(DISK_IMG)
	$(QEMU) $(QEMU_BASE) -serial stdio \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	  -cdrom $(ISO) -boot d $(NVME_DRIVE) $(NVME_DEV) \
	  -netdev user,id=net0 -device virtio-net-pci,netdev=net0

$(DISK_IMG):
	@echo ">> creating 4 MiB disk image"
	@dd if=/dev/zero of=$@ bs=1M count=4 status=none

# ---- one-time setup ------------------------------------------------------
setup: limine libcxx-src

limine: $(LIMINE)
$(LIMINE):
	@echo ">> fetching Limine bootloader (v8.x-binary)"
	rm -rf $(LIMINE_DIR)
	git clone https://github.com/limine-bootloader/limine.git \
	  --branch=v8.x-binary --depth=1 $(LIMINE_DIR)
	$(MAKE) -C $(LIMINE_DIR)

clean:
	rm -rf $(BUILD_DIR) $(ISO)

distclean: clean
	rm -rf $(LIMINE_DIR) $(LIBCXX_VENDOR_DIR)

-include $(DEPS)
