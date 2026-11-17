# SPDX-License-Identifier: GPL-3.0-or-later
# FreeCUPL.  Builds the tools for the host (cc) and for Windows XP and
# later (i686-w64-mingw32).  DARKSOFT.
#   make              host tools in build/<host>/, plus the win32 tools
#                     when the cross compiler is found
#   make native, make win32, make clean
#   make check        golden tests (tests/golden)
#   make check-win32  the same with the win32 tools under wine
#   make test         development test harness, not in the source release
#   make FIX_DEFAULT=0  WinCUPL's bugs by default, as in 1.0 (with check
#                     and check-win32 too)

CC        ?= cc
WINCC     ?= i686-w64-mingw32-gcc
HOSTCC    ?= $(CC)
CSTD      = -std=c99 -pedantic
WARN      = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
OPT       = -O2
# extra link flags for the host tools, e.g. LDFLAGS_N=-static
LDFLAGS_N ?=
CFLAGS_N  = $(CSTD) $(WARN) $(OPT)
# Windows XP SP3: msvcrt, static runtime, PE subsystem 5.01, no newer APIs.
CFLAGS_W  = $(CSTD) $(WARN) $(OPT) -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
            -D__USE_MINGW_ANSI_STDIO=1 -D__MSVCRT_VERSION__=0x0600
LDFLAGS_W = -static -static-libgcc -Wl,--subsystem,console:5.01 \
            -Wl,--major-os-version,5 -Wl,--minor-os-version,1 \
            -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1

UNAME    := $(shell uname -s 2>/dev/null | tr A-Z a-z)
ifeq ($(UNAME),darwin)
  NATIVE = macos
else
  NATIVE = linux
endif
B_N  = build/$(NATIVE)
B_W  = build/win32
GEN  = build/gen

LIBSRC = src/util.c src/bugs.c src/jed.c src/device.c src/sim.c src/sim3.c src/devload.c
CUPLSRC = $(sort $(wildcard src/cupl/*.c)) $(sort $(wildcard src/csim/*.c))
HDRS   = $(wildcard src/*.h) $(wildcard src/cupl/*.h) $(wildcard src/csim/*.h) build/fixdefault
# FIX_DEFAULT=0 builds the 1.0 behaviour: WinCUPL's bugs by default.
FIX_DEFAULT ?=
ifneq ($(FIX_DEFAULT),)
CFLAGS_N += -DFREECUPL_FIX_DEFAULT=$(FIX_DEFAULT)
CFLAGS_W += -DFREECUPL_FIX_DEFAULT=$(FIX_DEFAULT)
endif
# rebuild when FIX_DEFAULT changes
$(shell mkdir -p build && printf '%s\n' '$(FIX_DEFAULT)' > build/fixdefault.new && \
  { cmp -s build/fixdefault.new build/fixdefault || cp build/fixdefault.new build/fixdefault; }; \
  rm -f build/fixdefault.new)
DEVS   = $(sort $(wildcard devices/*.dev))
TOOLS  = freecupl freecsim jedsim equiv jedtool
HAVE_WINCC := $(shell command -v $(WINCC) >/dev/null 2>&1 && echo yes)

.PHONY: all native win32 check check-win32 test clean
all: native $(if $(HAVE_WINCC),win32)

native: $(addprefix $(B_N)/,$(TOOLS))
win32:  $(addprefix $(B_W)/,$(addsuffix .exe,$(TOOLS)))

$(GEN)/embed: src/host/embed.c
	@mkdir -p $(GEN)
	$(HOSTCC) -O1 -o $@ $<

# rewritten only when a description file is added or removed, so that
# devdb.c is regenerated in both cases
$(GEN)/devlist: FORCE
	@mkdir -p $(GEN)
	@echo "$(DEVS)" > $@.tmp; if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

FORCE:

$(GEN)/devdb.c: $(GEN)/embed $(DEVS) $(GEN)/devlist
	$(GEN)/embed $@ $(DEVS)

$(B_N)/freecupl: src/tools/freecupl.c $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(HDRS)
	@mkdir -p $(B_N)
	$(CC) $(CFLAGS_N) -Isrc -o $@ $< $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(LDFLAGS_N)

$(B_W)/freecupl.exe: src/tools/freecupl.c $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(HDRS)
	@mkdir -p $(B_W)
	$(WINCC) $(CFLAGS_W) -Isrc -o $@ $< $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(LDFLAGS_W)

$(B_N)/freecsim: src/tools/freecsim.c $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(HDRS)
	@mkdir -p $(B_N)
	$(CC) $(CFLAGS_N) -Isrc -o $@ $< $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(LDFLAGS_N)

$(B_W)/freecsim.exe: src/tools/freecsim.c $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(HDRS)
	@mkdir -p $(B_W)
	$(WINCC) $(CFLAGS_W) -Isrc -o $@ $< $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(LDFLAGS_W)

$(B_N)/%: src/tools/%.c $(LIBSRC) $(GEN)/devdb.c $(HDRS)
	@mkdir -p $(B_N)
	$(CC) $(CFLAGS_N) -Isrc -o $@ $< $(LIBSRC) $(GEN)/devdb.c $(LDFLAGS_N)

$(B_W)/%.exe: src/tools/%.c $(LIBSRC) $(GEN)/devdb.c $(HDRS)
	@mkdir -p $(B_W)
	$(WINCC) $(CFLAGS_W) -Isrc -o $@ $< $(LIBSRC) $(GEN)/devdb.c $(LDFLAGS_W)

# FIX_DEFAULT=0: the golden default cases are run with -fix
GOLDEN_ENV = $(if $(filter 0,$(FIX_DEFAULT)),DEFAULT_ARGS=-fix)

check: native
	$(GOLDEN_ENV) sh tests/golden/run.sh $(B_N)

check-win32: win32
	$(GOLDEN_ENV) RUN=wine EXE=.exe sh tests/golden/run.sh $(B_W)

test: all
	@if [ -f tests/run_tests.sh ]; then FIX_DEFAULT='$(FIX_DEFAULT)' sh tests/run_tests.sh; \
	else echo "make test: the development harness is not part of this tree; use make check"; exit 1; fi

clean:
	rm -rf build
