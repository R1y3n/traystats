CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDLIBS ?= -lX11 -lm

.PHONY: all clean

all: traystats

traystats: traystats.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f traystats
