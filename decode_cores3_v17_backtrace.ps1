$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Elf = Join-Path $Root ".pio\build\m5stack_cores3\firmware.elf"
$ToolchainBin = Join-Path $env:USERPROFILE ".platformio\packages\toolchain-xtensa-esp-elf\bin"

Write-Host "============================================================"
Write-Host "WLED CoreS3 V17 Backtrace Decoder"
Write-Host "Phase 10.4.2P-V17h"
Write-Host "============================================================"
Write-Host ""

if (-not (Test-Path $Elf)) {
    Write-Host "ERROR: firmware.elf not found:"
    Write-Host "  $Elf"
    exit 1
}

$Addr2Line = Get-ChildItem $ToolchainBin -Filter "*addr2line.exe" -File |
    Select-Object -First 1 -ExpandProperty FullName

if (-not $Addr2Line) {
    Write-Host "ERROR: addr2line.exe not found under:"
    Write-Host "  $ToolchainBin"
    exit 2
}

Write-Host "ELF:"
Write-Host "  $Elf"
Write-Host "addr2line:"
Write-Host "  $Addr2Line"
Write-Host ""

$Addresses = @(
    "0x4037de81",
    "0x4037de49",
    "0x403854ce",
    "0x420b11ce",
    "0x420a9cec",
    "0x420a9d4d",
    "0x4202ee6d",
    "0x4204d1f5",
    "0x4204d49b",
    "0x4205e07f",
    "0x4205e38e",
    "0x420964cc",
    "0x4037ebad"
)

Write-Host "Decoded backtrace:"
Write-Host "------------------------------------------------------------"

& $Addr2Line -pfiaC -e $Elf $Addresses

Write-Host "------------------------------------------------------------"
Write-Host ""
Write-Host "Copy the decoded output and paste it into ChatGPT."
