# Default compiler (dynamic drop-in replacement by Docker)
CC ?= gcc
PYTHON3 ?= python3
COMMON_WARNINGS = -Wall -Wno-implicit-function-declaration
CFLAGS = -O2 $(COMMON_WARNINGS)
LDFLAGS =
LDLIBS = -lm -lz
OLED_LUT_GENERATOR = generate_oled_lut_header.py
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

# Identify all loose C system modules (excluding special targets and libraries without main)
PI_SRCS = $(filter-out badapple_waveshare.c hal_gpio_spi.c lcd_gc9a01.c, $(wildcard *.c))
PI_BINS = $(PI_SRCS:.c=)

# Uno Q Host payloads
UNOQ_SRCS = unoq_st_dashboard/host/st_dashboard_stream.c unoq_st_smartwatch/host/st_smartwatch_stream.c
UNOQ_BINS = $(UNOQ_SRCS:.c=)

ALL_BINS = $(PI_BINS) $(UNOQ_BINS) badapple_waveshare

# Auto-generate matching target binaries
DEP_DIR = .deps

ifeq ($(strip $(MAKECMDGOALS)),)
DEP_TARGETS = $(ALL_BINS)
else ifneq ($(filter clean,$(MAKECMDGOALS)),)
DEP_TARGETS =
else
DEP_TARGETS = $(filter $(ALL_BINS),$(MAKECMDGOALS))
endif

DEPS = $(patsubst %,$(DEP_DIR)/%.d,$(DEP_TARGETS))

.DEFAULT_GOAL := all
.PHONY: all pi unoq clean FORCE

i2c_oled_demo: CFLAGS := $(SIZE_CFLAGS)
i2c_oled_demo: LDFLAGS += $(SIZE_LDFLAGS)

elevated: CFLAGS := $(SIZE_CFLAGS) $(SIZE_EXTRA_CFLAGS)
elevated: LDFLAGS += $(SIZE_LDFLAGS) $(SIZE_EXTRA_LDFLAGS) $(SIZE_ELF_EXTRA_LDFLAGS)
elevated: LDLIBS := -lm
elevated: $(OLED_LUT_HEADER)

hal_gpio_spi.o: hal_gpio_spi.c hal_gpio_spi.h
	$(CC) $(CFLAGS) -c -o $@ $<

lcd_gc9a01.o: lcd_gc9a01.c lcd_gc9a01.h gpio_config.h hal_gpio_spi.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Waveshare Bad Apple Demo
badapple_waveshare: hal_gpio_spi.o lcd_gc9a01.o badapple_waveshare.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

badapple_waveshare.o: badapple_waveshare.c lcd_gc9a01.h gpio_config.h hal_gpio_spi.h
	$(CC) $(CFLAGS) -c -o $@ $<

all: pi unoq

pi: $(PI_BINS) badapple_waveshare

unoq: $(UNOQ_BINS)

$(DEP_DIR):
	mkdir -p $@

$(DEP_DIR)/%.d: %.c | $(DEP_DIR)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MM -MP -MF $@ -MT $* $<

$(OLED_LUT_HEADER): $(OLED_LUT_GENERATOR) FORCE
	$(PYTHON3) $< --output $@ --calibration $(OLED_CALIBRATION_FILE)

# Rule to compile every individual .c file directly into its isolated binary payload
%: %.c $(DEP_DIR)/%.d
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

unoq_st_dashboard/host/%: unoq_st_dashboard/host/%.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

unoq_st_smartwatch/host/%: unoq_st_smartwatch/host/%.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf $(ALL_BINS) $(DEP_DIR) $(OLED_LUT_HEADER) *.o badapple_waveshare

-include $(DEPS)
