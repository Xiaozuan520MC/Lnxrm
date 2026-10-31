CROSS   ?=
CC      := $(CROSS)gcc
NASM    := nasm -w-implicit-abs-deprecated

WARN    := -Wall -Wextra -Wno-unused-parameter

BUILD   ?= $(abspath ../build)

MAKEFLAGS += --no-print-directory
.DELETE_ON_ERROR:

USERFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
             -nostdlib -nostartfiles -static -no-pie -mcmodel=large $(WARN) \
             -mno-mmx -mno-sse -Ilib -I../include -idirafter ../include

USER_LINK_BASE := 0x7f8000400000

USER_ALL_C := $(shell find bin -maxdepth 1 -name '*.c' 2>/dev/null)
UIMG      := $(patsubst bin/%.c,%,$(USER_ALL_C))

USER_OBJDIR := $(BUILD)/usero
USER_ELFS   := $(addprefix $(BUILD)/user/,$(UIMG))

# Print targets relative to the build dir (short, cwd-independent).
SHORT = $(patsubst $(BUILD)/%,%,$@)

.PHONY: all clean
all: $(USER_ELFS)

$(BUILD):
	@mkdir -p $@

$(USER_OBJDIR)/crt0.o: lib/crt0.S | $(BUILD)
	@mkdir -p $(USER_OBJDIR)
	@printf "  NASM    $(SHORT)\n"
	@$(NASM) -f elf64 -o $@ $<

$(USER_OBJDIR)/ulib.o: lib/ulib.c | $(BUILD)
	@mkdir -p $(USER_OBJDIR)
	@printf "  CC      $(SHORT)\n"
	@$(CC) $(USERFLAGS) -MMD -MP -c -o $@ $<

$(USER_OBJDIR)/%.o: bin/%.c | $(BUILD)
	@mkdir -p $(USER_OBJDIR)
	@printf "  CC      $(SHORT)\n"
	@$(CC) $(USERFLAGS) -MMD -MP -c -o $@ $<

$(USER_ELFS): $(BUILD)/user/%: $(USER_OBJDIR)/%.o $(USER_OBJDIR)/ulib.o $(USER_OBJDIR)/crt0.o | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  LD      $(SHORT)\n"
	@$(CC) $(USERFLAGS) -Wl,-Ttext=$(USER_LINK_BASE) -o $@ $^

clean:
	@rm -rf $(BUILD)/usero $(BUILD)/user
	@echo Done!

-include $(USER_ALL_C:bin/%.c=$(USER_OBJDIR)/%.d) $(USER_OBJDIR)/ulib.d
