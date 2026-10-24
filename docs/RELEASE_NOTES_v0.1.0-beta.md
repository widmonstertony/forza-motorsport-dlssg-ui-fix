# Forza Motorsport DLSSG UI Fix v0.1.0-beta

First public beta of the frame-token synchronized HUDless/UI path.

### Highlights

- Fixes the obvious multi-copy trail on the lower-right tachometer in the
  tested Motorsport build.
- Synchronizes scene, depth, motion, HUDless and UI tags to one frame token.
- Preserves native-resolution UI recomposition with fixed 4x DLSS MFG.
- Uses game-local files only; no global NVIDIA/NGX settings are changed.
- Includes automatic backup, install and uninstall scripts.
- Includes reproducible source and SHA-256 checksums.

### Tested configuration

- Forza Motorsport 1.859.7102.0
- RTX 4090 Laptop / Windows 11 / 4K
- Streamline 2.14.1
- DLSS-G 310.9.16
- Fixed 4x MFG

This is an experimental beta. Read the requirements and known limits in the
README before installing.

