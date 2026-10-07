param([string]$VersionText)
$ErrorActionPreference = 'Stop'
if (!$VersionText) { $VersionText = (Get-Content (Join-Path $PSScriptRoot 'VERSION') -Raw).Trim() }
if ($VersionText -notmatch '^\d+\.\d+\.\d+$') { throw 'Version must be major.minor.patch.' }
$compilerPath = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (!(Test-Path -LiteralPath $compilerPath)) { throw 'Windows .NET Framework C# compiler is required for the settings UI.' }
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswherePath)) { throw 'MSVC is required to build the native effect engine.' }
$installation = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'MSVC C++ tools are required to build the native effect engine.' }
$outputDirectory = $PSScriptRoot
$versionFile = Join-Path $env:TEMP ('toy-version-' + [guid]::NewGuid().ToString('N') + '.cs')
[IO.File]::WriteAllText($versionFile, ('[assembly: System.Reflection.AssemblyVersion("' + $VersionText + '.0")]'), (New-Object Text.UTF8Encoding($false)))
& $compilerPath /nologo /target:winexe /optimize+ "/out:$outputDirectory\차라리 이거.exe" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll /reference:System.Runtime.Serialization.dll /reference:System.IO.Compression.dll /reference:System.IO.Compression.FileSystem.dll $versionFile (Join-Path $PSScriptRoot 'Settings.cs') (Join-Path $PSScriptRoot 'App.cs') (Join-Path $PSScriptRoot 'Update.cs')
Remove-Item -LiteralPath $versionFile -ErrorAction SilentlyContinue
if ($LASTEXITCODE -ne 0) { throw 'Settings app build failed. Close the app before rebuilding.' }
$environmentScript = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$taskId = [Guid]::NewGuid().ToString('N')
$nativeBatch = Join-Path $env:TEMP ('mountain-build-' + $taskId + '.cmd')
$nativeObject = Join-Path $env:TEMP ('mountain-build-' + $taskId + '.obj')
try {
    $lines = @('@echo off', 'chcp 65001 >nul', ('call "' + $environmentScript + '" >nul'), 'if errorlevel 1 exit /b 1',
        ('cl /nologo /O2 /MT /EHsc /utf-8 /DUNICODE /D_UNICODE "' + (Join-Path $PSScriptRoot 'NativeEffects.cpp') + '" /Fo:"' + $nativeObject + '" /Fe:"' + $outputDirectory + '\copilot_key.exe" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib'))
    [IO.File]::WriteAllLines($nativeBatch, $lines, (New-Object Text.UTF8Encoding($false)))
    & cmd /d /c $nativeBatch
    if ($LASTEXITCODE -ne 0) { throw 'Native engine build failed.' }
}
finally { Remove-Item -LiteralPath $nativeBatch,$nativeObject -ErrorAction SilentlyContinue }
Write-Output 'Build complete: settings UI + native copilot_key effect engine.'
