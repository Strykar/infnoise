CFLAGS ?= -O2 -Wall -Wextra
SOFTWARE = ../../software
SAMPLES = ../samples

# healthcheck.c references no libftdi symbol, but libinfnoise.h embeds
# struct ftdi_context in struct infnoise_context, so the headers are needed.
FTDI_CFLAGS := $(shell pkg-config --cflags libftdi1 2>/dev/null)

# Same byte count as the committed sample, so the control is a fair comparison.
UNIFORM_BYTES = 1216512

all: replay

replay: replay.c $(SOFTWARE)/healthcheck.c
	$(CC) $(CFLAGS) -I$(SOFTWARE) $(FTDI_CFLAGS) -o $@ $^ -lm

check: replay
	./replay --expect pass $(SAMPLES)/infnoise_raw.bin
	./replay --expect fail --uniform $(UNIFORM_BYTES)
	./replay --expect fail $(SAMPLES)/infnoise_whitened.bin

clean:
	rm -f replay

.PHONY: all check clean
