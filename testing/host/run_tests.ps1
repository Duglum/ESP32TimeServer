# ESP32 Time Server - host tests (see README.md)
# Builds the tests with clang (LLVM-MinGW) and AddressSanitizer / UndefinedBehaviorSanitizer and runs them.
#   .\run_tests.ps1                    unit tests
#   .\run_tests.ps1 -FuzzSeconds 300   also builds the libFuzzer targets and runs each for 300 s
#   .\run_tests.ps1 -FuzzSeconds 180 -Only radius   only the targets whose name contains "radius"
param([int]$FuzzSeconds = 0, [string]$Only = '')

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

# TF-PSA-Crypto of ESP-IDF (MD5, SHA-1, SHA-256, HMAC) for the RADIUS and TOTP tests: built once for the host (Release,
# without the sanitizers) into build\psa with ESP-IDF's CMake and Ninja. ESP-IDF's copy includes "mbedtls/bignum.h" and
# "mbedtls/ecp.h" of its port: stubs\psa_shim maps them to the private headers. Its include folders come first: stubs
# has a "psa/crypto.h" with SHA-256 only for the other tests.
$idf = if ($env:IDF_PATH) { $env:IDF_PATH } else { 'C:\esp\v6.1\esp-idf' }
$psaSource = Join-Path $idf 'components\mbedtls\mbedtls\tf-psa-crypto'
$psaBuild = Join-Path $build 'psa'
$psaShim = Join-Path $here 'stubs\psa_shim'
$psaFlags = @("-I$psaShim", "-I$psaSource\include", "-I$psaSource\drivers\builtin\include", "-I$psaBuild\include")
$psaLibs = @('tfpsacrypto', 'builtin', 'extras', 'platform', 'utilities', 'everest', 'p256-m', 'pqcp')
function Build-Psa {
    if (Test-Path (Join-Path $psaBuild 'core\libtfpsacrypto.a')) { return }
    $tools = Join-Path $env:USERPROFILE '.espressif\tools'
    $cmake = Get-ChildItem "$tools\cmake\*\bin\cmake.exe" -ErrorAction SilentlyContinue | Select-Object -Last 1 -ExpandProperty FullName
    $ninja = Get-ChildItem "$tools\ninja\*\ninja.exe" -ErrorAction SilentlyContinue | Select-Object -Last 1 -ExpandProperty FullName
    if (-not $cmake -or -not $ninja -or -not (Test-Path $psaSource)) { throw "TF-PSA-Crypto, CMake or Ninja of ESP-IDF not found ($psaSource, $tools)" }
    $env:Path = (Split-Path $ninja) + ';' + $env:Path
    $cc = Join-Path (Split-Path $clang) 'clang.exe'
    $shim = $psaShim -replace '\\', '/'
    & $cmake -S $psaSource -B $psaBuild -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$cc" "-DCMAKE_C_FLAGS=-I$shim" `
        -DGEN_FILES=OFF -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'TF-PSA-Crypto: CMake failed' }
    & $cmake --build $psaBuild | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'TF-PSA-Crypto: build failed' }
}
function Psa-Link {
    $libs = foreach ($lib in $psaLibs) { Get-ChildItem $psaBuild -Recurse -Filter "lib$lib.a" | Select-Object -First 1 -ExpandProperty FullName }
    return @($libs) + @('-lbcrypt')
}

# cJSON comes with the ESP-IDF component manager (managed_components after the first firmware build)
$cjson = Join-Path $here '..\..\managed_components\espressif__cjson\cJSON'
$common = @('-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
    "-I$here\stubs", "-I$here", "-I$main", "-I$main\custom", "-I$cjson", '-DAPP_BUILD_DATE="2026-10-28"')
$warnings = @('-Wall', '-Wextra', '-Werror')
$fuzz = @('-fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp', '-DHOST_FUZZ')

