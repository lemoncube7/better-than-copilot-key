param([string]$VersionText)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -VersionText $VersionText
$testRoot = Join-Path $env:TEMP ('toy-check-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$native=Start-Process -FilePath (Join-Path $PSScriptRoot 'copilot_key.exe') -ArgumentList ('--selftest --verify "' + (Join-Path $testRoot 'selftest.log') + '"') -WindowStyle Hidden -PassThru
if (!$native.WaitForExit(20000) -or $native.ExitCode -ne 0) { throw 'Native self-test failed.' }
$update=Start-Process -FilePath (Join-Path $PSScriptRoot '차라리 이거.exe') -ArgumentList ('--update-test "' + (Join-Path $testRoot 'update') + '"') -WindowStyle Hidden -PassThru
if (!$update.WaitForExit(20000) -or $update.ExitCode -ne 0) { throw 'Update self-test failed.' }
$packageRoot = Join-Path $testRoot 'package'
New-Item -ItemType Directory -Path $packageRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'copilot_key.exe'),(Join-Path $PSScriptRoot '차라리 이거.exe'),(Join-Path $PSScriptRoot '시작하기.txt') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'settings.example.json') -Destination (Join-Path $packageRoot 'settings.json')
$zip = Join-Path $PSScriptRoot 'better-than-copilot-key-windows-x64.zip'
Compress-Archive -LiteralPath (Join-Path $packageRoot 'copilot_key.exe'),(Join-Path $packageRoot '차라리 이거.exe'),(Join-Path $packageRoot '시작하기.txt'),(Join-Path $packageRoot 'settings.json') -DestinationPath $zip -Force
(Get-FileHash $zip).Hash.ToLowerInvariant() | Set-Content (Join-Path $PSScriptRoot 'SHA256.txt') -Encoding ascii
Write-Output 'Package complete. Native and update self-tests passed.'
