# Makefile for the UHF reader tools.
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wshadow
CPPFLAGS = -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS   = -lm           # log10() in the RSSI calibration

LIB      = libuhf.a
LIB_OBJ  = src/serial.o src/uhf.o
BINS     = uhf_probe uhf_scan uhf_console

.PHONY: all clean
all: $(BINS)

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

uhf_probe:   src/probe.o   $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) $(LDFLAGS) $(LDLIBS)
uhf_scan:    src/scan.o    $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) $(LDFLAGS) $(LDLIBS)
uhf_console: src/console.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(LIB) $(BINS)
