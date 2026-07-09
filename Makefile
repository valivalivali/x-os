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
# Exclude Limine, ring-3 userspace sources, generated blobs, and raw XNU BSD
# files from kernel build. Include only x-os adapted files (*_xos.c, bsd_net_init.c).
CFILES       := $(shell find . -type f -name '*.c' -not -path '*/limine/*' -not -path './userspace/*' -not -path './build-qemu/*' -not -name '*_blob.c' -not -path './bsd/kern/kern_*.c' -not -path './bsd/kern/sys_*.c' -not -path './bsd/kern/uipc_domain.c' -not -path './bsd/kern/uipc_socket.c' -not -path './bsd/kern/uipc_socket2.c' -not -path './bsd/kern/uipc_proto.c' -not -path './bsd/kern/uipc_syscalls.c' -not -path './bsd/kern/uipc_usrreq.c' -not -path './bsd/kern/uipc_mbuf.c' -not -path './bsd/kern/uipc_mbuf2.c' -not -path './bsd/kern/uipc_mbuf_mcache.c' -not -path './bsd/kern/mcache.c' -not -path './bsd/kern/kpi_*.c' -not -path './bsd/kern/mach_*.c' -not -path './bsd/kern/bsd_*.c' -not -path './bsd/kern/subr_*.c' -not -path './bsd/kern/tty*.c' -not -path './bsd/kern/proc_info.c' -not -path './bsd/kern/posix_*.c' -not -path './bsd/kern/sysv_*.c' -not -path './bsd/kern/stackshot.c' -not -path './bsd/kern/tracker.c' -not -path './bsd/kern/vsock_domain.c' -not -path './bsd/kern/ubc_subr.c' -not -path './bsd/kern/imageboot.c' -not -path './bsd/kern/chunklist.c' -not -path './bsd/kern/decmpfs.c' -not -path './bsd/kern/hvg_sysctl.c' -not -path './bsd/kern/kdebug*.c' -not -path './bsd/kern/mem_acct.c' -not -path './bsd/kern/netboot.c' -not -path './bsd/kern/policy_check.c' -not -path './bsd/kern/process_policy.c' -not -path './bsd/kern/proc_uuid_policy.c' -not -path './bsd/kern/socket_flows.c' -not -path './bsd/kern/socket_info.c' -not -path './bsd/kern/sys_coalition.c' -not -path './bsd/kern/sys_domain.c' -not -path './bsd/kern/sys_ecc.c' -not -path './bsd/kern/sys_eventlink.c' -not -path './bsd/kern/sys_persona.c' -not -path './bsd/kern/sys_reason.c' -not -path './bsd/kern/sys_record_event.c' -not -path './bsd/kern/sys_recount.c' -not -path './bsd/kern/sys_ulock.c' -not -path './bsd/kern/sys_work_interval.c' -not -path './bsd/kern/qsort.c' \( -not -path './bsd/net/*' -o -name 'net_xos.c' \) -not -path './bsd/netinet/*' -not -path './bsd/vfs/*' -not -path './bsd/pthread/*' -not -path './bsd/libkern/*' -not -path './bsd/compat/*' 2>/dev/null)
SFILES       := $(shell find . -type f -name '*.S' -not -path '*/limine/*' -not -path './userspace/*' -not -path './build-qemu/*' -not -name '*_blob.S' 2>/dev/null)
OBJS         := $(patsubst %.c,$(OBJ_DIR)/%.o,$(CFILES)) $(patsubst %.S,$(OBJ_DIR)/%.o,$(SFILES))
DEPS         := $(patsubst %.c,$(OBJ_DIR)/%.d,$(CFILES))

CFLAGS := \
  --target=x86_64-unknown-none-elf \
  -ffreestanding -fno-stack-protector -fno-stack-check \
  -fno-pic -fno-pie -m64 -march=x86-64 \
  -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
  -O2 -pipe -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -I. -I$(LIMINE_DIR) -Ibsd \
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

XPLORER_ELF    := $(BUILD_DIR)/userspace/services/xplorer/xplorer.elf
XPLORER_BLOB_C := kernel/proc/xplorer_elf_blob.c
XPLORER_BLOB_O := $(OBJ_DIR)/kernel/proc/xplorer_elf_blob.o

DOCK_ELF       := $(BUILD_DIR)/userspace/services/dock/dock.elf
DOCK_BLOB_C    := kernel/proc/dock_elf_blob.c
DOCK_BLOB_O    := $(OBJ_DIR)/kernel/proc/dock_elf_blob.o

