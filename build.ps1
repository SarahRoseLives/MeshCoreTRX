# MeshCoreTRX build (MSYS2 MinGW-w64)
$ErrorActionPreference = "Stop"
$mingw = "C:\msys64\mingw64"
$env:PATH = "$mingw\bin;$env:PATH"

cmake -S $PSScriptRoot -B "$PSScriptRoot\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMCTRX_BUILD_GUI=ON
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build "$PSScriptRoot\build" --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

foreach ($exe in @("MeshCoreTRX-cli.exe", "MeshCoreTRX.exe")) {
    $src = Join-Path $PSScriptRoot "build\$exe"
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $PSScriptRoot $exe) -Force
    }
}
foreach ($d in @("libiio.dll", "libusb-1.0.dll", "libxml2-16.dll", "zlib1.dll", "libiconv-2.dll",
                 "libzstd.dll", "liblzma-5.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll",
                 "libwinpthread-1.dll", "glfw3.dll")) {
    $src = Join-Path $mingw "bin\$d"
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination $PSScriptRoot -Force
    }
}
Write-Host "built MeshCoreTRX.exe and MeshCoreTRX-cli.exe"
