# Forza Motorsport DLSSG UI Fix

An experimental, game-local fix for DLSS Multi Frame Generation ghosting in
Forza Motorsport. It keeps the 3D scene, depth, motion vectors, HUDless image
and UI layer on the same Streamline frame token, then lets DLSS-G recompose the
native-resolution UI after interpolation.

The first public build is the exact binary pair visually tested on an RTX 4090
Laptop at 4K with 4x MFG. In the tested setup it removed the obvious trailing
copies of the lower-right tachometer and greatly stabilized cars/wheels without
changing global NVIDIA or NGX settings.

## What is in the release

- `version.dll`: a Motorsport-specific DLSSG-Transfusion build.
- `fm-hudless-mfg.addon64`: ReShade add-on that creates and tags HUDless/UI
  resources.
- `FMHUDlessCarrier.fx`: invisible carrier technique used to obtain a safe
  D3D12 command-list submission point.
- A tested local 4x profile, plus backup-aware install and uninstall scripts.

The repository also includes the complete add-on source, shader source and the
three modified Transfusion source files. It does **not** include game files,
NVIDIA DLLs/models, ReShade binaries or Microsoft Detours.

## Requirements

- Forza Motorsport (Microsoft Store/Xbox app build).
- Windows 11 and an RTX 40-series GPU with Hardware-accelerated GPU scheduling.
- A working ReShade installation with add-on support in the game directory.
- A working local DLSS/Streamline/MFG setup. The mod does not download or
  replace NVIDIA DLSS, DLSS-G or model files.

Tested on game build `1.859.7102.0`, Streamline `2.14.1`, DLSS-G `310.9.16`,
RTX 4090 Laptop, 4K output, fixed 4x MFG. Other builds may need new signatures.

## Install

1. Exit the game completely.
2. Extract the release ZIP.
3. Right-click `Install.ps1` and choose **Run with PowerShell**, or run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\Install.ps1
   ```

The default target is `C:\XboxGames\Forza Motorsport\Content`. Use `-GameDir`
if the game is elsewhere. The installer refuses to run while the game process
is active, backs up every changed file, enables the carrier technique in the
active ReShade preset, and writes an uninstall manifest.

Press **Insert** in game to open ReShade. The supplied profile requests fixed
4x MFG. DLSS/NR quality and movie-filter choices remain controlled by your
existing DLSS add-on.

## Uninstall

Exit the game, then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\Uninstall.ps1
```

The uninstaller restores the exact files saved by the latest installation.

## Why this works

Motorsport may submit its main scene resources and its late HUD/UI resources
through different Streamline tagging paths. If DLSS-G consumes those resources
under different frame identities, the UI and some moving geometry trail even
when their textures are otherwise correct. This mod:

1. captures the authoritative frame token from `slSetConstants`;
2. enables frame-based resource tagging only inside the game process;
3. promotes each fresh scene/depth/motion/HUDless/UI batch to that token; and
4. supplies a full-resolution HUDless image plus UI color/alpha resources.

No NVIDIA global profile, OTA policy or system-wide DLL is changed.

## Known limits

- This is a beta tied to a specific game and DLSS-G integration.
- Game, Streamline, DLSS-G or ReShade updates can break the hooks.
- A small amount of interpolation artifacting can still occur in unsupported
  transient overlays or when base frame rate is very low.
- Use at your own risk. Prefer offline/single-player use; injected add-ons may
  be incompatible with anti-cheat or future online policies.

## License

Original code in this repository is MIT licensed. See
`THIRD_PARTY_NOTICES.md` for upstream attribution.

