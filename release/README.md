# Forza Motorsport DLSSG UI Fix

[中文](#中文说明) | [English](#english)

## 中文说明

这是一个专门给 **Forza Motorsport** 使用的实验性 DLSS Multi Frame
Generation 修复 Mod。它处理的不是普通抗锯齿模糊，而是开启 DLSS-G/MFG
以后，右下角转速表、数字、车辆轮毂和部分运动物体出现重复轮廓或拖尾的问题。

### 它是干什么的

Motorsport 的主 3D 场景和较晚绘制的 HUD/UI 可能通过不同的 Streamline
资源标记路径提交。帧生成如果把不同时间点的场景、运动矢量和 UI 当作同一帧的
输入，就会在生成帧中重复插值 HUD，或让车辆边缘、轮毂落后于真实画面。

这个 Mod 在游戏进程内完成三件事：

1. 从 `slSetConstants` 取得游戏当前帧的权威 Frame Token。
2. 把 Scene Color、Depth、Motion Vectors、HUDless Color 和 UI Color/Alpha
   统一绑定到同一个 Frame Token。
3. 生成完整分辨率的无 UI 场景与 UI 图层，让 DLSS-G 在插值后重新合成 UI。

目标渲染顺序是：

```text
真实 3D 场景
  -> DLSS 超分辨率
  -> 仅对无 UI 场景执行 DLSS-G/MFG
  -> 合成原生分辨率 UI
  -> 显示
```

所有改动都是游戏目录内的局部修改。它不会改 NVIDIA 全局配置，也不会修改
系统范围的 NGX OTA 策略。

### 实测效果

首个公开版本使用 Release 中同一对二进制文件，在以下环境完成了实际驾驶测试：

- Forza Motorsport `1.859.7102.0`
- RTX 4090 Laptop、Windows 11、4K 输出
- Streamline `2.14.1`、DLSS-G `310.9.16`
- 固定 4x MFG；同时保留 2-pass 电影风格 NR 配置

在这套环境中，之前明显出现多份影子的右下角转速表恢复为稳定、干净的单一 UI，
车辆与轮毂的拖尾也明显降低，同时保留 4x MFG 的高输出帧率。Release 中的
`TESTED_PROFILE.ini` 记录了参考参数。

效果依赖基础帧率、游戏版本、DLSS-G 版本和其他注入插件。这个 Beta 不承诺在
所有硬件和后续游戏版本上完全消除每一种插值伪影。

### Release 内容

- `version.dll`：针对 Motorsport 修改的 DLSSG-Transfusion 代理。
- `fm-hudless-mfg.addon64`：创建并提交 HUDless/UI 资源的 ReShade add-on。
- `FMHUDlessCarrier.fx`：不可见的 ReShade carrier technique，用于获得安全的
  D3D12 命令列表提交点。
- `DLSSG-Transfusion.json`：已验证的本地固定 4x 配置，公开版默认关闭高频日志。
- `Install.ps1` / `Uninstall.ps1`：带逐文件备份和还原记录的安装与卸载脚本。
- `SHA256SUMS.txt`：发布文件校验值。

仓库包含完整 add-on 源码、HLSL、生成的 DXIL 头文件和三个修改后的
DLSSG-Transfusion 源文件。仓库和 Release **不包含**游戏文件、NVIDIA DLL、
模型文件、ReShade 二进制或 Microsoft Detours。

### 使用条件

- Microsoft Store/Xbox App 版 Forza Motorsport。
- Windows 11、RTX 40 系显卡，并开启硬件加速 GPU 调度。
- 游戏目录中已有支持 add-on 的 ReShade。
- 已经可以工作的本地 DLSS/Streamline/MFG 环境。本 Mod 不下载或替换 NVIDIA
  的 DLSS、DLSS-G 与模型文件。

### 安装

1. 完全退出游戏。
2. 解压 Release ZIP。
3. 右键 `Install.ps1`，选择“使用 PowerShell 运行”，或执行：

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\Install.ps1
   ```

默认目标为 `C:\XboxGames\Forza Motorsport\Content`。游戏在其他位置时使用
`-GameDir`。安装器会在游戏运行时拒绝写入，为所有被修改的文件创建备份，保留
现有 ReShade 电影滤镜和其他 techniques，并只添加必需的 HUD carrier。

游戏内按 **Insert** 打开 ReShade。发布配置请求固定 4x MFG；DLSS/NR 的质量与
电影滤镜仍由现有 DLSS add-on 控制。

### 卸载

退出游戏后执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\Uninstall.ps1
```

卸载器会依据安装清单恢复安装前的每一个文件，备份本身仍会保留。

### 已知限制

- 这是与具体游戏和 DLSS-G 集成相关的 Beta。
- 游戏、Streamline、DLSS-G 或 ReShade 更新后，Hook 签名可能需要更新。
- 基础帧率很低，或出现尚未识别的临时 Overlay 时，仍可能存在少量伪影。
- 注入式 add-on 可能与反作弊或后续在线政策不兼容，建议优先离线/单人使用。

## English

This is an experimental **Forza Motorsport** fix for a specific DLSS Multi
Frame Generation failure mode. It targets repeated outlines and trails on the
lower-right tachometer, numbers, car wheels and some moving geometry after
DLSS-G/MFG is enabled. It is not a generic sharpening or anti-aliasing filter.

### What it does

Motorsport can submit the main 3D scene and its late HUD/UI through different
Streamline resource-tagging paths. If frame generation consumes scene color,
motion vectors and UI from different moments as one frame, generated frames may
interpolate the HUD or leave wheels and vehicle edges behind the real image.

This mod performs three local changes inside the game process:

1. It captures the authoritative current Frame Token from `slSetConstants`.
2. It binds Scene Color, Depth, Motion Vectors, HUDless Color and UI Color/Alpha
   to that same Frame Token.
3. It supplies full-resolution HUDless and UI layers so DLSS-G can recompose the
   UI after scene interpolation.

The intended pipeline is:

```text
Real 3D scene
  -> DLSS Super Resolution
  -> DLSS-G/MFG on the HUDless scene only
  -> native-resolution UI recomposition
  -> display
```

All changes are game-local. The mod does not change the global NVIDIA profile
or the system-wide NGX OTA policy.

### Observed result

The first public build uses the exact binary pair tested while driving with:

- Forza Motorsport `1.859.7102.0`
- RTX 4090 Laptop, Windows 11 and 4K output
- Streamline `2.14.1` and DLSS-G `310.9.16`
- fixed 4x MFG while retaining the 2-pass cinematic NR setup

In that setup, the previously obvious multiple copies of the lower-right
tachometer became a stable single UI image. Trails on cars and wheels were also
substantially reduced while retaining the high output frame rate of 4x MFG.
`TESTED_PROFILE.ini` records the reference settings.

Results still depend on base frame rate, game build, DLSS-G version and other
injected add-ons. This beta does not promise that every interpolation artifact
will disappear on every system or future game build.

### Release contents

- `version.dll`: Motorsport-specific DLSSG-Transfusion proxy build.
- `fm-hudless-mfg.addon64`: ReShade add-on that creates and tags HUDless/UI
  resources.
- `FMHUDlessCarrier.fx`: invisible ReShade carrier technique that provides a
  safe D3D12 command-list submission point.
- `DLSSG-Transfusion.json`: tested local fixed-4x profile with high-frequency
  tracing disabled for the public build.
- `Install.ps1` / `Uninstall.ps1`: per-file backup-aware install and restore.
- `SHA256SUMS.txt`: release file checksums.

The repository contains the complete add-on source, HLSL, generated DXIL header
and three modified DLSSG-Transfusion source files. It does **not** contain game
files, NVIDIA DLLs/models, ReShade binaries or Microsoft Detours.

### Requirements

- Microsoft Store/Xbox App build of Forza Motorsport.
- Windows 11 and an RTX 40-series GPU with Hardware-accelerated GPU scheduling.
- ReShade with add-on support already installed in the game directory.
- A working local DLSS/Streamline/MFG setup. The mod does not download or
  replace NVIDIA DLSS, DLSS-G or model files.

### Installation

1. Exit the game completely.
2. Extract the Release ZIP.
3. Right-click `Install.ps1` and choose **Run with PowerShell**, or run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\Install.ps1
   ```

The default target is `C:\XboxGames\Forza Motorsport\Content`. Use `-GameDir`
for another location. The installer refuses to write while the game is active,
backs up every changed file, preserves existing ReShade film filters and other
techniques, and only appends the required HUD carrier.

Press **Insert** in game to open ReShade. The supplied profile requests fixed
4x MFG; DLSS/NR quality and film-filter settings remain controlled by the
existing DLSS add-on.

### Uninstall

Exit the game, then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\Uninstall.ps1
```

The uninstaller restores every pre-install file from the installation manifest
and retains the backup directory.

### Known limitations

- This is a beta tied to a specific game and DLSS-G integration.
- Game, Streamline, DLSS-G or ReShade updates may require new hook signatures.
- A small amount of artifacting may remain at very low base frame rates or in
  transient overlays that have not been identified.
- Injected add-ons may conflict with anti-cheat or future online policies.
  Prefer offline/single-player use.

## License

Original code in this repository is MIT licensed. See
`THIRD_PARTY_NOTICES.md` for upstream attribution.

