# Playing Dead or Alive Xtreme 2 on Windows

This project recompiles the Xbox 360 version of Dead or Alive Xtreme 2 into a Windows program. Its releases hold the
game's code, recompiled, but none of the game's data: you need your own disc image of **Dead or Alive Xtreme 2 (USA)**,
which the game reads its files from.

## What you need

- Windows 10 or 11, 64-bit, with a processor that has AVX (from 2011 on) and a graphics card that supports Direct3D 12.
- An Xbox controller, or any XInput controller (a keyboard works too). PlayStation controllers need a tool that presents
  them as an Xbox controller, such as DS4Windows.
- Your disc image of Dead or Alive Xtreme 2 (USA), as an `.iso` file. The launcher checks that it's this version.
- About 2 GB of free disk space for the game's hard disk cache.

## Installing

1. Download the zip of the latest release from https://github.com/tarball/doax2-recomp-releases/releases and extract it
   to a folder of its own that you can change, such as one in Documents (not Program Files).
2. Start `DOAX2 Launcher.exe` in that folder. Windows may say it doesn't recognize the app, since it isn't signed:
   choose More info, then Run anyway.
3. Press Choose disc image... and choose your disc image. The launcher checks it and remembers where it is (in
   `data\settings.ini`): the disc image stays where it is, and is only read, never changed. If an `.iso` is already in
   the folder, it's found by itself.
4. Press Launch. The launcher starts the game and closes.

If something goes wrong, the launcher says what. Windows Security's controlled folder access can keep the launcher
and the game from writing to Documents: allow `DOAX2 Launcher.exe` and `doax2.exe` there, or use another folder.

## Playing

- **Settings:** OPTIONS > Video Settings in the game has the PC's options between the game's own: Fullscreen,
  Resolution (up to 4 times 1280x720), Anti-Aliasing, Alpha to Coverage and Texture Filtering. Alt+Enter also switches
  fullscreen.
- **Keyboard:** arrows or WASD move, Enter is Start, Backspace is Back, Z/J/Space is A, X/K is B, C/U is X, V/I is Y,
  Q and E are the shoulder buttons, 1 and 3 the triggers.
- **Saves and settings** are in the `data` folder next to the launcher, with the log, `doax2.log`.
- **The first run** copies 1.7 GB of the game from the disc image to its hard disk cache (`data\hdd`), as the console
  does: this takes about 7 minutes and happens in the background while you play. It also compiles the game's shaders in
  the background, which takes a minute or two: until then parts of a scene can appear a moment late. Later runs use
  both.
- **Xbox Live** is gone: XBOX LIVE on the title menu goes back to the menu, as when you back out of signing in.

## Updating

Extract the new release over the old one. Your saves, settings and the hard disk cache are in `data`, which releases
don't include, and the disc image stays where it is.

## Building from source

To build the game yourself rather than download it:

1. Install these free tools:
   - [Visual Studio 2026 Build Tools](https://visualstudio.microsoft.com/downloads/) (or any edition). In the
     installer, choose "Desktop development with C++", and under its optional components also "C++ Clang tools for
     Windows" and "C++ CMake tools for Windows".
   - [Python 3](https://www.python.org/downloads/).
   - [Git](https://git-scm.com/downloads).

   They need about 10 GB, and the build a few GB more.

2. Get this project and its recompiler side by side, in the same folder:

   ```
   git clone https://github.com/tarball/doax2-recomp-releases.git doax2-recomp
   git clone --recursive --branch doa4 https://github.com/tarball/XenonRecomp.git XenonRecomp
   ```

   That's the latest release. Each release is a tag (its date and commit, such as `v2026.10.27-e3a1ced`) whose notes
   name the XenonRecomp commit it was built with: to build an earlier one, `git checkout <tag>` in `doax2-recomp`, and
   that commit in `XenonRecomp` (`git checkout <commit>`, then `git submodule update --recursive`). The recompiler's
   `doa4` branch serves both this project and Dead or Alive 4's.

3. Run the setup from the `doax2-recomp` folder. It asks for your disc image (or pass `-Iso`):

   ```
   powershell -ExecutionPolicy Bypass -File setup.ps1
   ```

   It checks the tools, extracts and checks the game's executable, recompiles it, offers to download FFmpeg (the
   game's audio and music decoder, about 90 MB), builds the game and puts it in the `game` folder, with a shortcut. It
   takes a few minutes. `-Destination <folder>` puts the game elsewhere.

4. Play with `game\Dead or Alive Xtreme 2.lnk`.

The disc image stays where it is: the game reads it from there (its path is in `game\data\settings.ini`). If you move
it next to `doax2.exe`, the game finds it there too. To update, get the new version (`git pull` in both folders) and
run the setup again: it updates the game in place, keeping your saves, settings and the hard disk cache.
