# SPDX-License-Identifier: Apache-2.0

CROSS   ?=
CC      := $(CROSS)clang
NASM    := nasm -w-implicit-abs-deprecated

WARN    := -Wall -Wextra -Wno-unused-parameter

BUILD   ?= $(abspath ../build)

MAKEFLAGS += --no-print-directory
.DELETE_ON_ERROR:

USERFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
             -mcmodel=large $(WARN) \
             -mno-mmx -mno-sse -Ilib -I../include
USERLDFLAGS := -nostdlib -nostartfiles -static -fuse-ld=lld

USER_LINK_BASE := 0x7f8000400000

DYNFLAGS := -ffreestanding -fno-stack-protector -fPIC -O2 \
            $(WARN) \
            -mno-mmx -mno-sse -Ilib -I../include
DYNLDFLAGS := -nostdlib -nostartfiles -fuse-ld=lld -pie \
              -Wl,--dynamic-linker=/lib/ld.so \
              -Wl,--hash-style=sysv

LIBDIR   := $(BUILD)/lib
LIBULIB  := $(LIBDIR)/libulib.so
LDSO     := $(LIBDIR)/ld.so

USER_ALL_C := $(shell find bin -maxdepth 1 -name '*.c' 2>/dev/null)
UIMG      := $(patsubst bin/%.c,%,$(USER_ALL_C))

STATIC_PROGS := init
DYN_PROGS    := $(filter-out $(STATIC_PROGS),$(UIMG))

MOD_ALL_C := $(shell find mod -maxdepth 1 -name '*.c' 2>/dev/null)
MODIMG    := $(patsubst mod/%.c,%,$(MOD_ALL_C))

USER_OBJDIR := $(BUILD)/usero
DYN_ELFS    := $(addprefix $(BUILD)/user/,$(DYN_PROGS))
STATIC_ELFS := $(addprefix $(BUILD)/user/,$(STATIC_PROGS))
USER_ELFS   := $(DYN_ELFS) $(STATIC_ELFS)

MOD_ELFS    := $(addprefix $(BUILD)/mod/,$(addsuffix .km,$(MODIMG)))

SHORT = $(patsubst $(BUILD)/%,%,$@)

.PHONY: all mod clean
all: $(USER_ELFS) $(LDSO) $(LIBULIB)
mod: $(MOD_ELFS)

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

# Position independent ulib.o, the sole input of libulib.so.
$(USER_OBJDIR)/pic/ulib.o: lib/ulib.c | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CC      $(SHORT)\n"
	@$(CC) $(DYNFLAGS) -MMD -MP -c -o $@ $<

$(USER_OBJDIR)/static/%.o: bin/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CC      $(SHORT)\n"
	@$(CC) $(USERFLAGS) -MMD -MP -c -o $@ $<

$(USER_OBJDIR)/pic/%.o: bin/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CC      $(SHORT)\n"
	@$(CC) $(DYNFLAGS) -MMD -MP -c -o $@ $<

$(USER_OBJDIR)/pic/mod/%.o: mod/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CC      $(SHORT)\n"
	@$(CC) $(DYNFLAGS) -MMD -MP -c -o $@ $<

$(USER_OBJDIR)/pic/ldso/ldso.o: ldso/ldso.c | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CC      $(SHORT)\n"
	@$(CC) $(DYNFLAGS) -MMD -MP -c -o $@ $<

$(USER_OBJDIR)/ldso/entry.o: ldso/entry.S | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  NASM    $(SHORT)\n"
	@$(NASM) -f elf64 -o $@ $<

# ---- static programs (kernel_spawn: init) ------------------------------
$(STATIC_ELFS): $(BUILD)/user/%: $(USER_OBJDIR)/static/%.o $(USER_OBJDIR)/ulib.o $(USER_OBJDIR)/crt0.o | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  LD      $(SHORT)\n"
	@$(CC) $(USERFLAGS) $(USERLDFLAGS) -Wl,-Ttext=$(USER_LINK_BASE) -o $@ $^

$(DYN_ELFS): $(BUILD)/user/%: $(USER_OBJDIR)/pic/%.o $(USER_OBJDIR)/crt0.o $(LIBULIB) | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  LD      $(SHORT)\n"
	@$(CC) $(DYNFLAGS) $(DYNLDFLAGS) -L$(LIBDIR) -lulib \
	   -o $@ $(filter-out %.so,$^)

$(LIBULIB): $(USER_OBJDIR)/pic/ulib.o | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  LD      $(SHORT)\n"
	@$(CC) $(DYNFLAGS) -nostdlib -fuse-ld=lld -shared \
	   -Wl,--hash-style=sysv -o $@ $^

$(LDSO): $(USER_OBJDIR)/ldso/entry.o $(USER_OBJDIR)/pic/ldso/ldso.o $(USER_OBJDIR)/pic/ulib.o | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  LD      $(SHORT)\n"
	@$(CC) $(DYNFLAGS) -nostdlib -nostartfiles -fuse-ld=lld -pie \
	   -Wl,--hash-style=sysv -Wl,--dynamic-linker= -o $@ $^

$(MOD_ELFS): $(BUILD)/mod/%.km: $(USER_OBJDIR)/pic/mod/%.o $(USER_OBJDIR)/crt0.o $(LIBULIB) | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  LD      $(SHORT)\n"
	@$(CC) $(DYNFLAGS) $(DYNLDFLAGS) -L$(LIBDIR) -lulib \
	   -o $@ $(filter-out %.so,$^)

clean:
	@rm -rf $(BUILD)/usero $(BUILD)/user $(BUILD)/mod $(BUILD)/lib
	@echo Done!

-include $(USER_ALL_C:bin/%.c=$(USER_OBJDIR)/static/%.d) \
         $(USER_ALL_C:bin/%.c=$(USER_OBJDIR)/pic/%.d) \
         $(USER_OBJDIR)/ulib.d $(USER_OBJDIR)/pic/ulib.d \
         $(USER_OBJDIR)/pic/ldso/ldso.d \
         $(MOD_ALL_C:mod/%.c=$(USER_OBJDIR)/mod/%.d)
