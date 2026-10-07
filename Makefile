CROSS   :=
CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
RUSTC   := rustc
NASM    := nasm -w-implicit-abs-deprecated
PYTHON3 := python3

BUILD   := build
ARCHDIR := arch

# Bootable ISO (legacy BIOS + GRUB).  The whole ISO tree is staged inside
# build/iso/ so nothing is ever written outside $(BUILD), and grub.cfg is
# generated (not committed) so `make clean && make imageiso` always works.
ISO_TREE := $(BUILD)/iso
ISO_CFG  := $(ISO_TREE)/boot/grub/grub.cfg
ISO_KRN  := $(ISO_TREE)/boot/bzImage
ISO      := $(BUILD)/lnxrm.iso

WARN    := -Wall -Wextra -Wno-unused-parameter
MAKEFLAGS += --no-print-directory

# A recipe that dies halfway (e.g. mcopy loop on disk.img) must not leave a
# half-written artifact behind that make then treats as up to date.
.DELETE_ON_ERROR:

# Boot self-test suite (ktest/): off by default -- a stock `make` links no
# test code into the kernel.  `make KTEST=1` compiles it: ktest/ joins the
# source list and main.c's ktest_run() calls appear behind -DCONFIG_KTEST.
# The ASSERT/WARN/BUG_ON infrastructure is production code and stays either
# way (kernel/selftest.c).
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
             -O2 -g $(WARN) -Iinclude -Iinclude/cpp -I. -MMD -MP \
             -ffunction-sections -fdata-sections $(KTEST_DEFS)
KERNFLAGS := $(BASEFLAGS) -std=gnu11
CXXFLAGS  := $(BASEFLAGS) -std=c++20 -fno-exceptions -fno-rtti \
             -fno-threadsafe-statics -fno-use-cxa-atexit -nostdinc++

RUSTFLAGS := --crate-type staticlib --edition 2021 -C panic=abort -C opt-level=s \
             -C relocation-model=static -C code-model=kernel \
             -C target-feature=-sse -C debuginfo=0 \
             --target x86_64-unknown-none

USRFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
             -nostdlib -nostartfiles -static -no-pie -mcmodel=large $(WARN) \
             -mno-mmx -mno-sse -Iinit -Iinclude -idirafter include

USER_LINK_BASE := 0x7f8000400000

KSRC_C   := $(shell find kernel arch $(KTEST_DIR) -name '*.c' 2>/dev/null)
KSRC_CXX := $(shell find kernel arch $(KTEST_DIR) -name '*.cpp' 2>/dev/null)
KASM_ALL := $(shell find arch -name '*.S' 2>/dev/null)
KASM     := $(filter-out arch/trampoline.S,$(KASM_ALL))
RUST_SRC := $(shell find kernel/rust -name '*.rs' 2>/dev/null)
USR_ALL_C := $(shell find usr -maxdepth 1 -name '*.c' 2>/dev/null)
UIMG      := $(patsubst usr/%.c,%,$(USR_ALL_C))
# user-space object dir and final ELFs
USR_OBJDIR := $(BUILD)/usro
USER_ELFS  := $(addprefix $(BUILD)/usr/,$(UIMG))

KOBJ_C   := $(patsubst %.c,$(BUILD)/%.o,$(KSRC_C))
KOBJ_CXX := $(patsubst %.cpp,$(BUILD)/%.opp,$(KSRC_CXX))
KOBJ_ASM := $(patsubst %.S,$(BUILD)/%.o,$(KASM)) $(BUILD)/trampoline.o
KOBJ     := $(KOBJ_C) $(KOBJ_CXX) $(KOBJ_ASM)

.PHONY: all clean run usr rust smoke imageiso help
all: $(BUILD)/bzImage

KTEST_STAMP := $(BUILD)/.ktest-mode-$(KTEST)
$(KTEST_STAMP):
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/.ktest-mode-*
	@touch $@
$(KOBJ): $(KTEST_STAMP)

# user space must be built before the disk image and before booting
usr: $(USER_ELFS)

$(BUILD):
	@mkdir -p $@

$(BUILD)/liblnxrm.a: $(RUST_SRC) | $(BUILD)
	@mkdir -p $(BUILD)/rustobj
	@printf "  RUST    $@\n"
	@$(RUSTC) $(RUSTFLAGS) --crate-name lnxrmrust -o $@ kernel/rust/lib.rs

rust: $(BUILD)/liblnxrm.a

$(BUILD)/trampoline.bin: arch/trampoline.S | $(BUILD)
	@printf "  NASM    $@\n"
	@$(NASM) -f bin -o $@ $<

$(BUILD)/trampoline.o: $(BUILD)/trampoline.bin | $(BUILD)
	@printf "  OBJCOPY $@\n"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.trampoline $< $@ \
	  --redefine-sym _binary_build_trampoline_bin_start=trampoline_start \
	  --redefine-sym _binary_build_trampoline_bin_end=trampoline_end \
	  --redefine-sym _binary_build_trampoline_bin_size=trampoline_size

$(BUILD)/arch/%.o: arch/%.S | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  NASM    $@\n"
	@$(NASM) -f elf64 -F dwarf -g -o $@ $<