MENUBAR_ELF    := $(BUILD_DIR)/userspace/services/menubar/menubar.elf
MENUBAR_BLOB_C := kernel/proc/menubar_elf_blob.c
MENUBAR_BLOB_O := $(OBJ_DIR)/kernel/proc/menubar_elf_blob.o

ZSH_ELF        := $(BUILD_DIR)/userspace/shell/zsh.elf
ZSH_BLOB_C     := kernel/proc/zsh_elf_blob.c
ZSH_BLOB_O     := $(OBJ_DIR)/kernel/proc/zsh_elf_blob.o

TERMINAL_ELF    := $(BUILD_DIR)/userspace/services/terminal/terminal.elf
TERMINAL_BLOB_C := kernel/proc/terminal_elf_blob.c
TERMINAL_BLOB_O := $(OBJ_DIR)/kernel/proc/terminal_elf_blob.o

SVGVIEW_ELF     := $(BUILD_DIR)/userspace/services/svgview/svgview.elf
SVGVIEW_BLOB_C  := kernel/proc/svgview_elf_blob.c
SVGVIEW_BLOB_O  := $(OBJ_DIR)/kernel/proc/svgview_elf_blob.o

# Add generated blob objects explicitly to kernel link
OBJS += $(INIT_BLOB_O) $(COMPOSER_BLOB_O) $(XPLORER_BLOB_O) $(DOCK_BLOB_O) $(MENUBAR_BLOB_O) $(ZSH_BLOB_O) $(TERMINAL_BLOB_O) $(SVGVIEW_BLOB_O)

# ---- newlib paths --------------------------------------------------------
NEWLIB_PREFIX  := /opt/x-os-newlib/x86_64-elf
NEWLIB_CFLAGS  := -I$(NEWLIB_PREFIX)/include
NEWLIB_LIBS    := $(NEWLIB_PREFIX)/lib/libc.a $(NEWLIB_PREFIX)/lib/libm.a

# CFLAGS for newlib-linked userspace programs
LIBC_CFLAGS := \
  --target=x86_64-unknown-none-elf \
  -ffreestanding -fno-stack-protector -fno-stack-check \
  -fno-pic -fno-pie -m64 -march=x86-64 -mno-red-zone \
  -mno-sse -mno-sse2 -mno-mmx \
  -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memmove \
  -O2 -pipe -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -I. -I$(LIMINE_DIR) $(NEWLIB_CFLAGS)

LIBC_LDFLAGS := \
  -nostdlib -static -no-pie -z max-page-size=0x1000 \
  -m elf_x86_64 -T userspace/libc/xos-libc.ld

# newlib test program
TEST_LIBC_ELF := $(BUILD_DIR)/userspace/libc/test_libc.elf

QEMU_BASE  := -M q35 -m 512M -smp 1 -no-reboot -rtc base=localtime -name "X OS" -vga none -device virtio-gpu-gl-pci,max_outputs=1,xres=2560,yres=1600 -display cocoa,show-cursor=off,gl=es

.PHONY: all run run-uefi clean distclean setup limine

all: $(ISO)

# ---- compile -------------------------------------------------------------
$(OBJ_DIR)/kernel/lib/string.o: kernel/lib/string.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memmove -c $< -o $@

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

# Build xplorer service ELF
$(XPLORER_ELF): userspace/services/xplorer/start.S userspace/services/xplorer/main.c userspace/lib/xgfx/xgfx.c userspace/lib/xgfx/xgfx_font_terminus.c userspace/lib/wm/wm.h userspace/runtime/syscall.c userspace/services/xplorer/xplorer.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/xplorer/start.S -o $(BUILD_DIR)/userspace/services/xplorer/start.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/xplorer/main.c -o $(BUILD_DIR)/userspace/services/xplorer/main.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/lib/xgfx/xgfx.c -o $(BUILD_DIR)/userspace/services/xplorer/xgfx.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/lib/xgfx/xgfx_font_terminus.c -o $(BUILD_DIR)/userspace/services/xplorer/xgfx_font.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/services/xplorer/syscall.o
	$(LD) -nostdlib -static -no-pie -z max-page-size=0x1000 -m elf_x86_64 -T userspace/services/xplorer/xplorer.ld \
	  $(BUILD_DIR)/userspace/services/xplorer/start.o \
	  $(BUILD_DIR)/userspace/services/xplorer/main.o \
	  $(BUILD_DIR)/userspace/services/xplorer/xgfx.o \
	  $(BUILD_DIR)/userspace/services/xplorer/xgfx_font.o \
	  $(BUILD_DIR)/userspace/services/xplorer/syscall.o \
	  -o $@
	@echo ">> linked $@"

