CC = gcc
CFLAGS = -g -Wall -Wpedantic -std=c17 -O2
LDFLAGS =
LDLIBS = -lm

http-lat-bench:

# The Makefile dependency is kind of bogus, but I include it because we use the
# Makefile to configure compile-time settings like CONFIG_URI_ROUND_ROBIN.
http-lat-bench.o: Makefile

.PHONY: clean
clean:
	rm -f *.o a.out core http-lat-bench

.PHONY: all
all: clean http-lat-bench
