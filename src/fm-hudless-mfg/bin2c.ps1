param(
  [Parameter(Mandatory = $true)][string]$InputFile,
  [Parameter(Mandatory = $true)][string]$OutputFile,
  [string]$Symbol = 'kUiMaskDxil'
)

$bytes = [System.IO.File]::ReadAllBytes([System.IO.Path]::GetFullPath($InputFile))
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('#pragma once')
$lines.Add('#include <cstddef>')
$lines.Add('#include <cstdint>')
$lines.Add("inline constexpr std::uint8_t $Symbol`[] = {")
for ($offset = 0; $offset -lt $bytes.Length; $offset += 16) {
  $end = [Math]::Min($offset + 16, $bytes.Length)
  $chunk = for ($index = $offset; $index -lt $end; ++$index) {
    '0x{0:X2}' -f $bytes[$index]
  }
  $lines.Add('  ' + ($chunk -join ', ') + ',')
}
$lines.Add('};')
$lines.Add("inline constexpr std::size_t ${Symbol}Size = sizeof($Symbol);")
[System.IO.File]::WriteAllLines([System.IO.Path]::GetFullPath($OutputFile), $lines)
