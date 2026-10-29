# ESP32 Time Server - host tests (see README.md)
# Builds the tests with clang (LLVM-MinGW) and AddressSanitizer / UndefinedBehaviorSanitizer and runs them.
#   .\run_tests.ps1                    unit tests
#   .\run_tests.ps1 -FuzzSeconds 300   also builds the libFuzzer targets and runs each for 300 s
param([int]$FuzzSeconds = 0)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$main = Join-Path $here '..\..\main'
$build = Join-Path $here 'build'

$clang = (Get-Command clang++ -ErrorAction SilentlyContinue).Source
if (-not $clang) {
    # Installed by winget, but the shell was started before
    $clang = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*\*\bin\clang++.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $clang) {
    throw 'clang++ not found. Install LLVM-MinGW: winget install MartinStorsjo.LLVM-MinGW.UCRT'
}
# The sanitizer runtime DLLs are next to the compiler
$env:Path = (Split-Path $clang) + ';' + $env:Path

$common = @('-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
    "-I$here\stubs", "-I$here", "-I$main", "-I$main\custom", '-DAPP_BUILD_DATE="2026-10-28"')
$warnings = @('-Wall', '-Wextra', '-Werror')
$fuzz = @('-fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp', '-DHOST_FUZZ')

# name = test program, sources (C files are compiled as C11)
$targets = [ordered]@{
    gnss_test      = @("$here\stubs\host_stubs.c", "$here\gnss_uart_hooks.cpp", "$here\gnss_test.cpp",
        "$main\custom\gnss_nmea.cpp", "$main\custom\gnss_ubx.cpp", "$main\custom\gnss_console.cpp")
    gnss_console_test = @("$here\stubs\host_stubs.c", "$here\gnss_uart_hooks.cpp", "$here\gnss_console_test.cpp",
        "$main\custom\gnss_nmea.cpp", "$main\custom\gnss_ubx.cpp", "$main\custom\gnss_console.cpp")
    ntp_cache_test = @("$here\stubs\host_stubs.c", "$main\custom\ntp_cache.c", "$here\ntp_cache_test.cpp")
}

function Build-Target([string]$name, [string[]]$sources, [string]$folder, [string[]]$flags, [string[]]$extra) {
    New-Item -ItemType Directory -Force $folder | Out-Null
    $objects = @()
    foreach ($source in $sources) {
        $object = Join-Path $folder ([IO.Path]::GetFileNameWithoutExtension($source) + '.o')
        if ($source.EndsWith('.c')) { & clang -std=gnu11 @flags -c $source -o $object }
        else { & clang++ -std=gnu++23 @flags -c $source -o $object }
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $source" }
        $objects += $object
    }
    $exe = Join-Path $build "$name.exe"
    & clang++ '-fsanitize=address,undefined' @objects @extra -o $exe
    if ($LASTEXITCODE -ne 0) { throw "link failed: $name" }
    return $exe
}

$failed = 0
foreach ($name in $targets.Keys) {
    $exe = Build-Target $name $targets[$name] (Join-Path $build $name) ($common + $warnings) @()
    & $exe
    if ($LASTEXITCODE -ne 0) { $failed++ }
}

if ($FuzzSeconds -gt 0) {
    $runtime = Join-Path (& clang++ -print-resource-dir) 'lib\windows\libclang_rt.fuzzer-x86_64.a'
    foreach ($name in $targets.Keys) {
        $fuzzer = $name -replace '_test$', '_fuzz'
        $exe = Build-Target $fuzzer $targets[$name] (Join-Path $build $fuzzer) ($common + $fuzz + @('-w')) @($runtime)
        $corpus = Join-Path $build "corpus_$fuzzer"
        New-Item -ItemType Directory -Force $corpus | Out-Null
        if ($name -eq 'gnss_test') { & (Join-Path $build 'gnss_test.exe') --seeds $corpus }
        Push-Location $build
        try { & $exe $corpus "-max_total_time=$FuzzSeconds" -max_len=2048 -print_final_stats=1 }
        finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { $failed++; Write-Host "$fuzzer found a problem: see the crash-* file in $build" }
    }
}

if ($failed -ne 0) { Write-Host "FAILED: $failed"; exit 1 }
Write-Host 'All host tests passed'
