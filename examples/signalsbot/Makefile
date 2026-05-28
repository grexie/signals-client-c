CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2
ROOT := ../..
PKGS := libwebsockets sqlite3 libcurl
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS))
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS))

all: signalsbot

$(ROOT)/libgrexie_signals_client.a:
	$(MAKE) -C $(ROOT) libgrexie_signals_client.a

signalsbot: src/main.c $(ROOT)/include/grexie_signals_client.h $(ROOT)/libgrexie_signals_client.a
	$(CC) $(CFLAGS) -I$(ROOT)/include $(PKG_CFLAGS) src/main.c $(ROOT)/libgrexie_signals_client.a $(PKG_LIBS) -lm -o $@

clean:
	rm -f signalsbot

.PHONY: all clean
