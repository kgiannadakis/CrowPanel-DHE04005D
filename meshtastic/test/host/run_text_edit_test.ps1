param([string]$Zig = $env:ZIG)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
if (-not $Zig) {
    $command = Get-Command zig -ErrorAction SilentlyContinue
    if ($command) { $Zig = $command.Source }
    else { $Zig = "$repo/.pio/host-tools/ziglang/zig.exe" }
}
if (-not (Test-Path -LiteralPath $Zig -PathType Leaf)) {
    throw 'Zig was not found. Install it on PATH, set ZIG, or pass -Zig with its executable path.'
}
$zig = $Zig
$lvgl = "$repo/.pio/libdeps/dhe04005d-5inch-P4-meshtastic/lvgl"
if (-not (Test-Path -LiteralPath "$lvgl/src" -PathType Container)) {
    throw 'LVGL is missing. Resolve the CrowPanel PlatformIO dependencies first.'
}
$config = "$PSScriptRoot/lvgl"
$library = "$repo/.pio/host-lvgl.lib"
Push-Location $lvgl
try {
    $sources = (Get-ChildItem src -Recurse -Filter '*.c').FullName | ForEach-Object {
        $_.Substring($lvgl.Length + 1).Replace('\', '/')
    }
    & $zig build-lib -lc -O ReleaseSafe -DLV_CONF_INCLUDE_SIMPLE '-I.' "-I$config" @sources "-femit-bin=$library"
    if ($LASTEXITCODE -ne 0) { throw 'LVGL host build failed' }
} finally { Pop-Location }
Push-Location $repo
try {
    & $zig c++ -std=c++17 -O1 -g -fsanitize=undefined -fno-sanitize-recover=all `
        -DHAS_TFT=1 -DUSE_MCUI=1 -DLV_CONF_INCLUDE_SIMPLE "-I$config" "-I$lvgl" `
        -Isrc test/host/text_edit_test.cpp src/graphics/mcui/McTextEdit.cpp `
        src/graphics/mcui/McKeyboard.cpp $library -o .pio/text_edit_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Text edit host build failed' }
    & .pio/text_edit_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Text edit test failed' }
} finally { Pop-Location }