$(BUILD)/%.o: %.c | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CC      $@\n"
	@$(CC) $(KERNFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.opp: %.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CXX     $@\n"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/vmlinux.elf: $(KOBJ) $(ARCHDIR)/kernel.ld $(BUILD)/liblnxrm.a
	@printf "  LD      $@\n"
	@$(LD) -T $(ARCHDIR)/kernel.ld --gc-sections -o $@ $(KOBJ) \
	      $(BUILD)/liblnxrm.a -Map $(BUILD)/vmlinux.map \
	      --no-warn-rwx-segments

$(BUILD)/vmlinux.bin: $(BUILD)/vmlinux.elf
	@printf "  OBJCOPY $@\n"
	@$(OBJCOPY) -O binary $< $@
	
$(BUILD)/setup.bin: $(ARCHDIR)/setup.asm $(BUILD)/vmlinux.elf | $(BUILD)
	@printf "  NASM    $@\n"
	@$(NASM) -f bin -o $@ $<
	@python3 -c "d=open('$@','rb').read();pad=(2048-len(d)%2048)%2048;open('$@','wb').write(d+b'\x00'*pad)"
	@$(PYTHON3) scripts/patch_setup_jmp.py $@ $(BUILD)/vmlinux.elf

$(BUILD)/bzImage: $(BUILD)/setup.bin $(BUILD)/vmlinux.bin
	@printf "  BZIMAGE $@\n"
	@cat $(BUILD)/setup.bin $(BUILD)/vmlinux.bin > $@
	@$(PYTHON3) scripts/patch_bzimage.py $@ $$(( $$(stat -c%s $(BUILD)/setup.bin) ))
	@printf "  SIZE    %s bytes\n" $$(stat -c%s $@)
	@printf "  Done!\n"

$(USR_OBJDIR)/crt0.o: init/crt0.S | $(BUILD)
	@mkdir -p $(USR_OBJDIR)
	@printf "  NASM    $@\n"
	@$(NASM) -f elf64 -o $@ $<

$(USR_OBJDIR)/ulib.o: init/ulib.c | $(BUILD)
	@mkdir -p $(USR_OBJDIR)
	@printf "  CC      $@\n"
	@$(CC) $(USRFLAGS) -MMD -MP -c -o $@ $<

$(USR_OBJDIR)/%.o: usr/%.c | $(BUILD)
	@mkdir -p $(USR_OBJDIR)
	@printf "  CC      $@\n"
	@$(CC) $(USRFLAGS) -MMD -MP -c -o $@ $<

$(USER_ELFS): $(BUILD)/usr/%: $(USR_OBJDIR)/%.o $(USR_OBJDIR)/ulib.o $(USR_OBJDIR)/crt0.o | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  LD      $@\n"
	@$(CC) $(USRFLAGS) -Wl,-Ttext=$(USER_LINK_BASE) -o $@ $^

clean:
	@rm -rf $(BUILD)
	@rm -f usr/*.o usr/*.d
	@echo Done!

help:
	@echo make -Build kernel
	@echo make clean -Clean compiled files
	@echo make usr -Build Build user program
	@echo make run -Start the kernel
	@echo make rust -Compile Rust
	@echo make smoke -Smoke test
	@echo make KTEST=1 -Build with the ktest self-test suite
	@echo make imageiso -Build ISO image

run: usr $(BUILD)/bzImage $(BUILD)/disk.img
	@qemu-system-x86_64 -m 256 -smp 2 -cpu qemu64,+smep,+smap -kernel $(BUILD)/bzImage \
	  -drive file=$(BUILD)/disk.img,format=raw,if=ide,index=0,media=disk \
	  -serial stdio -no-reboot -d int -D build/qemu.log  

smoke:
	@echo "smoke: forcing KTEST=1 (self-test markers are what it checks)"
	@MAKEFLAGS="$(filter-out KTEST=%,$(MAKEFLAGS))" KTEST=1 \
	  $(PYTHON3) scripts/smoke_test.py $(SMOKE_FLAGS)

imageiso: $(ISO)

$(ISO_CFG): | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  GRUB    $@\n"
	@printf '%s\n' \
	  'serial --unit=0 --speed=115200' \
	  'terminal_output serial' \
	  'set timeout=0' \
	  'set default=0' \
	  'insmod all_video' \
	  'set gfxmode=1280x720x32' \
	  'set gfxpayload=1280x720x32' \
	  'menuentry "lnxrm" {' \
	  '  linux /boot/bzImage console=ttyS0' \
	  '  boot' \
	  '}' > $@

$(ISO_KRN): $(BUILD)/bzImage | $(BUILD)
	@mkdir -p $(dir $@)
	@cp $< $@

# grub-mkrescue/xorriso chat on stderr; keep it out of the build output but
# dump the whole transcript when the build fails (build/iso.log has it too).
$(ISO): $(ISO_KRN) $(ISO_CFG)
	@command -v grub-mkrescue >/dev/null 2>&1 || { \
	  echo "error: grub-mkrescue not found -- install grub-pc-bin (Debian/Ubuntu)"; \
	  echo "       or grub2-pc / grub2-tools (Fedora) plus xorriso and mtools"; \
	  exit 1; }
	@printf "  ISO     $@\n"
	@grub-mkrescue -o $@ $(ISO_TREE) >$(BUILD)/iso.log 2>&1 || { \
	  cat $(BUILD)/iso.log >&2; exit 1; }

$(BUILD)/disk.img: $(BUILD)/README.md $(USER_ELFS)
	@printf "  DISK    $@\n"
	@dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	@mkfs.vfat -F 32 $@ >/dev/null 2>/dev/null
	@mcopy -i $@ $(BUILD)/README.md ::README.md
	@mmd -i $@ ::bin
	@for f in $(USER_ELFS); do \
	  mcopy -i $@ $$f ::bin/$$(basename $$f); \
	done

$(BUILD)/README.md: | $(BUILD)
	@echo "A tiny unix-like kernel in ASM + C + C++ + Rust!" > $@

-include $(KOBJ_C:.o=.d) $(KOBJ_CXX:.opp=.d) $(USR_ALL_C:usr/%.c=$(USR_OBJDIR)/%.d)
