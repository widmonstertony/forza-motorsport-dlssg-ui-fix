[CmdletBinding()]
param(
    [string]$GameDir = 'C:\XboxGames\Forza Motorsport\Content',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$game = [System.IO.Path]::GetFullPath($GameDir).TrimEnd('\')
$manifestPath = Join-Path $game 'FM-DLSSG-UI-Fix.install.json'

$running = Get-Process -Name 'forza_gaming.desktop.x64_release_final' -ErrorAction SilentlyContinue
if ($running -and -not $Force) {
    throw 'Forza Motorsport is running. Exit the game and run the uninstaller again.'
}
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Install manifest not found: $manifestPath"
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$backupDir = [System.IO.Path]::GetFullPath([string]$manifest.BackupDirectory)
if (-not (Test-Path -LiteralPath $backupDir -PathType Container)) {
    throw "Backup directory not found: $backupDir"
}

foreach ($record in $manifest.Files) {
    $target = [System.IO.Path]::GetFullPath((Join-Path $game ([string]$record.RelativePath)))
    if (-not $target.StartsWith($game + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest contains a target outside the game directory: $target"
    }
    if ([bool]$record.Existed) {
        $backup = Join-Path $backupDir ([string]$record.BackupName)
        if (-not (Test-Path -LiteralPath $backup)) { throw "Backup file missing: $backup" }
        Copy-Item -LiteralPath $backup -Destination $target -Force
    } elseif (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $target -Force
    }
}

Remove-Item -LiteralPath $manifestPath -Force
Write-Host 'The previous Forza Motorsport files and ReShade settings were restored.' -ForegroundColor Green
Write-Host "Backup retained at: $backupDir"