$(XPLORER_BLOB_C): $(XPLORER_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t xplorer_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *xplorer_elf_data = xplorer_elf_bytes;', 'size_t xplorer_elf_len = sizeof(xplorer_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

# Build dock service ELF
$(DOCK_ELF): userspace/services/dock/start.S userspace/services/dock/main.c userspace/lib/xgfx/xgfx.c userspace/lib/xgfx/xgfx_font_terminus.c userspace/lib/wm/wm.h userspace/runtime/syscall.c userspace/services/dock/dock.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/dock/start.S -o $(BUILD_DIR)/userspace/services/dock/start.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/dock/main.c -o $(BUILD_DIR)/userspace/services/dock/main.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/lib/xgfx/xgfx.c -o $(BUILD_DIR)/userspace/services/dock/xgfx.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/lib/xgfx/xgfx_font_terminus.c -o $(BUILD_DIR)/userspace/services/dock/xgfx_font.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/services/dock/syscall.o
	$(LD) -nostdlib -static -no-pie -z max-page-size=0x1000 -m elf_x86_64 -T userspace/services/dock/dock.ld \
	  $(BUILD_DIR)/userspace/services/dock/start.o \
	  $(BUILD_DIR)/userspace/services/dock/main.o \
	  $(BUILD_DIR)/userspace/services/dock/xgfx.o \
	  $(BUILD_DIR)/userspace/services/dock/xgfx_font.o \
	  $(BUILD_DIR)/userspace/services/dock/syscall.o \
	  -o $@
	@echo ">> linked $@"

$(DOCK_BLOB_C): $(DOCK_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t dock_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *dock_elf_data = dock_elf_bytes;', 'size_t dock_elf_len = sizeof(dock_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

# Build menubar service ELF
$(MENUBAR_ELF): userspace/services/menubar/start.S userspace/services/menubar/main.c userspace/lib/xgfx/xgfx.c userspace/lib/xgfx/xgfx_font_terminus.c userspace/lib/wm/wm.h userspace/runtime/syscall.c userspace/services/menubar/menubar.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/menubar/start.S -o $(BUILD_DIR)/userspace/services/menubar/start.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/menubar/main.c -o $(BUILD_DIR)/userspace/services/menubar/main.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/lib/xgfx/xgfx.c -o $(BUILD_DIR)/userspace/services/menubar/xgfx.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/lib/xgfx/xgfx_font_terminus.c -o $(BUILD_DIR)/userspace/services/menubar/xgfx_font.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/services/menubar/syscall.o
	$(LD) -nostdlib -static -no-pie -z max-page-size=0x1000 -m elf_x86_64 -T userspace/services/menubar/menubar.ld \
	  $(BUILD_DIR)/userspace/services/menubar/start.o \
	  $(BUILD_DIR)/userspace/services/menubar/main.o \
	  $(BUILD_DIR)/userspace/services/menubar/xgfx.o \
	  $(BUILD_DIR)/userspace/services/menubar/xgfx_font.o \
	  $(BUILD_DIR)/userspace/services/menubar/syscall.o \
	  -o $@
	@echo ">> linked $@"

$(MENUBAR_BLOB_C): $(MENUBAR_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t menubar_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *menubar_elf_data = menubar_elf_bytes;', 'size_t menubar_elf_len = sizeof(menubar_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

# Explicit blob object rules so Make knows to generate the .c first
$(INIT_BLOB_O): $(INIT_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMPOSER_BLOB_O): $(COMPOSER_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(XPLORER_BLOB_O): $(XPLORER_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(DOCK_BLOB_O): $(DOCK_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(MENUBAR_BLOB_O): $(MENUBAR_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ZSH_BLOB_C): $(ZSH_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t zsh_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *zsh_elf_data = zsh_elf_bytes;', 'size_t zsh_elf_len = sizeof(zsh_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

$(ZSH_BLOB_O): $(ZSH_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Build terminal service ELF
$(TERMINAL_ELF): userspace/services/terminal/start.S userspace/services/terminal/main.c userspace/lib/xgfx/xgfx.c userspace/lib/xgfx/xgfx_font_terminus.c userspace/lib/wm/wm.h userspace/runtime/syscall.c userspace/services/terminal/terminal.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/terminal/start.S -o $(BUILD_DIR)/userspace/services/terminal/start.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/terminal/main.c -o $(BUILD_DIR)/userspace/services/terminal/main.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/lib/xgfx/xgfx.c -o $(BUILD_DIR)/userspace/services/terminal/xgfx.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/lib/xgfx/xgfx_font_terminus.c -o $(BUILD_DIR)/userspace/services/terminal/xgfx_font.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/services/terminal/syscall.o
	$(LD) -nostdlib -static -no-pie -z max-page-size=0x1000 -m elf_x86_64 -T userspace/services/terminal/terminal.ld \
	  $(BUILD_DIR)/userspace/services/terminal/start.o \
	  $(BUILD_DIR)/userspace/services/terminal/main.o \
	  $(BUILD_DIR)/userspace/services/terminal/xgfx.o \
	  $(BUILD_DIR)/userspace/services/terminal/xgfx_font.o \
	  $(BUILD_DIR)/userspace/services/terminal/syscall.o \
	  -o $@
	@echo ">> linked $@"

$(TERMINAL_BLOB_C): $(TERMINAL_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t terminal_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *terminal_elf_data = terminal_elf_bytes;', 'size_t terminal_elf_len = sizeof(terminal_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

$(TERMINAL_BLOB_O): $(TERMINAL_BLOB_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Build svgview service ELF
$(SVGVIEW_ELF): userspace/services/svgview/start.S userspace/services/svgview/main.c userspace/lib/nanosvg/nanosvg_xos.c userspace/lib/nanosvg/nanosvg.h userspace/lib/nanosvg/nanosvgrast.h userspace/lib/wm/wm.h userspace/runtime/syscall.c userspace/services/svgview/svgview.ld
	@mkdir -p $(dir $@)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/svgview/start.S -o $(BUILD_DIR)/userspace/services/svgview/start.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/services/svgview/main.c -o $(BUILD_DIR)/userspace/services/svgview/main.o
	$(CC) $(USERSPACE_CFLAGS) -Iuserspace/lib/nanosvg/stubs -c userspace/lib/nanosvg/nanosvg_xos.c -o $(BUILD_DIR)/userspace/services/svgview/nanosvg_xos.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/services/svgview/syscall.o
	$(LD) -nostdlib -static -no-pie -z max-page-size=0x1000 -m elf_x86_64 -T userspace/services/svgview/svgview.ld \
	  $(BUILD_DIR)/userspace/services/svgview/start.o \
	  $(BUILD_DIR)/userspace/services/svgview/main.o \
	  $(BUILD_DIR)/userspace/services/svgview/nanosvg_xos.o \
	  $(BUILD_DIR)/userspace/services/svgview/syscall.o \
	  -o $@
	@echo ">> linked $@"

$(SVGVIEW_BLOB_C): $(SVGVIEW_ELF)
	@mkdir -p $(dir $@)
	@python3 -c "import os; data=open('$<','rb').read(); lines=['#include <stdint.h>', '#include <stddef.h>', '', 'static const uint8_t svgview_elf_bytes[] = {']; lines += ['    ' + ', '.join('0x%02x'%b for b in data[i:i+12]) + ',' for i in range(0,len(data),12)]; lines += ['};', '', 'const uint8_t *svgview_elf_data = svgview_elf_bytes;', 'size_t svgview_elf_len = sizeof(svgview_elf_bytes);']; open('$@','w').write('\n'.join(lines)+'\n')"
	@echo ">> generated $@"

$(SVGVIEW_BLOB_O): $(SVGVIEW_BLOB_C)
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

# ---- zsh shell (interactive shell via newlib) -----------------------------
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
	@echo ">> Building zsh (using local zsh-src)..."
	@# Compile our _start entry point (assembly for stack alignment)
	$(CC) $(USERSPACE_CFLAGS) -c userspace/shell/zsh_start.S -o $(BUILD_DIR)/userspace/shell/zsh_start.o
	@# Compile our zsh entry (IPC bridge setup + echo shell)
	$(CC) $(LIBC_CFLAGS) -c userspace/shell/zsh_entry.c -o $(BUILD_DIR)/userspace/shell/zsh_entry.o
	@# Compile our syscall stubs
	$(CC) $(LIBC_CFLAGS) -c userspace/libc/syscalls.c -o $(BUILD_DIR)/userspace/shell/zsh_syscalls.o
	$(CC) $(USERSPACE_CFLAGS) -c userspace/runtime/syscall.c -o $(BUILD_DIR)/userspace/shell/zsh_xos_syscall.o
	@# Link zsh with our entry point and syscall stubs
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
	@# Strip debug info to reduce embedded size
	/opt/homebrew/opt/llvm/bin/llvm-strip $(ZSH_ELF)
	@echo ">> linked $@"

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
setup: limine

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
	rm -rf $(LIMINE_DIR)

-include $(DEPS)
