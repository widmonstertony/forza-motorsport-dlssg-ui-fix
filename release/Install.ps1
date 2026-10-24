[CmdletBinding()]
param(
    [string]$GameDir = 'C:\XboxGames\Forza Motorsport\Content',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Set-IniValue {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Section,
        [string]$Key,
        [string]$Value
    )

    $sectionStart = -1
    $sectionEnd = $Lines.Count
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -match '^\s*\[(.+?)\]\s*$') {
            if ($sectionStart -ge 0) { $sectionEnd = $i; break }
            if ($Matches[1] -ieq $Section) { $sectionStart = $i }
        }
    }

    if ($sectionStart -lt 0) {
        if ($Lines.Count -gt 0 -and $Lines[$Lines.Count - 1] -ne '') { $Lines.Add('') }
        $Lines.Add('[' + $Section + ']')
        $Lines.Add($Key + '=' + $Value)
        return
    }

    for ($i = $sectionStart + 1; $i -lt $sectionEnd; $i++) {
        if ($Lines[$i] -match '^\s*([^;#=]+?)\s*=') {
            if ($Matches[1].Trim() -ieq $Key) {
                $Lines[$i] = $Key + '=' + $Value
                return
            }
        }
    }
    $Lines.Insert($sectionEnd, $Key + '=' + $Value)
}

function Get-IniValue {
    param([string[]]$Lines, [string]$Section, [string]$Key)
    $inside = $false
    foreach ($line in $Lines) {
        if ($line -match '^\s*\[(.+?)\]\s*$') {
            $inside = $Matches[1] -ieq $Section
            continue
        }
        if ($inside -and $line -match '^\s*([^;#=]+?)\s*=\s*(.*)$') {
            if ($Matches[1].Trim() -ieq $Key) { return $Matches[2].Trim() }
        }
    }
    return $null
}

function Add-CsvItem {
    param([string]$Value, [string]$Item)
    $items = @($Value -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if (-not ($items | Where-Object { $_ -ieq $Item })) { $items += $Item }
    return ($items -join ',')
}

$game = [System.IO.Path]::GetFullPath($GameDir).TrimEnd('\')
if (-not (Test-Path -LiteralPath $game -PathType Container)) {
    throw "Game directory not found: $game"
}

$gameExe = Join-Path $game 'forza_gaming.desktop.x64_release_final.exe'
if (-not (Test-Path -LiteralPath $gameExe)) {
    throw "Forza Motorsport executable not found in: $game"
}

$running = Get-Process -Name 'forza_gaming.desktop.x64_release_final' -ErrorAction SilentlyContinue
if ($running -and -not $Force) {
    throw 'Forza Motorsport is running. Exit the game and run the installer again.'
}

$payload = Join-Path $PSScriptRoot 'payload'
$required = @('version.dll', 'fm-hudless-mfg.addon64', 'FMHUDlessCarrier.fx', 'DLSSG-Transfusion.json')
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $name))) {
        throw "Release payload is incomplete: $name"
    }
}

