# SPDX-License-Identifier: GPL-3.0-or-later
# FreeCUPL.  Builds the tools for the host (cc) and for Windows XP and
# later (i686-w64-mingw32).  DARKSOFT.
#   make              host tools in build/<host>/, plus the win32 tools
#                     when the cross compiler is found; the win32 build
#                     includes the front end freecuplw.exe
#   make native, make win32, make clean
#   make check        golden tests (tests/golden), output file names
#                     (tests/names) and options (tests/cli)
#   make check-win32  the same with the win32 tools under wine
#   make check-serve  tests of freecupl serve (needs curl);
#                     check-serve-win32 the same under wine
#   make test         development test harness, not in the source release
#   make FIX_DEFAULT=0  WinCUPL's bugs by default, as in 1.0 (with check
#                     and check-win32 too)

CC        ?= cc
WINCC     ?= i686-w64-mingw32-gcc
WINDRES   ?= i686-w64-mingw32-windres
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
# the front end: no console
LDFLAGS_G = -static -static-libgcc -mwindows -Wl,--subsystem,windows:5.01 \
            -Wl,--major-os-version,5 -Wl,--minor-os-version,1 \
            -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1
LIBS_G    = -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -luser32

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

.PHONY: all native win32 check check-win32 check-serve check-serve-win32 test clean
all: native $(if $(HAVE_WINCC),win32)

native: $(addprefix $(B_N)/,$(TOOLS))
win32:  $(addprefix $(B_W)/,$(addsuffix .exe,$(TOOLS))) $(B_W)/freecuplw.exe

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

# freecupl serve: the page and the user guide are compiled in, the
# reserved words of the page come from the table of src/gui/core.c
WEBSRC  = src/web/http.c src/web/api.c src/web/serve.c src/web/sys_posix.c \
          src/web/sys_win.c src/gui/core.c
WEBHDR  = src/web/http.h src/web/web.h src/web/sys.h src/gui/core.h
WEBPAGE = src/web/page/index.html src/web/page/app.css src/web/page/app.js
WEBGEN  = $(GEN)/webpage.c $(GEN)/guidev.c

$(GEN)/webgen: src/host/webgen.c src/gui/core.c src/gui/core.h $(GEN)/guidev.c
	@mkdir -p $(GEN)
	$(HOSTCC) -O1 -Isrc -o $@ src/host/webgen.c src/gui/core.c $(GEN)/guidev.c

$(GEN)/kw.js: $(GEN)/webgen
	$(GEN)/webgen -kw $@

$(GEN)/webpage.c: $(GEN)/webgen $(WEBPAGE) $(GEN)/kw.js src/gui/freecuplw.ico docs/USERGUIDE.txt
	$(GEN)/webgen $@ index_html=src/web/page/index.html app_css=src/web/page/app.css \
	    app_js=src/web/page/app.js kw_js=$(GEN)/kw.js \
	    favicon_ico=src/gui/freecuplw.ico userguide_txt=docs/USERGUIDE.txt

$(B_N)/freecupl: src/tools/freecupl.c $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(HDRS) $(WEBSRC) $(WEBHDR) $(WEBGEN)
	@mkdir -p $(B_N)
	$(CC) $(CFLAGS_N) -Isrc -o $@ $< $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(WEBSRC) $(WEBGEN) $(LDFLAGS_N)

$(B_W)/freecupl.exe: src/tools/freecupl.c $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(HDRS) $(WEBSRC) $(WEBHDR) $(WEBGEN)
	@mkdir -p $(B_W)
	$(WINCC) $(CFLAGS_W) -Isrc -o $@ $< $(LIBSRC) $(CUPLSRC) $(GEN)/devdb.c $(WEBSRC) $(WEBGEN) $(LDFLAGS_W)

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

# freecuplw: the device table comes from the description files, the
# user guide is a resource
GUISRC = src/gui/freecuplw.c src/gui/core.c src/gui/proc.c
GUIHDR = src/gui/core.h src/gui/proc.h src/gui/resource.h src/version.h
GUIRES = src/gui/freecuplw.rc src/gui/freecuplw.manifest src/gui/freecuplw.ico \
         src/gui/tools16.bmp src/gui/tools24.bmp docs/USERGUIDE.txt

$(GEN)/guidev: src/host/guidev.c
	@mkdir -p $(GEN)
	$(HOSTCC) -O1 -o $@ $<

$(GEN)/guidev.c: $(GEN)/guidev $(DEVS) $(GEN)/devlist
	$(GEN)/guidev $@ $(DEVS)

$(B_W)/freecuplw.res.o: $(GUIRES) $(GUIHDR)
	@mkdir -p $(B_W)
	$(WINDRES) -O coff -Isrc -Isrc/gui -Idocs -o $@ src/gui/freecuplw.rc

$(B_W)/freecuplw.exe: $(GUISRC) $(GUIHDR) $(GEN)/guidev.c $(B_W)/freecuplw.res.o
	$(WINCC) $(CFLAGS_W) -Isrc -o $@ $(GUISRC) $(GEN)/guidev.c $(B_W)/freecuplw.res.o \
	    $(LDFLAGS_G) $(LIBS_G)

# FIX_DEFAULT=0: the golden default cases are run with -fix
GOLDEN_ENV = $(if $(filter 0,$(FIX_DEFAULT)),DEFAULT_ARGS=-fix)

check: native
	$(GOLDEN_ENV) sh tests/golden/run.sh $(B_N)
	sh tests/names/run.sh $(B_N)
	sh tests/cli/run.sh $(B_N)

check-win32: win32
	$(GOLDEN_ENV) RUN=wine EXE=.exe sh tests/golden/run.sh $(B_W)
	RUN=wine EXE=.exe sh tests/names/run.sh $(B_W)
	RUN=wine EXE=.exe sh tests/cli/run.sh $(B_W)

# freecupl serve: handlers without a socket, then curl against the server
WEBUNIT = tests/serve/unit.c src/web/http.c src/web/api.c src/web/sys_posix.c \
          src/web/sys_win.c src/gui/core.c

$(B_N)/serve_unit: $(WEBUNIT) $(WEBHDR) $(WEBGEN)
	@mkdir -p $(B_N)
	$(CC) $(CFLAGS_N) -Isrc -o $@ $(WEBUNIT) $(WEBGEN) $(LDFLAGS_N)

# not in build/win32: it is not one of the programs
$(GEN)/serve_unit.exe: $(WEBUNIT) $(WEBHDR) $(WEBGEN)
	$(WINCC) $(CFLAGS_W) -Isrc -o $@ $(WEBUNIT) $(WEBGEN) $(LDFLAGS_W)

check-serve: native $(B_N)/serve_unit
	@rm -rf build/check-serve && mkdir -p build/check-serve
	$(B_N)/serve_unit build/check-serve $(B_N)
	sh tests/serve/run.sh $(B_N)

check-serve-win32: win32 $(GEN)/serve_unit.exe
	@rm -rf build/check-serve-win32 && mkdir -p build/check-serve-win32
	wine $(GEN)/serve_unit.exe build/check-serve-win32 $(B_W)
	RUN=wine EXE=.exe sh tests/serve/run.sh $(B_W)

test: all
	@if [ -f tests/run_tests.sh ]; then FIX_DEFAULT='$(FIX_DEFAULT)' sh tests/run_tests.sh; \
	else echo "make test: the development harness is not part of this tree; use make check"; exit 1; fi

clean:
	rm -rf build
