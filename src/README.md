# Source layout and build

`fm-hudless-mfg/` contains the ReShade add-on and its compute shader.
`transfusion-overrides/` contains the three modified DLSSG-Transfusion files.

## HUDless add-on

Requirements: Visual Studio 2022, CMake 3.27+, a ReShade source checkout,
NVIDIA Streamline 2.14.1 headers, and Microsoft Detours source.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DRESHade_SOURCE_DIR=C:\src\reshade `
  -DSTREAMLINE_SOURCE_DIR=C:\src\streamline `
  -DDETOURS_SOURCE_DIR=C:\src\Detours\src
cmake --build build --config Release --target fm_hudless_mfg
```

The checked-in `ui_mask_dxil.hpp` is the tested shader blob. To regenerate it,
compile `ui_mask.hlsl` with DXC for Shader Model 6 and run `bin2c.ps1`.

## Transfusion proxy

1. Check out DLSSG-Transfusion commit
   `93ae5c1603c36935c865c79a7912be31f01ae7f1`.
2. Replace the corresponding files in `source/native/` with the files under
   `transfusion-overrides/source/native/`.
3. Follow the upstream Visual Studio build instructions for `version.dll`.

The Release binary was built for x64 with MSVC 2022 in Release mode.

