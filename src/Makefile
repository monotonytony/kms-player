# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thony <monotonytony@users.noreply.github.com>
#
# Build every component of kms-player.
#
# Only the C helpers are compiled; kms-play-py is a Python 3 program that runs
# straight from the tree. `make` produces binaries next to the sources so that
# install.sh can pick them up without knowing about object directories.
#
# The libdrm include path is probed because some Armbian images ship libdrm
# without a usable pkg-config file; in that case we fall back to the standard
# include directory.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PYTHON  ?= python3
PREFIX  ?= /usr/local

# libdrm is needed by kms-subd and kms-planes (plane/master programming) and by
# drm-plane-formats. kms-keys and kms-inject only talk to the input subsystem
# and the uinput device, so they need no extra library.
LIBDRM_CFLAGS := $(shell pkg-config --cflags libdrm 2>/dev/null || echo -I/usr/include/libdrm)
LIBDRM_LIBS   := $(shell pkg-config --libs libdrm 2>/dev/null || echo -ldrm)

BINS := kms-subd kms-keys kms-planes kms-inject kms-keywait drm-plane-formats
SHIM := libkmsshare.so

.PHONY: all clean check install

all: $(BINS) $(SHIM)
	@$(PYTHON) -m py_compile kms-play-py && echo "kms-play-py: syntax OK"

# DRM master owner: drives the subtitle and OSD planes.
# plane-select.h holds the shared selection algorithm included by both.
kms-subd: kms-subd.c plane-select.h
	$(CC) $(CFLAGS) -o $@ $< $(LIBDRM_CFLAGS) $(LIBDRM_LIBS)

# Keyboard daemon: one poll() loop over every input device.
kms-keys: kms-keys.c
	$(CC) $(CFLAGS) -o $@ $<

# Diagnostic: dump plane capabilities and current formats.
kms-planes: kms-planes.c plane-select.h
	$(CC) $(CFLAGS) -o $@ $< $(LIBDRM_CFLAGS) $(LIBDRM_LIBS)

# Diagnostic: synthetic uinput keyboard used to measure input latency.
kms-inject: kms-inject.c
	$(CC) $(CFLAGS) -o $@ $<

# Legacy watcher that only waits for ESC/Q; kept because the first version of
# the player still uses it and some scripts refer to it.
kms-keywait: kms-keywait.c
	$(CC) $(CFLAGS) -o $@ $<

drm-plane-formats: drm-plane-formats.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBDRM_CFLAGS) $(LIBDRM_LIBS)

# LD_PRELOAD shim: lends the DRM master to kmssink over a Unix socket.
# -fPIC and -shared are mandatory; -ldl for dlsym(RTLD_NEXT).
$(SHIM): kmsshare.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -ldl

check:
	@for f in kms-subd.c kms-keys.c kms-planes.c kms-inject.c kmsshare.c plane-select.h; do \
		grep -q "SPDX-License-Identifier" $$f || { echo "missing SPDX header: $$f"; exit 1; }; \
	done
	@echo "license headers: OK"

install: all
	./install.sh
	install -m 0755 $(SHIM) $(DESTDIR)/usr/local/lib/$(SHIM)

clean:
	rm -f $(BINS) $(SHIM) *.o
	rm -rf __pycache__
