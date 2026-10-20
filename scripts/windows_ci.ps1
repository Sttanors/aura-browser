param([Parameter(Mandatory=$true)][ValidateSet('bootstrap','configure','build','test','package')][string]$Stage)
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools missing' }
$dev = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$commands = @("call `"$dev`" -arch=x64 -host_arch=x64")
switch ($Stage) {
    bootstrap {
        # The workflow restores this directory only from the matching locked
        # dependencies, Skia build flags and source patches cache key.
        # Reuse the completed static library; fresh extraction otherwise makes
        # Ninja recompile generated sources after restoring object files.
        if (Test-Path 'third_party/skia-build/windows64/skia.lib') {
            $commands += 'python scripts/bootstrap.py --fetch-only'
        } else {
            $commands += 'python scripts/bootstrap.py --jobs 4'
        }
    }
    configure { $commands += 'cmake --preset windows-x64-release' }
    build { $commands += 'cmake --build --preset windows-x64-release --parallel 4' }
    test {
        $commands += 'ctest --preset windows-x64-release'
        $commands += 'python scripts/smoke_windows_browser.py'
    }
    package { $commands += 'python scripts/package.py' }
}
$path = Join-Path $env:RUNNER_TEMP "aura-$Stage.cmd"
Set-Content -Path $path -Value ($commands -join "`r`nif errorlevel 1 exit /b 1`r`n")
cmd /c $path
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Stage -eq 'package') {
    Get-FileHash artifacts/aura2-windows64.zip -Algorithm SHA256 | ForEach-Object {
        "$($_.Hash.ToLower())  aura2-windows64.zip" | Set-Content artifacts/aura2-windows64.zip.sha256
    }
}
