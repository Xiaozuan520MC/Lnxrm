USR_ALL_C := $(shell find usr -maxdepth 1 -name '*.c' 2>/dev/null)
UIMG      := $(patsubst usr/%.c,%,$(USR_ALL_C))

USR_OBJDIR := $(BUILD)/usro
USER_ELFS  := $(addprefix $(BUILD)/usr/,$(UIMG))

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
