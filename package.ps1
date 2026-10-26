$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $taskName = 'TeleAgent账号工作台-v0.5.0-win64'
    $taskPackage = Join-Path $PSScriptRoot (Join-Path 'dist' $taskName)
    $taskZip = Join-Path $PSScriptRoot (Join-Path 'dist' ($taskName + '.zip'))
    New-Item -ItemType Directory -Path $taskPackage -Force | Out-Null
    # Explicit allowlist: never copy an application directory recursively.
    Copy-Item -LiteralPath 'TeleAgentAccounts.exe','WebView2Loader.dll' -Destination $taskPackage -Force
    Copy-Item -LiteralPath 'third_party\webview2\LICENSE.txt' -Destination (Join-Path $taskPackage 'WebView2-LICENSE.txt') -Force
    Copy-Item -LiteralPath '使用说明-v0.5.0.txt' -Destination (Join-Path $taskPackage '使用说明.txt') -Force
    $taskAllowed = @('TeleAgentAccounts.exe','WebView2Loader.dll','WebView2-LICENSE.txt','使用说明.txt')
    $taskUnexpected = @(Get-ChildItem -LiteralPath $taskPackage -Force | Where-Object { $_.PSIsContainer -or $_.Name -notin $taskAllowed })
    if ($taskUnexpected.Count) { throw '分发目录含有清单外内容，已停止打包。请使用新的干净输出目录。' }
    Compress-Archive -LiteralPath @($taskAllowed | ForEach-Object { Join-Path $taskPackage $_ }) -DestinationPath $taskZip -Force
    Get-FileHash -LiteralPath $taskZip -Algorithm SHA256 | Format-List
    Write-Host "已生成 $taskZip"
} finally { Pop-Location }
