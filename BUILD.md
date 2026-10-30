# Building Dead or Alive Xtreme 2 for Windows

You need your own Dead or Alive Xtreme 2 (USA) disc image (.iso).

## Requirements

- [Visual Studio 2026 Build Tools](https://visualstudio.microsoft.com/downloads/) (or any edition) with "Desktop
  development with C++" and its optional components "C++ Clang tools for Windows" and "C++ CMake tools for Windows"
- [Python 3](https://www.python.org/downloads/)
- [Git](https://git-scm.com/downloads)

They take about 10 GB, and the build a few GB more.

## Building

1. Clone this repository and the XenonRecomp fork side by side:

   ```
   git clone https://github.com/tarball/doax2-recomp-releases.git doax2-recomp
   git clone --recursive --branch doa4 https://github.com/tarball/XenonRecomp.git XenonRecomp
   ```

2. Run the setup from the `doax2-recomp` folder:

   ```
   powershell -ExecutionPolicy Bypass -File setup.ps1
   ```

   It asks for your disc image (or pass `-Iso <path>`), recompiles the game's code, offers to download FFmpeg (about
   90 MB), and builds the game into the `game` folder (or `-Destination <folder>`).

3. Start `game\Dead or Alive Xtreme 2.lnk`.

To update, run `git pull` in both folders and run the setup again. Your saves and settings are kept.

To build an earlier release, check out its tag in `doax2-recomp`, and in `XenonRecomp` the commit its release notes
name (then `git submodule update --recursive`).
