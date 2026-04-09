# Default compiler (dynamic drop-in replacement by Docker)
CC ?= gcc
COMMON_WARNINGS = -Wall -Wno-implicit-function-declaration
CFLAGS = -O2 $(COMMON_WARNINGS)
LDFLAGS =
LDLIBS = -lm -lz

SIZE_CFLAGS = -Os -DNDEBUG $(COMMON_WARNINGS) -ffunction-sections -fdata-sections -fomit-frame-pointer \
	-fno-unwind-tables -fno-asynchronous-unwind-tables
SIZE_EXTRA_CFLAGS = -flto -fno-stack-protector -fno-ident

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SIZE_LDFLAGS = -Wl,-dead_strip -Wl,-x
else
SIZE_LDFLAGS = -Wl,--gc-sections -Wl,-s
endif

SIZE_EXTRA_LDFLAGS = -flto

# Identify all loose C system modules
SRCS = $(wildcard *.c)

# Auto-generate matching target binaries
BINS = $(SRCS:.c=)
DEP_DIR = .deps

ifeq ($(strip $(MAKECMDGOALS)),)
DEP_TARGETS = $(BINS)
else ifneq ($(filter clean,$(MAKECMDGOALS)),)
DEP_TARGETS =
else
DEP_TARGETS = $(filter $(BINS),$(MAKECMDGOALS))
endif

DEPS = $(patsubst %,$(DEP_DIR)/%.d,$(DEP_TARGETS))

.PHONY: all clean

i2c_oled_demo: CFLAGS := $(SIZE_CFLAGS)
i2c_oled_demo: LDFLAGS += $(SIZE_LDFLAGS)

elevated: CFLAGS := $(SIZE_CFLAGS) $(SIZE_EXTRA_CFLAGS)
elevated: LDFLAGS += $(SIZE_LDFLAGS) $(SIZE_EXTRA_LDFLAGS)
elevated: LDLIBS := -lm

all: $(BINS)

$(DEP_DIR):
	mkdir -p $@

$(DEP_DIR)/%.d: %.c | $(DEP_DIR)
	@$(CC) $(CFLAGS) -MM -MP -MF $@ -MT $* $<

# Rule to compile every individual .c file directly into its isolated binary payload
%: %.c $(DEP_DIR)/%.d
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf $(BINS) $(DEP_DIR)

-include $(DEPS)
