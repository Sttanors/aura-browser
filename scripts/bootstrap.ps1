param([switch]$Offline, [switch]$FetchOnly, [int]$Jobs = 8)
$ErrorActionPreference = 'Stop'
$arguments = @("$PSScriptRoot/bootstrap.py", '--jobs', "$Jobs")
if ($Offline) { $arguments += '--offline' }
if ($FetchOnly) { $arguments += '--fetch-only' }
python @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
