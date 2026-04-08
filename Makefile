# Default compiler (dynamic drop-in replacement by Docker)
CC ?= gcc
COMMON_WARNINGS = -Wall -Wno-implicit-function-declaration
CFLAGS = -O2 $(COMMON_WARNINGS)
LDFLAGS =
LDLIBS = -lm -lz

SIZE_CFLAGS = -Os -DNDEBUG $(COMMON_WARNINGS) -ffunction-sections -fdata-sections -fomit-frame-pointer \
	-fno-unwind-tables -fno-asynchronous-unwind-tables

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SIZE_LDFLAGS = -Wl,-dead_strip -Wl,-x
else
SIZE_LDFLAGS = -Wl,--gc-sections -Wl,-s
endif

# Identify all loose C system modules
SRCS = $(wildcard *.c)

# Auto-generate matching target binaries
BINS = $(SRCS:.c=)

i2c_oled_demo: CFLAGS := $(SIZE_CFLAGS)
i2c_oled_demo: LDFLAGS += $(SIZE_LDFLAGS)

all: $(BINS)

# Rule to compile every individual .c file directly into its isolated binary payload
%: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(BINS)
