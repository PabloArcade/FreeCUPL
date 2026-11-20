FreeCUPL 2.3.0

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
  freecuplw  Windows only: editor and front end that runs freecupl and
             freecsim (docs/USERGUIDE.txt, section 8)

"freecupl serve" is the editor and front end for Linux and macOS: a
page in the web browser of the same machine, served by freecupl itself
on 127.0.0.1 (docs/USERGUIDE.txt, section 9).

Command lines, file formats and messages follow WinCUPL, so existing
designs and build scripts can be used unchanged.  For every design that
WinCUPL compiles, freecupl writes a JEDEC file that makes the device
behave the same way, except where WinCUPL 5.0a is wrong.  Its known
bugs are corrected by default and reported with a warning; the option
-compat-refdefect reproduces them, for exact WinCUPL 5.0a output.  The
bugs are listed in docs/BUGS.txt.  FreeCUPL 1.0.0 had the opposite
default; make FIX_DEFAULT=0 builds that behaviour.

327 device mnemonics are supported: GAL16V8, GAL20V8, 22V10, GAL18V10,
GAL20RA10, GAL20XV10, GAL26CV12, ATV750, ATF750C, ATV2500, ATF2500C,
ATV5000, PEEL18CV8 and most PAL families, with their PLCC variants.
The full list is in section 10 of docs/USERGUIDE.txt; section 6 lists
the mnemonics that are not supported (PLS/IFL, GAL6001, EP300 to EP1800
and others).  CPLDs that need WinCUPL's external fitter are not
supported.

Platforms: Windows XP SP3 and later (32-bit), Linux, macOS.  The macOS
build is only tested by continuous integration.  freecuplw is a Windows
program; Linux and macOS have the command line and freecupl serve, and
freecuplw.exe also runs there under Wine (it was tested under Wine).

macOS: the package freecupl-VERSION-macos.zip has programs for Apple
and Intel processors and FreeCUPL.command, which starts freecupl serve
with a double click.  At the first start it creates the folder FreeCUPL
in the home folder for the designs, copies the examples into it and
prints what it did.  The programs are not signed with a developer
certificate.  macOS refuses the first start of a downloaded copy: open
FreeCUPL.command with the right mouse button and Open (macOS 15: allow
it under System Settings, Privacy & Security), or run

    xattr -dr com.apple.quarantine FOLDER

on the unpacked folder.  The package and Safari were not tested on a
Mac.

Usage:

    freecupl -jxf counter.pld     JEDEC and .doc file with fuse plot
    freecupl -jas counter.pld     compile and simulate counter.si
    freecupl -a counter.pld       write the absolute file, then
    freecsim -l counter           simulate, listing in counter.so
    freecupl serve examples       editor page for the examples

The examples directory has small designs with simulation inputs.  The
programs are described in docs/USERGUIDE.txt and in the manual pages
man/freecupl.1 and man/freecsim.1.

Building needs a C99 compiler and make:

    make           tools in build/linux (or build/macos)
    make win32     build/win32/*.exe with i686-w64-mingw32-gcc and
                   windres, freecuplw.exe included
    make check     run the test suite in tests/golden
    make check-serve   test freecupl serve (needs curl)

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