$reshadeIni = Join-Path $game 'ReShade.ini'
if (-not (Test-Path -LiteralPath $reshadeIni)) {
    throw 'ReShade.ini was not found. Install ReShade with add-on support first.'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = Join-Path $game (Join-Path 'FM-DLSSG-UI-Fix-Backups' $stamp)
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
$records = New-Object System.Collections.Generic.List[object]

function Backup-Target {
    param([string]$Target)
    $full = [System.IO.Path]::GetFullPath($Target)
    if (-not $full.StartsWith($game + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a file outside the game directory: $full"
    }
    $relative = $full.Substring($game.Length + 1)
    $safeName = $relative.Replace('\', '__')
    $existed = Test-Path -LiteralPath $full
    if ($existed) { Copy-Item -LiteralPath $full -Destination (Join-Path $backupDir $safeName) -Force }
    $records.Add([pscustomobject]@{ RelativePath = $relative; Existed = $existed; BackupName = $safeName })
}

$reshadeLines = [System.Collections.Generic.List[string]](Get-Content -LiteralPath $reshadeIni)
$presetValue = Get-IniValue -Lines $reshadeLines -Section 'GENERAL' -Key 'PresetPath'
if ([string]::IsNullOrWhiteSpace($presetValue)) { $presetValue = '.\ReShadePreset.ini' }
$presetCandidate = $presetValue.Trim('"')
if ([System.IO.Path]::IsPathRooted($presetCandidate)) {
    $presetPath = [System.IO.Path]::GetFullPath($presetCandidate)
} else {
    $presetPath = [System.IO.Path]::GetFullPath((Join-Path $game $presetCandidate))
}
if (-not $presetPath.StartsWith($game + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    $presetPath = Join-Path $game 'ReShadePreset.ini'
    Set-IniValue -Lines $reshadeLines -Section 'GENERAL' -Key 'PresetPath' -Value '.\ReShadePreset.ini'
}

$targets = @(
    (Join-Path $game 'version.dll'),
    (Join-Path $game 'fm-hudless-mfg.addon64'),
    (Join-Path $game 'FMHUDlessCarrier.fx'),
    (Join-Path $game 'DLSSG-Transfusion.json'),
    $reshadeIni,
    $presetPath
)
$targets | Select-Object -Unique | ForEach-Object { Backup-Target -Target $_ }

foreach ($name in $required) {
    Copy-Item -LiteralPath (Join-Path $payload $name) -Destination (Join-Path $game $name) -Force
}

$loadList = Get-IniValue -Lines $reshadeLines -Section 'ADDON' -Key 'LoadFromDllMain'
$loadList = Add-CsvItem -Value $loadList -Item 'fm-hudless-mfg.addon64'
Set-IniValue -Lines $reshadeLines -Section 'ADDON' -Key 'LoadFromDllMain' -Value $loadList
Set-IniValue -Lines $reshadeLines -Section 'ForzaMotorsport.HUDlessMFG' -Key 'Enabled' -Value '1'
Set-IniValue -Lines $reshadeLines -Section 'ForzaMotorsport.HUDlessMFG' -Key 'MaskGain' -Value '1'
Set-IniValue -Lines $reshadeLines -Section 'ForzaMotorsport.HUDlessMFG' -Key 'MaskThreshold' -Value '0.003'
Set-IniValue -Lines $reshadeLines -Section 'ForzaMotorsport.HUDlessMFG' -Key 'ProbeLevel' -Value '2'
Set-IniValue -Lines $reshadeLines -Section 'INPUT' -Key 'KeyOverlay' -Value '45,0,0,0'
[System.IO.File]::WriteAllLines($reshadeIni, $reshadeLines, (New-Object System.Text.UTF8Encoding($false)))

$presetLines = New-Object System.Collections.Generic.List[string]
if (Test-Path -LiteralPath $presetPath) {
    Get-Content -LiteralPath $presetPath | ForEach-Object { $presetLines.Add($_) }
}
$carrier = 'FMHUDlessCarrier@FMHUDlessCarrier.fx'
$currentTechniques = $null
$currentSorting = $null
foreach ($line in $presetLines) {
    if ($line -match '^Techniques=(.*)$') { $currentTechniques = $Matches[1] }
    if ($line -match '^TechniqueSorting=(.*)$') { $currentSorting = $Matches[1] }
}
$techniques = Add-CsvItem -Value $currentTechniques -Item $carrier
$sorting = Add-CsvItem -Value $currentSorting -Item $carrier

# Preset keys live before the first INI section. Handle that small format directly.
$foundTechniques = $false
$foundSorting = $false
for ($i = 0; $i -lt $presetLines.Count; $i++) {
    if ($presetLines[$i] -match '^Techniques=') { $presetLines[$i] = 'Techniques=' + $techniques; $foundTechniques = $true }
    if ($presetLines[$i] -match '^TechniqueSorting=') { $presetLines[$i] = 'TechniqueSorting=' + $sorting; $foundSorting = $true }
}
if (-not $foundTechniques) { $presetLines.Insert(0, 'Techniques=' + $carrier) }
if (-not $foundSorting) { $presetLines.Add('TechniqueSorting=' + $carrier) }
[System.IO.File]::WriteAllLines($presetPath, $presetLines, (New-Object System.Text.UTF8Encoding($false)))

$manifest = [pscustomobject]@{
    Product = 'Forza Motorsport DLSSG UI Fix'
    Version = '0.1.0-beta'
    InstalledAt = (Get-Date).ToString('o')
    BackupDirectory = $backupDir
    Files = $records
}
$manifestPath = Join-Path $game 'FM-DLSSG-UI-Fix.install.json'
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host 'Forza Motorsport DLSSG UI Fix v0.1.0-beta installed.' -ForegroundColor Green
Write-Host "Backup: $backupDir"
Write-Host 'Start the game normally. Press Insert to open ReShade.'

