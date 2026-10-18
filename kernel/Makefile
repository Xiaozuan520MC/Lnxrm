KERN_SRC_C  := $(shell find kernel -name '*.c' 2>/dev/null)
KTEST_SRC_C := $(if $(KTEST_DIR),$(shell find $(KTEST_DIR) -name '*.c' 2>/dev/null))

KERN_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(KERN_SRC_C))
KTEST_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(KTEST_SRC_C))

$(BUILD)/%.o: %.c | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CC      $@\n"
	@$(CC) $(KERNFLAGS) -MMD -MP -c -o $@ $<
