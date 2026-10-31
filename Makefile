CROSS   :=
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
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

BASEFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
             -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
             -Os -g $(WARN) -Iinclude -I. -Iktest/include -MMD -MP \
             -ffunction-sections -fdata-sections $(KTEST_DEFS)
KERNFLAGS := $(BASEFLAGS) -std=gnu11

.PHONY: all clean run user smoke imageiso initramfs help
all: $(BUILD)/bzImage

KASM_ALL := $(shell find arch -name '*.S' 2>/dev/null)
KASM     := $(filter-out arch/trampoline.S,$(KASM_ALL))

ARCH_SRC_C  := $(shell find arch -name '*.c' 2>/dev/null)
ARCH_C_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(ARCH_SRC_C))
ARCH_S_OBJS := $(patsubst %.S,$(BUILD)/%.o,$(KASM))

KERN_SRC_C  := $(shell find kernel -name '*.c' 2>/dev/null)
KTEST_SRC_C := $(if $(KTEST_DIR),$(shell find $(KTEST_DIR) -name '*.c' 2>/dev/null))

KERN_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(KERN_SRC_C))
KTEST_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(KTEST_SRC_C))

USER_PROGS := $(notdir $(basename $(wildcard userland/bin/*.c)))
USER_ELFS  := $(addprefix $(BUILD)/user/,$(USER_PROGS))

# ---- kernel rules -------------------------------------------------------
$(BUILD)/%.o: %.c | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CC      $(SHORT)\n"
	@$(CC) $(KERNFLAGS) -MMD -MP -c -o $@ $<

# ---- architecture rules -------------------------------------------------
$(BUILD)/arch/%.o: arch/%.S | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  NASM    $(SHORT)\n"
	@$(NASM) -f elf64 -F dwarf -g -o $@ $<

$(BUILD)/trampoline.bin: arch/trampoline.S | $(BUILD)
	@printf "  NASM    $(SHORT)\n"
	@$(NASM) -f bin -o $@ $<

$(BUILD)/trampoline.o: $(BUILD)/trampoline.bin | $(BUILD)
	@printf "  OBJCOPY $(SHORT)\n"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.trampoline $< $@ \
	  --redefine-sym _binary_build_trampoline_bin_start=trampoline_start \
	  --redefine-sym _binary_build_trampoline_bin_end=trampoline_end \
	  --redefine-sym _binary_build_trampoline_bin_size=trampoline_size

# Print targets relative to the build dir (short, cwd-independent).
SHORT = $(patsubst $(BUILD)/%,%,$@)

KOBJ_C   := $(KERN_OBJS) $(ARCH_C_OBJS) $(KTEST_OBJS)
KOBJ_ASM := $(ARCH_S_OBJS) $(BUILD)/trampoline.o
KOBJ     := $(KOBJ_C) $(KOBJ_ASM)

INITRAMFS ?=
ifneq ($(INITRAMFS),)
KOBJ += $(BUILD)/initramfs.o
endif

KTEST_STAMP := $(BUILD)/.ktest-mode-$(KTEST)
$(KTEST_STAMP):
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/.ktest-mode-*
	@touch $@
$(KOBJ): $(KTEST_STAMP)

user:
	@$(MAKE) -C userland BUILD=$(abspath $(BUILD))

$(USER_ELFS): $(BUILD)/user/%: userland/bin/%.c userland/lib/ulib.c userland/lib/crt0.S | user
	@test -f $@

$(BUILD):
	@mkdir -p $@

$(BUILD)/ifsrc/.stamp: $(USER_ELFS) $(BUILD)/README.md | $(BUILD)
	@rm -rf $(BUILD)/ifsrc
	@mkdir -p $(BUILD)/ifsrc/bin
	@cp $(USER_ELFS) $(BUILD)/ifsrc/bin/
	@cp $(BUILD)/README.md $(BUILD)/ifsrc/
	@touch $@

$(BUILD)/initramfs.cpio: $(BUILD)/ifsrc/.stamp scripts/mkinitramfs.py
	@$(PYTHON3) scripts/mkinitramfs.py $@ $(BUILD)/ifsrc

$(BUILD)/initramfs.o: $(INITRAMFS) | $(BUILD)
	@printf "  OBJCOPY $(SHORT) (initramfs %s)\n" "$$(stat -c%s $< 2>/dev/null || echo '?') bytes"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.initramfs,alloc,load,readonly,data,contents $< $@

initramfs: $(BUILD)/initramfs.cpio

$(BUILD)/vmlinux.elf: $(KOBJ) $(ARCHDIR)/kernel.ld
	@printf "  LD      $(SHORT)\n"
	@$(LD) -T $(ARCHDIR)/kernel.ld --gc-sections -o $@ $(KOBJ) \
	      -Map $(BUILD)/vmlinux.map \
	      --no-warn-rwx-segments

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
	@echo Done!

help:
	@echo make
	@echo make clean
	@echo make user
	@echo make run
	@echo make smoke
	@echo make imageiso
	@echo make INITRAMFS=build/initramfs.cpio

DISK ?= 1
ifeq ($(DISK),1)
RUN_DEPS  := $(BUILD)/disk.img
RUN_DRIVE := -drive file=$(BUILD)/disk.img,format=raw,if=ide,index=0,media=disk
endif

run: user build/bzImage $(RUN_DEPS)
	@qemu-system-x86_64 $(KVM) -machine $(MACHINE) -m 256 -smp 2 -cpu qemu64,+smep,+smap \
	  -kernel build/bzImage $(RUN_DRIVE) \
	  -serial stdio -no-reboot -d int -D build/qemu.log
ahci: run

smoke:
	@echo "smoke: forcing KTEST=1 (self-test markers are what it checks)"
	@MAKEFLAGS="$(filter-out KTEST=%,$(MAKEFLAGS))" KTEST=1 \
	  $(PYTHON3) scripts/smoke_test.py $(SMOKE_FLAGS)

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

$(BUILD)/disk.img: $(BUILD)/README.md $(USER_ELFS)
	@printf "  DISK    $(SHORT)\n"
	@dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	@# -R 1024: reserve 1024 sectors (512 KiB).  FAT32 leaves the reserved
	@# area to the OS, and this kernel keeps its crash-consistency journal
	@# in sectors 8..reserved-1 of it -- mkfs's default 32 sectors would give
	@# the log only 24 sectors (~11 records), too small for the unlink/write
	@# transactions the driver runs.  Nothing else in FAT32 uses them.
	@mkfs.vfat -F 32 -R 1024 $@ >/dev/null 2>/dev/null
	@mcopy -i $@ $(BUILD)/README.md ::README.md
	@mmd -i $@ ::bin
	@for f in $(USER_ELFS); do \
	  mcopy -i $@ $$f ::bin/$$(basename $$f); \
	done

$(BUILD)/README.md: | $(BUILD)
	@echo "A tiny unix-like kernel in ASM + C!" > $@

-include $(KOBJ_C:.o=.d)
