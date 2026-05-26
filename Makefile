CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2
AR ?= ar

all: libgrexie_signals_client.a

libgrexie_signals_client.a: src/grexie_signals_client.o
	$(AR) rcs $@ $<

src/grexie_signals_client.o: src/grexie_signals_client.c include/grexie_signals_client.h
	$(CC) $(CFLAGS) -Iinclude -c src/grexie_signals_client.c -o $@

test: libgrexie_signals_client.a tests/test_client.c
	$(CC) $(CFLAGS) -Iinclude tests/test_client.c libgrexie_signals_client.a -lm -o tests/test_client
	./tests/test_client

clean:
	rm -f src/*.o libgrexie_signals_client.a tests/test_client

.PHONY: all test clean