# name = test program, sources (C files are compiled as C11)
$targets = [ordered]@{
    gnss_test      = @("$here\stubs\host_stubs.c", "$here\gnss_uart_hooks.cpp", "$here\gnss_test.cpp",
        "$main\custom\gnss_nmea.cpp", "$main\custom\gnss_ubx.cpp", "$main\custom\gnss_console.cpp")
    gnss_console_test = @("$here\stubs\host_stubs.c", "$here\gnss_uart_hooks.cpp", "$here\gnss_console_test.cpp",
        "$main\custom\gnss_nmea.cpp", "$main\custom\gnss_ubx.cpp", "$main\custom\gnss_console.cpp")
    ntp_cache_test = @("$here\stubs\host_stubs.c", "$main\custom\ntp_cache.c", "$here\ntp_cache_test.cpp")
    ptp_test       = @("$here\ptp_test.cpp")
    ptp_selection_test = @("$here\ptp_selection_test.cpp")
    mqtt_queue_test = @("$here\mqtt_queue_test.cpp")
    ntp_auth_test  = @("$here\stubs\host_stubs.c", "$main\custom\ntp_auth.cpp", "$here\ntp_auth_test.cpp")
    ptp_auth_test  = @("$here\stubs\host_stubs.c", "$main\custom\ptp_auth.cpp", "$here\ptp_auth_test.cpp")
    # The login modules, taken from ESP32-ConsoleServer with their tests (check.h, seeds\<name>_fuzz)
    ip_text_test   = @("$here\ip_text_test.cpp", "$main\net\ip_text.cpp")
    radius_test    = @("$here\radius_test.cpp", "$main\auth\radius_proto.cpp", "$main\auth\radius_client.cpp")
    totp_test      = @("$here\totp_test.cpp", "$main\auth\totp.cpp", "$main\third_party\qrcodegen\qrcodegen.c")
    auth_config_test = @("$here\auth_config_test.cpp", "$main\auth\auth_config.cpp", "$main\auth\radius_proto.cpp",
        "$main\net\ip_text.cpp", "$cjson\cJSON.c")
}
# Test programs without a fuzz target
$no_fuzz = @('ptp_selection_test', 'mqtt_queue_test', 'ntp_auth_test', 'ptp_auth_test')
# Test programs with TF-PSA-Crypto
$with_psa = @('radius_test', 'totp_test', 'auth_config_test')

function Build-Target([string]$name, [string[]]$sources, [string]$folder, [string[]]$flags, [string[]]$extra) {
    New-Item -ItemType Directory -Force $folder | Out-Null
    $objects = @()
    foreach ($source in $sources) {
        $object = Join-Path $folder ([IO.Path]::GetFileNameWithoutExtension($source) + '.o')
        # Third-party code (unchanged) is compiled without -Werror
        $sourceFlags = if ($source.StartsWith("$main\third_party") -or $source.StartsWith($cjson)) { $flags | Where-Object { $_ -notin $warnings } } else { $flags }
        if ($source.EndsWith('.c')) { & clang -std=gnu11 @sourceFlags -c $source -o $object }
        else { & clang++ -std=gnu++23 @sourceFlags -c $source -o $object }
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
    if ($Only -and $name -notlike "*$Only*") { continue }
    $flags = $common + $warnings
    $libs = @()
    if ($with_psa -contains $name) { Build-Psa; $flags = $psaFlags + $flags; $libs = Psa-Link }
    $exe = Build-Target $name $targets[$name] (Join-Path $build $name) $flags $libs
    & $exe
    if ($LASTEXITCODE -ne 0) { $failed++ }
}

if ($FuzzSeconds -gt 0) {
    $runtime = Join-Path (& clang++ -print-resource-dir) 'lib\windows\libclang_rt.fuzzer-x86_64.a'
    foreach ($name in $targets.Keys) {
        if ($no_fuzz -contains $name) { continue }
        if ($Only -and $name -notlike "*$Only*") { continue }
        $fuzzer = $name -replace '_test$', '_fuzz'
        $flags = $common + $fuzz + @('-w')
        $libs = @($runtime)
        if ($with_psa -contains $name) { Build-Psa; $flags = $psaFlags + $flags; $libs += Psa-Link }
        $exe = Build-Target $fuzzer $targets[$name] (Join-Path $build $fuzzer) $flags $libs
        $corpus = Join-Path $build "corpus_$fuzzer"
        New-Item -ItemType Directory -Force $corpus | Out-Null
        if ($name -eq 'gnss_test') { & (Join-Path $build 'gnss_test.exe') --seeds $corpus }
        $corpora = @($corpus)
        $seeds = Join-Path $here "seeds\$fuzzer"
        if (Test-Path $seeds) { $corpora += $seeds }
        # A RADIUS datagram can be 4096 bytes long
        $maxLen = if ($name -eq 'radius_test') { 4096 } else { 2048 }
        Push-Location $build
        try { & $exe @corpora "-max_total_time=$FuzzSeconds" "-max_len=$maxLen" -print_final_stats=1 }
        finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { $failed++; Write-Host "$fuzzer found a problem: see the crash-* file in $build" }
    }
}

if ($failed -ne 0) { Write-Host "FAILED: $failed"; exit 1 }
Write-Host 'All host tests passed'
