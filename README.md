# Dead or Alive Xtreme 2 for Windows

A recompilation of the Xbox 360 game Dead or Alive Xtreme 2 into a Windows program. The game's PowerPC code is
translated to C++ by [XenonRecomp](https://github.com/tarball/XenonRecomp) (a fork of it, branch `doa4`, which also
serves Dead or Alive 4's recompilation), and runs on a runtime of its own: the console's kernel and system calls, its
hard disk cache, a Direct3D 12 renderer for its graphics, and its XMA audio, movies and radio.

**None of the game's files are here, and the releases hold its code, recompiled, but none of its data** (its models,
textures, sounds and movies). You need your own disc image of Dead or Alive Xtreme 2 (USA), an `.iso` file, which the
launcher checks and remembers and the game reads its files from, never changing it.

## Playing

Download the zip of the latest release from [the releases](https://github.com/tarball/doax2-recomp-releases/releases),
extract it to a folder of its own that you can change (such as one in Documents), and start `DOAX2 Launcher.exe`.
[INSTALL.md](INSTALL.md) has what it needs (64-bit Windows 10 or 11, a processor with AVX, a graphics card with
Direct3D 12, about 2 GB for the game's hard disk cache), the controls and settings, and updating.

Xbox Live is gone: XBOX LIVE on the title menu goes back to the menu, as when you back out of signing in.

## Building from source

[INSTALL.md's Building from source](INSTALL.md#building-from-source): with Visual Studio's C++ and Clang tools, Python
and Git, `setup.ps1` checks your disc image, recompiles the game's code and builds the game.

## This repository

Each commit is a release, tagged by its date and its own short hash (`v2026.10.27-e3a1ced`), and the release of the
same name has its zip. Its notes name the XenonRecomp commit it was built with.

## Licenses

This project is free software under the GNU General Public License, version 3: see [LICENSE](LICENSE).

A release's `licenses` folder has that license and those of what it ships and what's compiled into it: XenonRecomp's
(its `ppc_context.h` is in the game's code), simde, xxHash, FFmpeg (LGPL), and Xenia's (BSD-3-Clause), whose work
parts of the runtime follow. The game's own code, recompiled, isn't covered by this project's license.

This project isn't affiliated with or endorsed by Koei Tecmo, Team NINJA or Microsoft.
