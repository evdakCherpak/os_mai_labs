CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc -pthread
LDFLAGS += -pthread
LDLIBS += -lm

vpath %.c src
vpath %.h src

all: morph

morph: main.o matrix.o morph.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

main.o: matrix.h morph.h
matrix.o: matrix.h
morph.o: matrix.h morph.h

clean:
	rm -f morph *.o

.PHONY: all clean
