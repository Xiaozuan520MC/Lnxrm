# SPDX-License-Identifier: Apache-2.0

CROSS   :=
CC      := $(CROSS)clang
LD      := $(CROSS)ld.lld
OBJCOPY := $(CROSS)llvm-objcopy
NASM    := nasm -w-implicit-abs-deprecated
PYTHON3 := python3

BUILD   := build
ARCHDIR := arch

ISO_TREE := $(BUILD)/iso
ISO_CFG  := $(ISO_TREE)/boot/grub/grub.cfg
ISO_KRN  := $(ISO_TREE)/boot/bzImage
ISO      := $(BUILD)/lnxrm.iso

KVM ?= $(shell test -r /dev/kvm && test -w /dev/kvm && echo --enable-kvm)
MACHINE ?= q35
GRUB_TIMEOUT ?= 0

CMDLINE ?=
ifneq ($(CMDLINE),)
RUN_APPEND := -append "$(CMDLINE)"
endif

WARN    := -Wall -Wextra -Wno-unused-parameter
MAKEFLAGS += --no-print-directory
.DELETE_ON_ERROR:

KTEST ?= 0
ifeq ($(KTEST),1)
KTEST_DIR  := ktest
KTEST_DEFS := -DCONFIG_KTEST=1
else
KTEST_DIR  :=
KTEST_DEFS :=
endif

OBJDIR := $(BUILD)/obj$(KTEST)

MODE_MARK := $(BUILD)/.mode-$(KTEST)

BASEFLAGS := -ffreestanding -fno-stack-protector -fno-omit-frame-pointer -fno-pic -fno-pie \
             -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
             -Os -g $(WARN) -Iinclude -I. -Iktest/include -MMD -MP \
             -ffunction-sections -fdata-sections $(KTEST_DEFS)
KERNFLAGS := $(BASEFLAGS) -std=gnu11

.PHONY: all clean run user mod smoke imageiso help
all: $(BUILD)/bzImage

KASM_ALL := $(shell find arch -name '*.S' 2>/dev/null)
KASM     := $(filter-out arch/trampoline.S,$(KASM_ALL))

ARCH_SRC_C  := $(shell find arch -name '*.c' 2>/dev/null)
ARCH_C_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(ARCH_SRC_C))
ARCH_S_OBJS := $(patsubst %.S,$(OBJDIR)/%.o,$(KASM))

KERN_SRC_C  := $(shell find kernel -name '*.c' 2>/dev/null)
KTEST_SRC_C := $(if $(KTEST_DIR),$(shell find $(KTEST_DIR) -name '*.c' 2>/dev/null))

KERN_OBJS  := $(patsubst %.c,$(OBJDIR)/%.o,$(KERN_SRC_C))
KTEST_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(KTEST_SRC_C))

