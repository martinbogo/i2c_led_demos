# Default compiler (dynamic drop-in replacement by Docker)
CC ?= gcc
CXX ?= g++
PYTHON3 ?= python3
COMMON_WARNINGS = -Wall -Wno-implicit-function-declaration
CFLAGS = -O2 $(COMMON_WARNINGS)
CXXFLAGS = -O2 -Wall -std=c++17
LDFLAGS =
LDLIBS = -lm -lz
CPP_LDLIBS = -lm -lz -lpthread
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

KOI_POND_LIB_STATIC = libkoi_pond.a
KOI_POND_LIB_SHARED = libkoi_pond.so
KOI_POND_BINS = koi_pond_static koi_pond_dynamic
KOI_POND_CORE_OBJS = koi_pond_port.o hal_gpio_spi.o
KOI_POND_MAIN_OBJ = koi_pond_main.o

ALL_BINS = $(PI_BINS) $(UNOQ_BINS) badapple_waveshare $(KOI_POND_LIB_STATIC) $(KOI_POND_LIB_SHARED) $(KOI_POND_BINS)

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

koi_pond_port.o: CXXFLAGS += -fPIC
koi_pond_port.o: koi_pond_port.cpp koi_pond_port.h koi_pond_assets.h gpio_config.h hal_gpio_spi.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

koi_pond_main.o: koi_pond_main.cpp koi_pond_port.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

lcd_gc9a01.o: lcd_gc9a01.c lcd_gc9a01.h gpio_config.h hal_gpio_spi.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Waveshare Bad Apple Demo
badapple_waveshare: hal_gpio_spi.o lcd_gc9a01.o badapple_waveshare.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

badapple_waveshare.o: badapple_waveshare.c lcd_gc9a01.h gpio_config.h hal_gpio_spi.h
	$(CC) $(CFLAGS) -c -o $@ $<

all: pi unoq

pi: $(PI_BINS) badapple_waveshare $(KOI_POND_LIB_STATIC) $(KOI_POND_LIB_SHARED) $(KOI_POND_BINS)

unoq: $(UNOQ_BINS)

$(KOI_POND_LIB_STATIC): $(KOI_POND_CORE_OBJS)
	$(AR) rcs $@ $^

$(KOI_POND_LIB_SHARED): $(KOI_POND_CORE_OBJS)
	$(CXX) -shared $(LDFLAGS) -o $@ $^ $(CPP_LDLIBS)

koi_pond_static: $(KOI_POND_MAIN_OBJ) $(KOI_POND_LIB_STATIC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(KOI_POND_MAIN_OBJ) -L. -lkoi_pond $(CPP_LDLIBS)

koi_pond_dynamic: $(KOI_POND_MAIN_OBJ) $(KOI_POND_LIB_SHARED)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(KOI_POND_MAIN_OBJ) -L. -lkoi_pond -Wl,-rpath,'$$ORIGIN' $(CPP_LDLIBS)

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
