<p align="center">
  <img src="docs/img/marquee.png" alt="Sega Rally 2" width="100%">
</p>

<p align="center">
  <b>Sega's 1998 arcade rally game, running natively on PC.</b><br>
  Not an emulator: the original code, recompiled to C++, with a modern Vulkan renderer.
</p>

<p align="center">
  <a href="https://www.youtube.com/@snicolast"><img src="https://img.shields.io/badge/YouTube-@snicolast-red?logo=youtube&logoColor=white" alt="YouTube: snicolast"></a>
  <a href="https://buymeacoffee.com/snicolast8u"><img src="https://img.shields.io/badge/Buy_me_a_coffee-snicolast8u-FFDD00?logo=buymeacoffee&logoColor=black" alt="Buy me a coffee"></a>
</p>

<p align="center">
  <img src="docs/img/village.jpg" alt="Through a mountain village in true widescreen, with ray-traced sun shadows" width="100%">
</p>

## What you get

- **True widescreen**, not stretched, with the HUD at the corners.
- **Ray tracing**: shadows, global illumination and reflections on the cars.
- **Real dust**: billowing clouds and flying stones instead of flat sprites.
- **HD liveries and HUD**, redrawn for this port.
- **Sharp and smooth**: NVIDIA DLAA and DLSS, or MSAA, and 120 / 144 Hz and above.
- **Arcade sound**, with surround and headphone modes.
- **Keyboard, gamepad or wheel** (wheel experimental).
- **Achievements and statistics**: 14 achievements, your best laps and times per course, in RECORDS.

<table>
  <tr>
    <td><img src="docs/img/forest.jpg" alt="The forest road of the mountain stage"></td>
    <td><img src="docs/img/snow.jpg" alt="Snow thrown up by the wheels on the snowy stage"></td>
  </tr>
</table>

<p align="center">
  <img src="docs/img/menu.jpg" alt="Main menu" width="100%">
</p>

## ⚠️ ROM not included

You need your own **`srally2.zip`** (Sega Model 3 Step 2.0, files like `epr-20632.17`, with
`epr-20512.bin`, the force feedback board). Nothing from it is in this repository, and other
versions will not work. `build.bat` says which chips are missing.

## Install

You need Windows 10/11 (64-bit) and a GPU with a Vulkan 1.3 driver (most from 2016 on). Ray
tracing needs an NVIDIA RTX or an AMD RX 6000 or newer; DLAA and DLSS, an NVIDIA RTX. On other
cards those options switch themselves off, and the game builds the same on any GPU.

