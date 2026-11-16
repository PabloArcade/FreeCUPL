FreeCUPL 1.0.0

FreeCUPL is a free replacement for the command-line compiler and
simulator of WinCUPL 5.0a, the CUPL tools for simple programmable logic
devices (PAL, GAL, ATV and similar parts).  It is written from scratch
and contains no code or data from WinCUPL.

  freecupl   compiles CUPL source (.pld) into a JEDEC fuse file (.jed),
             with optional documentation, listing and macro expansion
  freecsim   runs CSIM test vectors (.si) against a compiled design
  jedsim     fuse-level simulator for JEDEC files
  equiv      checks that two JEDEC files behave the same
  jedtool    JEDEC file utilities

Command lines, file formats and messages follow WinCUPL, so existing
designs and build scripts can be used unchanged.  For every design that
WinCUPL compiles, freecupl writes a JEDEC file that makes the device
behave the same way.  This includes the known bugs of WinCUPL 5.0a,
which are reproduced by default and reported with a warning; the -fix
option gives the corrected result.  The bugs are listed in
docs/BUGS.txt.

307 device mnemonics are supported: GAL16V8, GAL20V8, 22V10, GAL18V10,
GAL20RA10, ATV750, ATF750C, ATV2500, ATF2500C, PEEL18CV8 and most PAL
families, with their PLCC variants.  The full list is in section 8 of
docs/USERGUIDE.txt.  CPLDs that need WinCUPL's external fitter are not
supported.

Platforms: Windows XP SP3 and later (32-bit), Linux, macOS.  The macOS
build is only tested by continuous integration.

Usage:

    freecupl -jxf counter.pld     JEDEC and .doc file with fuse plot
    freecupl -jas counter.pld     compile and simulate counter.si
    freecupl -a counter.pld       write the absolute file, then
    freecsim -l counter           simulate, listing in counter.so

The examples directory has small designs with simulation inputs.  The
programs are described in docs/USERGUIDE.txt and in the manual pages
man/freecupl.1 and man/freecsim.1.

Building needs a C99 compiler and make:

    make           tools in build/linux (or build/macos)
    make win32     build/win32/*.exe with i686-w64-mingw32-gcc
    make check     run the test suite in tests/golden

make also builds the Windows programs when i686-w64-mingw32-gcc is
installed.  make check compiles and simulates the designs in
tests/golden and compares the results with the stored ones; it needs
only a POSIX shell and the built tools.  The programs are
self-contained and can be copied anywhere.

Copyright (C) 2026 DARKSOFT

FreeCUPL is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version (GPL-3.0-or-later).  It is distributed WITHOUT
ANY WARRANTY; see the file LICENSE.

WinCUPL is a product of its owner.  FreeCUPL is not affiliated with it.
