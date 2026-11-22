# Wada build script — w64devkit only, no system toolchain needed.
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$candidates = @(
    (Join-Path $root ".tools\w64devkit\w64devkit\bin"),
    (Join-Path $root "..\c-desktop\.tools\w64devkit\w64devkit\bin"),
    "$env:WADA_W64DEVKIT"
)
$bin = $null
foreach ($c in $candidates) {
    if ($c -and (Test-Path (Join-Path $c "gcc.exe"))) { $bin = $c; break }
}
if (-not $bin) {
    throw "w64devkit not found. Put it at .tools\w64devkit\w64devkit or set WADA_W64DEVKIT."
}

$env:PATH = "$bin;$env:PATH"
Write-Host "toolchain: $bin"

cmake -S $root -B (Join-Path $root "build") -G Ninja `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    -DCMAKE_C_COMPILER="$bin\gcc.exe"
cmake --build (Join-Path $root "build")
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$tests = Get-ChildItem (Join-Path $root "build") -Filter "*_tests.exe" | Sort-Object Name
foreach ($t in $tests) {
    Write-Host ""
    Write-Host "--- $($t.Name)"
    & $t.FullName
    if ($LASTEXITCODE -ne 0) { throw "$($t.Name) failed" }
}
Write-Host ""
Write-Host "build ok: build\wada.exe"
