# Builds build\VisualCopyPlus.exe with MinGW-w64 GCC (statically linked, no runtime DLLs).
#   .\build.ps1          build
#   .\build.ps1 -Run     stop the running copy, build, start the new one
param([switch]$Run, [switch]$Debug)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    $cand = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*\mingw64\bin\g++.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $cand) { throw 'g++ not found. Install MinGW-w64, e.g.: winget install BrechtSanders.WinLibs.POSIX.UCRT' }
    $env:PATH = "$($cand.DirectoryName);$env:PATH"
}

New-Item -ItemType Directory -Force build | Out-Null
$exe = Join-Path $PSScriptRoot 'build\VisualCopyPlus.exe'

if ($Run -and (Test-Path $exe)) {
    # Ask the running instance to quit cleanly (removes its tray icon), then make sure it is gone.
    Start-Process $exe -ArgumentList '--exit' -Wait
    $deadline = (Get-Date).AddSeconds(3)
    while ((Get-Process VisualCopyPlus -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
    Get-Process VisualCopyPlus -ErrorAction SilentlyContinue | Stop-Process -Force
}

& windres --input-format=rc -O coff -I res -i res/app.rc -o build/app.res.o
if ($LASTEXITCODE) { throw "windres failed ($LASTEXITCODE)" }

# MinGW GCC links its own default-manifest.o, which clashes with ours (DPI awareness, comctl v6).
# An empty object of that name found first via -B replaces it.
New-Item -ItemType Directory -Force build\nomanifest | Out-Null
'' | Set-Content -Encoding ascii build\nomanifest\empty.c
& gcc -c build/nomanifest/empty.c -o build/nomanifest/default-manifest.o
if ($LASTEXITCODE) { throw "gcc failed ($LASTEXITCODE)" }

$opt = if ($Debug) { @('-O0', '-g') } else { @('-O2', '-s') }
$sources = (Get-ChildItem src\*.cpp).FullName
$flags = @('-std=c++17', '-municode', '-mwindows', '-static', '-static-libgcc', '-static-libstdc++',
           '-DUNICODE', '-D_UNICODE', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
           '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-Bbuild/nomanifest/')
$libs = @('-luser32', '-lgdi32', '-lshell32', '-lcomctl32', '-lcomdlg32', '-ladvapi32', '-ldwmapi',
          '-lshcore', '-lwinmm', '-lversion')
& g++ @flags @opt @sources build/app.res.o -o $exe @libs
if ($LASTEXITCODE) { throw "g++ failed ($LASTEXITCODE)" }

$size = [math]::Round((Get-Item $exe).Length / 1KB)
Write-Host "Built $exe ($size KB)"

if ($Run) { Start-Process $exe }
