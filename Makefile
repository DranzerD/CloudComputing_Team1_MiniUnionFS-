################################################################################
# Makefile — Mini-UnionFS
#
# Targets:
#   make            — build the binary
#   make debug      — build with AddressSanitizer + debug symbols
#   make clean      — remove build artefacts
#   make test       — run the automated test suite (requires FUSE & root perms)
#   make install    — copy binary to /usr/local/bin
#   make uninstall  — remove from /usr/local/bin
################################################################################

CC       := gcc
BINARY   := mini_unionfs
SRC      := mini_unionfs.c

# pkg-config is the canonical way to get FUSE 3 flags
PKG      := fuse3

CFLAGS   := -Wall -Wextra -Wpedantic -O2 -std=gnu17 \
            $(shell pkg-config --cflags $(PKG))
LDFLAGS  := $(shell pkg-config --libs   $(PKG))

# Debug build flags
DBGFLAGS := -g3 -O0 -fsanitize=address,undefined -DDEBUG

.PHONY: all debug clean test install uninstall

all: $(BINARY)

$(BINARY): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

debug: $(SRC)
	$(CC) $(CFLAGS) $(DBGFLAGS) -o $(BINARY)_debug $< $(LDFLAGS)
	@echo "Debug binary: ./$(BINARY)_debug"

clean:
	rm -f $(BINARY) $(BINARY)_debug *.o
	rm -rf unionfs_test_env /tmp/mini_unionfs.log

test: all
	@echo "Running test suite..."
	bash test_unionfs.sh

install: all
	install -m 755 $(BINARY) /usr/local/bin/$(BINARY)
	@echo "Installed to /usr/local/bin/$(BINARY)"

uninstall:
	rm -f /usr/local/bin/$(BINARY)