The easy way is **[NicoLauncher](https://github.com/snicolast/NicoLauncher-Hub)**, a free app that
downloads, builds, updates and starts the game for you. No Visual Studio, nothing to set up by hand.

1. Download **[NicoLauncher-setup.exe](https://github.com/snicolast/NicoLauncher-Hub/releases/latest)** and run it.
2. In the launcher, open **Sega Rally 2**, press **Install** and choose your `srally2.zip` when it asks.
3. Press **Play**.

<details>
<summary>Without the launcher: install.bat</summary>

1. Download this repository (green **Code** button > **Download ZIP**) and unzip it.
2. Put `srally2.zip` in the folder.
3. Double-click `install.bat`. It installs Python 3 and the Visual Studio 2022 C++ build tools
   with winget (a few GB, accept the Windows prompt), then builds the game.
4. Play with `SegaRally2.exe`.

</details>

<details>
<summary>Manual install</summary>

Install [Python 3.10 or later](https://www.python.org/downloads/) and
[Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)
with the *Desktop development with C++* workload. The full workload is over 6 GB; the build only
needs the MSVC compiler, CMake and a Windows SDK. In a command prompt:

```
winget install Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
```

Or in the Visual Studio Installer, choose *Individual components* instead of the workload and
tick *MSVC v143 - VS 2022 C++ x64/x86 build tools (Latest)*, *C++ CMake tools for Windows*
and one *Windows 11 SDK* (it works on Windows 10 too).

Then run `build.bat` with `srally2.zip` next to it (or pass its path: `build.bat D:\roms\srally2.zip`).
It downloads SDL3, miniz and NVIDIA DLSS on the first run. DLSS is optional: if that download
fails, the game is built without it and uses MSAA.
</details>

If the build can't download SDL3 or miniz (a proxy, an antivirus, a flaky connection), it lists
the links: download them with your browser, put them in `third_party\downloads\` keeping the file
name and run `build.bat` again.

### Update

With NicoLauncher: it tells you when there is something new. Press **Update**.

Without it: when there is a new version, the main menu says what is new and offers **UPDATE NOW**. Or run
`update.bat` yourself: it downloads the changed files from GitHub (no git needed) and rebuilds.
Your ROM, settings and textures are kept; files you edited are backed up to `out\update_backup\`.

## Controls

| | Keyboard | Gamepad |
|---|---|---|
| Steer | Left / Right | Left stick |
| Accelerator / brake | Up / Down | RT / LT (or A / B) |
| Shift up / down | A / Z | RB / LB |
| VIEW CHANGE | V | Y |
| Hand brake | Space | X |
| Coin + start | 5, then 1 | BACK, then START |
| Pause | Esc | START |

F11 or Alt+Enter toggles fullscreen. Everything else is in **Options**. In the selection screens
"confirm with the pedal" means the accelerator. The gamepad rumbles with the wheel's force feedback.

**Steering wheels (experimental)** are read through SDL, with force feedback. This has never been
tested on a real wheel: the settings are in `save\wheel.ini`, and `set SR2_WHEEL_LOG=1` prints
every axis and button you move. If you have one, please tell me how it goes in an issue.

## Performance

OPTIONS > DISPLAY > **QUALITY** sets the costly effects together, from LOW (laptops) to ULTRA.
On an RTX, **ANTI-ALIASING: DLSS QUALITY** draws the 3D at two thirds of the size and rebuilds it
sharp, about 30% lighter on the GPU than DLAA: the one to try first at 1440p, 4K or 120 Hz and above.

## More

- `SegaRally2.exe --help` lists the command-line options (`--aspect 21:9`, `--hud-centre`...).
- Textures of your own: `--dump-textures` writes the game's to `out\textures\`; a redrawn one
  in `textures\` with the same name replaces it.
- [How it works](docs/ARCHITECTURE.md): the PowerPC to C++ recompiler and the hardware.
- Other recompiled arcade games: [The Lost World](https://github.com/snicolast/LostWorldNative)
  and [Star Wars Trilogy](https://github.com/snicolast/StarWarsTrilogyNative).

## Support

If you enjoy the port and want to help with the time and tools behind it, you can buy me a coffee.
Entirely optional: it supports the work on this project, not the game, which belongs to Sega.

<a href="https://buymeacoffee.com/snicolast8u"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me a Coffee" height="50"></a>

## License

The code in this repository is under the [MIT License, with additional terms](LICENSE). The libraries and fonts in
`third_party/` keep their own licenses. The gamepad button pictures in `res/pad/` are
[Controller Input Icons](https://opengameart.org/content/controller-input-icons) by ElDuderino (CC0).
The game itself (ROM, code, data, music and voices) is not included and stays the property of its
owners: you need your own copy of `srally2.zip`.

**No prebuilt downloads.** The license does not allow anyone to share a compiled build of this
port, a ROM, or anything made from one. I have never published any of them: everyone builds the
game themselves from their own `srally2.zip`. Any ready-to-play download you find elsewhere is
not from me, breaks the license, and is at your own risk.

<sub>A fan project for personal use, not affiliated with or endorsed by Sega or by any of the car
makers whose cars appear in the game. Sega Rally is a trademark of Sega; the car names and
liveries belong to their owners.</sub>
