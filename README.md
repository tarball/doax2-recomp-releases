# Dead or Alive Xtreme 2 for Windows

An unofficial Windows port of the Xbox 360 version of Dead or Alive Xtreme 2, made by static recompilation with a
[fork](https://github.com/tarball/XenonRecomp) of [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

**This repository doesn't include any of the game's resources. You need your own Dead or Alive Xtreme 2 (USA) disc
image to play.**

## Requirements

- 64-bit Windows 10 or 11
- A processor with AVX (most from 2011 on)
- A graphics card with Direct3D 12
- About 2 GB of free disk space
- Your Dead or Alive Xtreme 2 (USA) disc image (.iso)

## How to install

1. Download the latest release's zip from [Releases](https://github.com/tarball/doax2-recomp-releases/releases).
2. Extract it to a folder of its own that you can write to, such as one in Documents (not Program Files).
3. Run `DOAX2 Launcher.exe`. If Windows says it doesn't recognize the app, choose More info, then Run anyway.
4. Select your Dead or Alive Xtreme 2 (USA) disc image, then press Launch.

To update, extract a new release over the old one. Your saves and settings are kept.

## Playing

- **Controls:** an Xbox controller or any XInput controller (PlayStation controllers need a tool such as DS4Windows).
  Keyboard: arrows or WASD move, Enter is Start, Backspace is Back, Z/J/Space is A, X/K is B, C/U is X, V/I is Y, Q
  and E are the shoulder buttons, 1 and 3 the triggers.
- **Graphics options:** OPTIONS > Video Settings has Fullscreen, Resolution (up to 4 times 1280x720), Anti-Aliasing,
  Alpha to Coverage and Texture Filtering. Alt+Enter toggles fullscreen.
- **First run:** the game copies 1.7 GB from the disc image to its hard disk cache in the background (about 7
  minutes) and compiles its shaders, so some scenes can appear a moment late.
- **Xbox Live** isn't available: XBOX LIVE on the title menu goes back to the menu.
- Saves, settings and the log (`doax2.log`) are in the `data` folder.

## Building

See [BUILD.md](BUILD.md).

## License

GNU General Public License, version 3: see [LICENSE](LICENSE). Releases include the licenses of the libraries they
ship. This project isn't affiliated with or endorsed by Koei Tecmo, Team NINJA or Microsoft.
