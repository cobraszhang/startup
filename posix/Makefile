# posix/Makefile - POSIX build of the boot-time application launcher.
# Layout: platform-specific sources live in this directory, the shared
# C89 core in ../src, compiler output in ./build, release package in
# ./dist/bin. Requires a C89 compiler (cc/gcc/clang).

CC      ?= cc
CFLAGS  ?= -std=c89 -pedantic -Wall -Wextra -O2

CORE = ../src
BLD  = build

OBJS = $(BLD)/cfg.o $(BLD)/topo.o $(BLD)/log.o $(BLD)/cli.o \
       $(BLD)/posix_main.o

HDRS = $(CORE)/cfg.h $(CORE)/topo.h $(CORE)/log.h $(CORE)/cli.h

all: bootlauncher

bootlauncher: $(OBJS)
	$(CC) $(CFLAGS) -o $(BLD)/bootlauncher $(OBJS)

$(BLD):
	mkdir -p $(BLD)

$(BLD)/%.o: $(CORE)/%.c $(HDRS) | $(BLD)
	$(CC) $(CFLAGS) -I$(CORE) -c -o $@ $<

$(BLD)/posix_main.o: posix_main.c $(HDRS) | $(BLD)
	$(CC) $(CFLAGS) -I$(CORE) -c -o $@ $<

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
