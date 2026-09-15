$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$QueueH = Join-Path $Root "third_party\cspot\cspot\bell\main\utilities\include\Queue.h"
$TcpH = Join-Path $Root "third_party\cspot\cspot\bell\main\io\include\TCPSocket.h"
$DesktopQueue = Join-Path $Root "tools\desktop-Queue.h"
$PatchTcp = Join-Path $Root "patch\TCPSocket.h"
$SavedQueue = Join-Path $Root "tools\.saved-Queue.h"
$SavedTcp = Join-Path $Root "tools\.saved-TCPSocket.h"

if (Test-Path $QueueH) { Copy-Item -Force $QueueH $SavedQueue }
if (Test-Path $TcpH) { Copy-Item -Force $TcpH $SavedTcp }
Copy-Item -Force $DesktopQueue $QueueH
Copy-Item -Force $PatchTcp $TcpH
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$vcpkg = (Join-Path $Root "vcpkg\scripts\buildsystems\vcpkg.cmake")
$mbedtls = (Join-Path $Root "vcpkg\installed\x64-windows\share\mbedtls")
$protoc = (Join-Path $Root "tools\protoc\bin\protoc.exe")
$buildDir = Join-Path $Root "build-host"
$cliDir = Join-Path $Root "third_party\cspot\targets\cli"
$dest = Join-Path $Root "release\get-auth-json\windows\vitaspotify-auth-helper.exe"

if (-not (Test-Path $cmake)) { throw "CMake not found at $cmake" }
if (-not (Test-Path $vcpkg)) { throw "vcpkg toolchain missing at $vcpkg" }
if (-not (Test-Path $protoc)) { throw "protoc missing at $protoc" }

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

Push-Location $buildDir
try {
  & $cmake $cliDir `
    -G "Visual Studio 18 2026" -A x64 `
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkg" `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DCMAKE_BUILD_TYPE=Release `
    -DUSE_PORTAUDIO=OFF `
    -DBELL_DISABLE_CODECS=ON `
    "-DBELL_EXTERNAL_MBEDTLS=$mbedtls" `
    "-DPROTOBUF_PROTOC_EXECUTABLE=$protoc"

  if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

  & $cmake --build . --config Release --target cspotcli -j
  if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
} finally {
  Pop-Location
}

$built = Join-Path $buildDir "Release\cspotcli.exe"
if (-not (Test-Path $built)) { throw "Build failed: $built not found" }

New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
Copy-Item -Force $built $dest
Write-Host "Built: $dest"

if (Test-Path $SavedQueue) { Copy-Item -Force $SavedQueue $QueueH }
if (Test-Path $SavedTcp) { Copy-Item -Force $SavedTcp $TcpH }
