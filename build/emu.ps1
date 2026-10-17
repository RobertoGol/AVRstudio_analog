# =====================================================================
#  AVR CLI Studio - Emulator loader (emu.ps1)
#  The core is compiled C++: build/emu.exe  (source: build/emu.cpp)
#  PowerShell here is only a thin launcher / compatibility shell
#  (README rule sec.10: PS 1.1 - 7.6.0+, no ANSI escapes, no new cmdlets).
# =====================================================================
param(
    [string]$ProjectRoot = (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)),
    [string]$ElfFile     = "",
    [string]$RunScript   = ""
)

$exe = Join-Path $PSScriptRoot "emu.exe"
if (-not (Test-Path $exe)) {
    Write-Host "emu.exe not found. Build it first:" -ForegroundColor Red
    Write-Host ('  g++ -O2 -std=c++17 -o "' + $exe + '" "' + (Join-Path $PSScriptRoot 'emu.cpp') + '"') -ForegroundColor Yellow
    exit 1
}

$argList = @()
if ($RunScript -ne "") {
    if (-not (Test-Path $RunScript)) { Write-Host "Script not found: $RunScript" -ForegroundColor Red; exit 1 }
    $argList += @("-f", (Resolve-Path $RunScript).Path)
}
if ($ElfFile     -ne "") { $argList += @("-e", $ElfFile) }

$oldDir = Get-Location
Set-Location $ProjectRoot
try   { & $exe @argList }
finally { Set-Location $oldDir }
