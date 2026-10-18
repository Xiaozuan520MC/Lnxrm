KASM_ALL := $(shell find arch -name '*.S' 2>/dev/null)
KASM     := $(filter-out arch/trampoline.S,$(KASM_ALL))

ARCH_SRC_C  := $(shell find arch -name '*.c' 2>/dev/null)
ARCH_C_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(ARCH_SRC_C))
ARCH_S_OBJS := $(patsubst %.S,$(BUILD)/%.o,$(KASM))

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
