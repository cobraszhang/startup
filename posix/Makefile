# posix/Makefile - POSIX build of the boot-time application launcher.
# Layout: platform-specific sources live in this directory, the shared
# C89 core in ../src, compiler output in ./build, release package in
# ./dist/bin. Requires a C89 compiler (cc/gcc/clang).

CC      ?= cc
CFLAGS  ?= -std=c89 -pedantic -Wall -Wextra -O2

CORE = ../src
BLD  = build

# GTK+ GUI is built in automatically when the development files exist
ifeq ($(shell pkg-config --exists gtk+-3.0 && echo yes),yes)
  # pkg-config emits -I; rewrite to -isystem so GTK/GLib headers cannot
  # produce warnings under -std=c89 -pedantic (our own code stays strict).
  GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 | sed 's/-I/-isystem /g')
  GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)
  GUI_OBJS   := $(BLD)/gui_gtk.o
  DEFS       := -DHAVE_GUI
  $(info build: GTK+ GUI enabled)
else
  $(info build: gtk+-3.0 not found, compiling WITHOUT the GUI)
endif

ALL_CFLAGS = $(CFLAGS) $(DEFS) $(GTK_CFLAGS)

OBJS = $(BLD)/cfg.o $(BLD)/topo.o $(BLD)/log.o $(BLD)/cli.o \
       $(BLD)/posix_main.o $(GUI_OBJS)

HDRS = $(CORE)/cfg.h $(CORE)/topo.h $(CORE)/log.h $(CORE)/cli.h

all: bootlauncher

bootlauncher: $(OBJS)
	$(CC) $(ALL_CFLAGS) -o $(BLD)/bootlauncher $(OBJS) $(GTK_LIBS)

$(BLD):
	mkdir -p $(BLD)

$(BLD)/%.o: $(CORE)/%.c $(HDRS) | $(BLD)
	$(CC) $(ALL_CFLAGS) -I$(CORE) -c -o $@ $<

$(BLD)/posix_main.o: posix_main.c posix_app.h $(HDRS) | $(BLD)
	$(CC) $(ALL_CFLAGS) -I$(CORE) -c -o $@ $<

$(BLD)/gui_gtk.o: gui_gtk.c posix_app.h $(HDRS) | $(BLD)
	$(CC) $(ALL_CFLAGS) -I$(CORE) -c -o $@ $<

# install the launcher and a default configuration
# (the binary resolves its config as <exe dir>/../conf/startup.conf,
#  so /usr/local/bin/bootlauncher reads /usr/local/conf/startup.conf)
install: bootlauncher
	install -m 755 $(BLD)/bootlauncher /usr/local/bin/bootlauncher
	mkdir -p /usr/local/conf
	install -m 644 conf/startup.conf /usr/local/conf/startup.conf

# release package: posix/dist/{bin/bootlauncher, conf/startup.conf}
package: bootlauncher
	mkdir -p dist/bin dist/conf
	cp -f $(BLD)/bootlauncher dist/bin/bootlauncher
	cp -f conf/startup.conf dist/conf/startup.conf
	cp -f ../README.md dist/README.md
	@echo "package ready: posix/dist  (bin/bootlauncher, conf/startup.conf)"

clean:
	rm -rf $(BLD)

.PHONY: all install package clean
