# 编译两样工具：PbzExport（读游戏资源，.NET 10 + CUE4Parse）和 UE 编辑器 C++ 模块 PbzModEditor。
# 用法：pwsh tools\build-tools.ps1 [-SkipExport] [-SkipEditor]
param([switch]$SkipExport, [switch]$SkipEditor)
. "$PSScriptRoot\common.ps1"

if (-not $SkipExport) {
    Step '编译 PbzExport（首次会还原 NuGet 包并编译 CUE4Parse，较慢）'
    $log = Join-Path $PbzLogs 'pbzexport-build.log'
    & $PbzDotnet build (Join-Path $PSScriptRoot 'PbzExport\PbzExport.csproj') -c Release -nologo -v:q -clp:ErrorsOnly -p:UseSharedCompilation=false -nodeReuse:false "-p:RestoreConfigFile=$PSScriptRoot\nuget.config" 2>&1 | Out-File -Encoding utf8 $log
    if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 30; throw "PbzExport 编译失败（见 $log）" }
    Check (Test-Path $PbzExportExe) "没有生成 $PbzExportExe"
    $info = & $PbzExportExe info 2>&1
    if ($LASTEXITCODE -ne 0) { $info | Select-Object -Last 10; throw 'PbzExport 挂载游戏失败：检查游戏目录、AES、usmap 是否和游戏版本一致' }
    $info | Select-Object -First 5
}

if (-not $SkipEditor) {
    Step '编译 UE 编辑器模块 PbzModEditor'
    $log = Join-Path $PbzLogs 'ubt-build.log'
    # UBT 用引擎自带的 .NET，去掉为 PbzExport 设置的 .NET 变量，避免干扰
    $saved = @{}; foreach ($k in 'DOTNET_ROOT', 'DOTNET_CLI_HOME', 'MSBUILDDISABLENODEREUSE') { $saved[$k] = [Environment]::GetEnvironmentVariable($k); Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    & "$PbzUERoot\Engine\Build\BatchFiles\Build.bat" ProjectPBZEditor Win64 Development "-Project=$PbzUProject" -WaitMutex -NoHotReload "-Log=$PbzLogs\UBT.txt" 2>&1 | Out-File -Encoding utf8 $log
    $code = $LASTEXITCODE
    foreach ($k in $saved.Keys) { if ($saved[$k]) { Set-Item "Env:$k" $saved[$k] } }
    Select-String -Path $log -Pattern 'error|Result:|toolchain' | Select-Object -First 20 | ForEach-Object { $_.Line }
    Check ($code -eq 0) "编辑器模块编译失败（见 $log）。常见原因：MSVC 低于 14.44.35211、缺 Windows SDK 或 NetFxSDK"
}
Step '完成'
