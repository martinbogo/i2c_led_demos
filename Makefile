# Default compiler (dynamic drop-in replacement by Docker)
CC ?= gcc
PYTHON3 ?= python3
COMMON_WARNINGS = -Wall -Wno-implicit-function-declaration
CFLAGS = -O2 $(COMMON_WARNINGS)
LDFLAGS =
LDLIBS = -lm -lz
OLED_LUT_HEADER = oled_build_lut.h
OLED_CALIBRATION_FILE ?= oled_gamma_calibration.txt

SIZE_CFLAGS = -Os -DNDEBUG $(COMMON_WARNINGS) -ffunction-sections -fdata-sections -fomit-frame-pointer \
	-fno-unwind-tables -fno-asynchronous-unwind-tables
SIZE_EXTRA_CFLAGS = -flto -fno-stack-protector -fno-ident
SIZE_ELF_EXTRA_LDFLAGS =

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SIZE_LDFLAGS = -Wl,-dead_strip -Wl,-x
else
SIZE_LDFLAGS = -Wl,--gc-sections -Wl,-s
SIZE_ELF_EXTRA_LDFLAGS = -Wl,--build-id=none -Wl,-z,norelro
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

.PHONY: all clean FORCE

i2c_oled_demo: CFLAGS := $(SIZE_CFLAGS)
i2c_oled_demo: LDFLAGS += $(SIZE_LDFLAGS)

elevated: CFLAGS := $(SIZE_CFLAGS) $(SIZE_EXTRA_CFLAGS)
elevated: LDFLAGS += $(SIZE_LDFLAGS) $(SIZE_EXTRA_LDFLAGS) $(SIZE_ELF_EXTRA_LDFLAGS)
elevated: LDLIBS := -lm
elevated: $(OLED_LUT_HEADER)

all: $(BINS)

$(DEP_DIR):
	mkdir -p $@

$(DEP_DIR)/%.d: %.c | $(DEP_DIR)
	@$(CC) $(CFLAGS) -MM -MP -MF $@ -MT $* $<

$(OLED_LUT_HEADER): /Users/martin/Development/i2c_led_demos/generate_oled_lut_header.py FORCE
	$(PYTHON3) $< --output $@ --calibration $(OLED_CALIBRATION_FILE)

# Rule to compile every individual .c file directly into its isolated binary payload
%: %.c $(DEP_DIR)/%.d
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf $(BINS) $(DEP_DIR) $(OLED_LUT_HEADER)

-include $(DEPS)
