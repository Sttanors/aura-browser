param([switch]$UseLocalArchive)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path '.').Path
$payload = Join-Path $root 'artifacts/installer-payload'
New-Item -ItemType Directory -Force artifacts | Out-Null
$identityText = python scripts/release_identity.py
if ($LASTEXITCODE -ne 0) { throw 'Cannot read compiled release identity' }
$identity = $identityText | ConvertFrom-Json
$numeric = $identity.numeric_version
if (-not $UseLocalArchive) {
    gh release download "$($identity.tag)" --repo Sttanors/aura-browser --pattern aura2-windows64.zip --pattern aura2-windows64.zip.sha256 --dir artifacts --clobber
    if ($LASTEXITCODE -ne 0) { throw 'Runtime download failed' }
    $expected = ((Get-Content artifacts/aura2-windows64.zip.sha256) -split '\s+')[0]
    if ((Get-FileHash artifacts/aura2-windows64.zip -Algorithm SHA256).Hash.ToLower() -ne $expected) { throw 'Runtime SHA256 mismatch' }
}
Expand-Archive artifacts/aura2-windows64.zip -DestinationPath $payload -Force
$version = (Get-Item (Join-Path $payload 'Aura2/aura_browser.exe')).VersionInfo
if ($version.FileMajorPart -ne $numeric[0] -or $version.FileMinorPart -ne $numeric[1] -or $version.FileBuildPart -ne $numeric[2] -or $version.FilePrivatePart -ne $numeric[3]) { throw 'Executable numeric version mismatch' }
if ($version.ProductVersion -ne $identity.version) { throw 'Executable product version mismatch' }
if ($version.ProductName -ne 'Aura Browser' -or $version.FileDescription -ne 'Aura Browser') { throw 'Executable identity missing' }
if (-not (Test-Path (Join-Path $payload 'Aura2/branding/aura.ico'))) { throw 'Branding assets missing' }
if (Test-Path (Join-Path $payload 'Aura2/bootstrap.exe')) { throw 'Unbranded CEF bootstrap leaked into package' }
$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { throw 'Inno Setup 6 missing from CI image' }
& $iscc "/DAuraVersion=$($identity.version)" "/DAuraNumericVersion=$($numeric -join '.')" "/DPayloadDir=$payload\Aura2" "/DOutputDir=$root\artifacts" packaging/windows/aura.iss
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed' }
$setup = Join-Path $root 'artifacts/aura2-windows64-setup.exe'
$installed = Join-Path $env:LOCALAPPDATA 'Programs/Aura Browser'
$profile = Join-Path $env:LOCALAPPDATA 'Aura Browser/profile'
New-Item -ItemType Directory -Force $profile | Out-Null
Set-Content (Join-Path $profile 'preserve-me.txt') 'profile must survive uninstall'
function Install-Aura {
    $p = Start-Process $setup -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /TASKS=desktopicon' -PassThru -Wait
    if ($p.ExitCode -ne 0) { throw "Install failed: $($p.ExitCode)" }
}
Install-Aura
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{9B640453-B1CB-4653-A0B7-7DCCBA5F3D6E}_is1'
if ((Get-ItemProperty $uninstallKey).DisplayVersion -ne $identity.version) { throw 'Installed DisplayVersion mismatch' }
if ((Get-Item (Join-Path $installed 'aura_browser.exe')).VersionInfo.ProductVersion -ne $identity.version) { throw 'Installed executable version mismatch' }
foreach ($name in 'aura_browser.exe','aura_browser.dll','libcef.dll','default.toml') {
    if (-not (Test-Path (Join-Path $installed $name))) { throw "Missing installed file: $name" }
}
$shell = New-Object -ComObject WScript.Shell
$shortcutPath = Join-Path ([Environment]::GetFolderPath('Programs')) 'Aura Browser/Aura Browser.lnk'
if (-not (Test-Path $shortcutPath)) { throw 'Start menu shortcut missing' }
$shortcut = $shell.CreateShortcut($shortcutPath)
if ($shortcut.TargetPath -ne (Join-Path $installed 'aura_browser.exe')) { throw 'Wrong shortcut target' }
if ($shortcut.Arguments -ne "`"--data-dir=$env:LOCALAPPDATA\Aura Browser\profile`"") { throw "Wrong profile arguments: $($shortcut.Arguments)" }
$desktopLink = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Aura Browser.lnk'
if (-not (Test-Path $desktopLink)) { throw 'Desktop shortcut missing' }
$env:AURA_TEST_BINARY = Join-Path $installed 'aura_browser.exe'
python scripts/smoke_windows_browser.py
if ($LASTEXITCODE -ne 0) { throw 'Installed browser integration failed' }
# Reinstall must retain data and continue to produce working shortcuts.
Install-Aura
$p = Start-Process (Join-Path $installed 'unins000.exe') -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -PassThru -Wait
if ($p.ExitCode -ne 0) { throw 'Uninstall failed' }
if (Test-Path (Join-Path $installed 'aura_browser.exe')) { throw 'Browser survived uninstall' }
if (Test-Path $shortcutPath) { throw 'Start shortcut survived uninstall' }
if (Test-Path $desktopLink) { throw 'Desktop shortcut survived uninstall' }
if (-not (Test-Path (Join-Path $profile 'preserve-me.txt'))) { throw 'Uninstaller removed profile' }
Get-FileHash $setup -Algorithm SHA256 | ForEach-Object {
    "$($_.Hash.ToLower())  aura2-windows64-setup.exe" | Set-Content artifacts/aura2-windows64-setup.exe.sha256
}
Set-Content artifacts/windows-installer-PASS.txt 'PASS: install, start/desktop shortcuts, installed browser GUI, reinstall, uninstall, profile preservation.'
