$ErrorActionPreference = 'Stop'
$executable = Join-Path $PSScriptRoot 'build/windows-x64-release/aura_browser.exe'
if (-not (Test-Path $executable)) {
    throw 'Build Aura first. See README.md for Windows build instructions.'
}
& $executable "--data-dir=$(Join-Path $PSScriptRoot '.aura-profile')" @args
exit $LASTEXITCODE
