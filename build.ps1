param([switch]$SkipRootCopy,[string]$OutputDirectory='build-local\v0.7.0')
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $taskLoaderHash = (Get-FileHash -LiteralPath 'third_party\webview2\x64\WebView2Loader.dll' -Algorithm SHA256).Hash
    if ($taskLoaderHash -ne '86545B66CDB0603BC26B626FB9AD610CB6E71F28D468F5EA66DF23B03DDA96D5') { throw '内嵌 WebView2 加载库的 SHA256 与来源记录不符' }
    # windres cannot reliably open resources below a Chinese directory name.
    # Stage only build inputs in the user's temporary folder, never account data.
    $taskBuildRoot = Join-Path ([IO.Path]::GetTempPath()) 'TeleAgentAccountDesk-v070-build'
    $taskSource = Join-Path $taskBuildRoot 'source'
    $taskBuild = Join-Path $taskBuildRoot 'build'
    New-Item -ItemType Directory -Path $taskSource -Force | Out-Null
    Copy-Item -LiteralPath 'src','ui','assets','third_party','CMakeLists.txt','app.rc','app.manifest' -Destination $taskSource -Recurse -Force
    cmake -S $taskSource -B $taskBuild -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败' }
    cmake --build $taskBuild
    if ($LASTEXITCODE -ne 0) { throw '编译失败' }
    $taskOutput = Join-Path $PSScriptRoot $OutputDirectory
    New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
    $taskExecutable = Join-Path $taskOutput 'TeleAgentAccounts.exe'
    Copy-Item -LiteralPath (Join-Path $taskBuild 'TeleAgentAccounts.exe') -Destination $taskExecutable -Force
    if (-not $SkipRootCopy) {
        Copy-Item -LiteralPath $taskExecutable -Destination '.\TeleAgentAccounts.exe' -Force
    }
    Write-Host "已生成 $taskExecutable（无需旁置 DLL）"
} finally { Pop-Location }