USER_PROGS := $(notdir $(basename $(wildcard userland/bin/*.c)))
USER_ELFS  := $(addprefix $(BUILD)/user/,$(USER_PROGS))

MOD_PROGS := $(notdir $(basename $(wildcard userland/mod/*.c)))

MOD_ELFS  := $(addprefix $(BUILD)/mod/,$(addsuffix .km,$(MOD_PROGS)))

# ---- kernel rules -------------------------------------------------------
$(OBJDIR)/%.o: %.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	@printf "  CC      $(SHORT)\n"
	@$(CC) $(KERNFLAGS) -MMD -MP -c -o $@ $<

# ---- architecture rules -------------------------------------------------
$(OBJDIR)/arch/%.o: arch/%.S | $(OBJDIR)
	@mkdir -p $(dir $@)
	@printf "  NASM    $(SHORT)\n"
	@$(NASM) -f elf64 -F dwarf -g -o $@ $<

$(BUILD)/trampoline.bin: arch/trampoline.S | $(BUILD)
	@printf "  NASM    $(SHORT)\n"
	@$(NASM) -f bin -o $@ $<

$(OBJDIR)/trampoline.o: $(BUILD)/trampoline.bin | $(OBJDIR)
	@printf "  OBJCOPY $(SHORT)\n"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.trampoline $< $@ \
	  --redefine-sym _binary_build_trampoline_bin_start=trampoline_start \
	  --redefine-sym _binary_build_trampoline_bin_end=trampoline_end

SHORT = $(patsubst $(BUILD)/%,%,$(patsubst $(OBJDIR)/%,%,$@))

KOBJ_C   := $(KERN_OBJS) $(ARCH_C_OBJS) $(KTEST_OBJS)
KOBJ_ASM := $(ARCH_S_OBJS) $(OBJDIR)/trampoline.o
KOBJ     := $(KOBJ_C) $(KOBJ_ASM)

user:
	@$(MAKE) -C userland BUILD=$(abspath $(BUILD))

mod:
	@$(MAKE) -C userland BUILD=$(abspath $(BUILD)) mod

$(USER_ELFS): $(BUILD)/user/%: userland/bin/%.c userland/lib/ulib.c userland/lib/crt0.S | user
	@test -f $@

USER_LIBS := $(BUILD)/lib/ld.so $(BUILD)/lib/libulib.so

$(USER_LIBS): | user
	@test -f $@

$(MOD_ELFS): $(BUILD)/mod/%.km: userland/mod/%.c userland/lib/ulib.c userland/lib/crt0.S | mod
	@test -f $@

$(BUILD):
	@mkdir -p $@

$(OBJDIR):
	@mkdir -p $@

$(MODE_MARK): | $(BUILD)
	@rm -f $(BUILD)/.mode-*
	@touch $@

$(BUILD)/vmlinux.elf: $(KOBJ) $(ARCHDIR)/kernel.ld $(MODE_MARK)
	@printf "  LD      $(SHORT)\n"
	@$(LD) -T $(ARCHDIR)/kernel.ld --gc-sections -o $@ $(KOBJ) \
	      -Map $(BUILD)/vmlinux.map

$(BUILD)/vmlinux.bin: $(BUILD)/vmlinux.elf
	@printf "  OBJCOPY $(SHORT)\n"
	@$(OBJCOPY) -O binary $< $@
	
$(BUILD)/setup.bin: $(ARCHDIR)/setup.asm $(BUILD)/vmlinux.elf | $(BUILD)
	@printf "  NASM    $(SHORT)\n"
	@$(NASM) -f bin -o $@ $<
	@python3 -c "d=open('$@','rb').read();pad=(2048-len(d)%2048)%2048;open('$@','wb').write(d+b'\x00'*pad)"
	@$(PYTHON3) scripts/patch_setup_jmp.py $@ $(BUILD)/vmlinux.elf

$(BUILD)/bzImage: $(BUILD)/setup.bin $(BUILD)/vmlinux.bin
	@printf "  BZIMAGE $(SHORT)\n"
	@cat $(BUILD)/setup.bin $(BUILD)/vmlinux.bin > $@
	@$(PYTHON3) scripts/patch_bzimage.py $@ $$(( $$(stat -c%s $(BUILD)/setup.bin) ))
	@printf "  SIZE    %s bytes\n" $$(stat -c%s $@)
	@printf "  Done!\n"

clean:
	@rm -rf $(BUILD)
	@printf "CLEAN!\n"

help:
	@echo make
	@echo make clean
	@echo make user
	@echo make mod
	@echo make run
	@echo make smoke
	@echo make imageiso
	@echo make mod

DISK ?= 1
ifeq ($(DISK),1)
RUN_DEPS  := $(BUILD)/disk.img
RUN_DRIVE := -drive file=$(BUILD)/disk.img,format=raw,if=ide,index=0,media=disk
endif

run: user build/bzImage $(RUN_DEPS)
	@qemu-system-x86_64 $(KVM) -machine $(MACHINE) -m 256 -smp 2 -cpu qemu64,+smep,+smap \
	  -kernel build/bzImage $(RUN_APPEND) $(RUN_DRIVE) \
	  -serial stdio -no-reboot
ahci: run

SMOKE_QEMU_ARGS ?= -machine q35

smoke:
	@MAKEFLAGS="$(filter-out KTEST=%,$(MAKEFLAGS))" KTEST=1 \
	  $(PYTHON3) scripts/smoke_test.py --qemu-args "$(SMOKE_QEMU_ARGS)" $(SMOKE_FLAGS)

imageiso: $(ISO)

$(ISO_CFG): | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  GRUB    $(SHORT)\n"
	@printf '%s\n' \
	  'serial --unit=0 --speed=115200' \
	  'terminal_output serial console' \
	  "set timeout=$(GRUB_TIMEOUT)" \
	  'set default=0' \
	  'insmod all_video' \
	  'set gfxmode=1920x1080x32,1280x720x32,auto' \
	  'set gfxpayload=1920x1080x32,1280x720x32,keep' \
	  'menuentry "lnxrm" {' \
	  '  linux /boot/bzImage console=ttyS0' \
	  '  boot' \
	  '}' > $@

$(ISO_KRN): $(BUILD)/bzImage | $(BUILD)
	@mkdir -p $(dir $@)
	@cp $< $@

$(ISO): $(ISO_KRN) $(ISO_CFG)
	@command -v grub-mkrescue >/dev/null 2>&1 || { \
	  echo "error: grub-mkrescue not found -- install grub-pc-bin (Debian/Ubuntu)"; \
	  echo "       or grub2-pc / grub2-tools (Fedora) plus xorriso and mtools"; \
	  exit 1; }
	@printf "  ISO     $(SHORT)\n"
	@grub-mkrescue -o $@ $(ISO_TREE) >$(BUILD)/iso.log 2>&1 || { \
	  cat $(BUILD)/iso.log >&2; exit 1; }

$(BUILD)/disk.img: Makefile $(BUILD)/README.md $(USER_ELFS) $(USER_LIBS) $(MOD_ELFS)
	@printf "  DISK    $(SHORT)\n"
	@for t in mke2fs debugfs; do \
	  command -v $$t >/dev/null 2>&1 || { \
	    echo "error: $$t not found -- install e2fsprogs (mke2fs, debugfs, e2fsck)"; \
	    exit 1; }; \
	done
	@python3 scripts/pack_img.py $(BUILD) $(BUILD)/README.md $@ \
	       --user-elfs $(USER_ELFS) --libs $(USER_LIBS) --mods $(MOD_ELFS)
	@# ::mod holds the ring3 driver modules (S1b).  Kept out of ::bin so
	@# a plain `ls /bin` still lists programs and init's /mod scan is the
	@# only thing that ever spawns a .km.  The names are copied through AS
	@# THEY ARE: build/mod/<name>.km already carries the suffix (MOD_ELFS
	@# adds it when linking), so appending one here would install
	@# ps2-keyboard.km.km -- and a bare build/mod/<name> would install a
	@# name init's *.km scan never matches, which is the bug that left the
	@# modules on the disk, unspawned, with nothing said.  (The same rule
	@# lives in scripts/pack_img.py; this comment stays with the why.)

$(BUILD)/README.md: | $(BUILD)
	@echo "A tiny unix-like kernel in ASM + C!" > $@

-include $(KOBJ_C:.o=.d)
