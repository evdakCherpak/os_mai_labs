CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc

vpath %.c src
vpath %.h src

all: master slave

master: master.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

slave: slave.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

master.o slave.o common.o: common.h

clean:
	rm -f master slave *.o

.PHONY: all clean
