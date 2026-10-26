$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    # windres cannot reliably open resources below a Chinese directory name.
    # Stage only build inputs in the user's temporary folder, never account data.
    $taskBuildRoot = Join-Path ([IO.Path]::GetTempPath()) 'TeleAgentAccountDesk-v050-build'
    $taskSource = Join-Path $taskBuildRoot 'source'
    $taskBuild = Join-Path $taskBuildRoot 'build'
    New-Item -ItemType Directory -Path $taskSource -Force | Out-Null
    Copy-Item -LiteralPath 'src','ui','assets','third_party','CMakeLists.txt','app.rc','app.manifest','WebView2Loader.dll' -Destination $taskSource -Recurse -Force
    cmake -S $taskSource -B $taskBuild -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败' }
    cmake --build $taskBuild
    if ($LASTEXITCODE -ne 0) { throw '编译失败' }
    New-Item -ItemType Directory -Path '.\build-local' -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $taskBuild 'TeleAgentAccounts.exe') -Destination '.\build-local\TeleAgentAccounts.exe' -Force
    Copy-Item -LiteralPath 'WebView2Loader.dll' -Destination '.\build-local\WebView2Loader.dll' -Force
    Copy-Item -LiteralPath '.\build-local\TeleAgentAccounts.exe' -Destination '.\TeleAgentAccounts.exe' -Force
    Write-Host '已生成 TeleAgentAccounts.exe'
} finally { Pop-Location }
