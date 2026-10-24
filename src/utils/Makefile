
CC = gcc
CFLAGS = -Wall -Wstrict-prototypes -Werror

all: lpcchksum genhdr rled bootimg zx0c

genhdr: genhdr.o
	$(CC) $(CFLAGS) $^ --output $@

lpcchksum: lpcchksum.o
	$(CC) $(CFLAGS) $^ --output $@

# rled + zx0c re-pack the RLE images that are embedded in the firmware (src/Makefile)
rled: rled.o
	$(CC) $(CFLAGS) $^ --output $@

bootimg: bootimg.o
	$(CC) $(CFLAGS) $^ --output $@

bootimg.o: ../snesboot.h

# ZX0 compressor by Einar Saukas (zx0/, BSD-3, see zx0/LICENSE; the -w option added there)
ZX0_SRC = zx0/zx0.c zx0/optimize.c zx0/compress.c zx0/memory.c
zx0c: $(ZX0_SRC) zx0/zx0.h
	$(CC) -O2 $(ZX0_SRC) --output $@

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f genhdr.o genhdr
	rm -f lpcchksum.o lpcchksum
	rm -f rled.o rled bootimg.o bootimg zx0c
