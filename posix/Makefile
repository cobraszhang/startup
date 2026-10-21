# posix/Makefile - POSIX build of the boot-time application launcher.
# Layout: platform-specific sources live in this directory, the shared
# C89 core in ../src, compiler output in ./build/{debug,release},
# release package in ./dist/bin. Requires a C89 compiler (cc/gcc/clang).
#
#   make            -> build/release/bootlauncher  (-O2 -DNDEBUG)
#   make debug      -> build/debug/bootlauncher    (-g -O0 -D_DEBUG)
#   make all        -> both

CC      ?= cc
CFLAGS  ?= -std=c89 -pedantic -Wall -Wextra

CORE = ../src
BLD  = build

# configuration selection (make / make MODE=debug / make debug)
MODE ?= release
ifeq ($(MODE),debug)
  OBJDIR    = $(BLD)/debug
  MODEFLAGS = -g -O0 -D_DEBUG
else
  OBJDIR    = $(BLD)/release
  MODEFLAGS = -O2 -DNDEBUG
endif

# GTK+ GUI is built in automatically when the development files exist
ifeq ($(shell pkg-config --exists gtk+-3.0 && echo yes),yes)
  # pkg-config emits -I; rewrite to -isystem so GTK/GLib headers cannot
  # produce warnings under -std=c89 -pedantic (our own code stays strict).
  GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 | sed 's/-I/-isystem /g')
  GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)
  GUI_OBJS   := $(OBJDIR)/gui_gtk.o
  DEFS       := -DHAVE_GUI
  $(info build: $(MODE), GTK+ GUI enabled)
else
  $(info build: $(MODE), gtk+-3.0 not found, compiling WITHOUT the GUI)
endif

ALL_CFLAGS = $(CFLAGS) $(MODEFLAGS) $(DEFS) $(GTK_CFLAGS)

OBJS = $(OBJDIR)/cfg.o $(OBJDIR)/topo.o $(OBJDIR)/log.o \
       $(OBJDIR)/cli.o $(OBJDIR)/posix_main.o $(GUI_OBJS)

HDRS = $(CORE)/cfg.h $(CORE)/topo.h $(CORE)/log.h $(CORE)/cli.h \
       $(CORE)/version.h

all: release debug

release:
	@$(MAKE) MODE=release bootlauncher

debug:
	@$(MAKE) MODE=debug bootlauncher

bootlauncher: $(OBJS)
	$(CC) $(ALL_CFLAGS) -o $(OBJDIR)/bootlauncher $(OBJS) $(GTK_LIBS)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(CORE)/%.c $(HDRS) | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -I$(CORE) -c -o $@ $<

$(OBJDIR)/posix_main.o: posix_main.c posix_app.h $(HDRS) | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -I$(CORE) -c -o $@ $<

$(OBJDIR)/gui_gtk.o: gui_gtk.c posix_app.h $(HDRS) | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -I$(CORE) -c -o $@ $<

# install the launcher and a default configuration
# (the binary resolves its config as <exe dir>/../conf/startup.conf,
#  so /usr/local/bin/bootlauncher reads /usr/local/conf/startup.conf)
install:
	@$(MAKE) MODE=release bootlauncher
	install -m 755 $(BLD)/release/bootlauncher /usr/local/bin/bootlauncher
	mkdir -p /usr/local/conf
	install -m 644 conf/startup.conf /usr/local/conf/startup.conf

# release package: posix/dist/{bin/bootlauncher, conf/startup.conf}
package:
	@$(MAKE) MODE=release bootlauncher
	mkdir -p dist/bin dist/conf
	cp -f $(BLD)/release/bootlauncher dist/bin/bootlauncher
	cp -f conf/startup.conf dist/conf/startup.conf
	cp -f ../README.md dist/README.md
	@echo "package ready: posix/dist  (bin/bootlauncher, conf/startup.conf)"

clean:
	rm -rf $(BLD)

.PHONY: all release debug install package clean
