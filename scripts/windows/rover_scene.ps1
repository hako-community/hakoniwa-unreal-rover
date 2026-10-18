# rover_scene.ps1 — 月面の場面（ROV-15）を組む・撮る・地形の高さを検査する。
#
#   pwsh scripts\windows\rover_scene.ps1 -Build              # 地形タイル・位置暦・GLB から /Game/Rover/Levels/LunarSouthPole を作り直す
#   pwsh scripts\windows\rover_scene.ps1 -Shots -Probe       # -game で開き、検査点の高さを測って撮影（Saved\RoverShots\<時刻>\）
#   pwsh scripts\windows\rover_scene.ps1 -Open               # エディタで場面を開く
#   pwsh scripts\windows\rover_scene.ps1 -Compare            # 参照カメラ（mujoco-rover の bodies\*\generated\ref_cameras.json）ごとに
#                                                            # 場面を作って撮り、参照の絵と並べる（mujoco-rover の tools\compare_ref_shots.py）
#
# 地形と位置暦は hakoniwa-mujoco-rover が作る（tools/dem_tile.py・tools/sun_ephem.py。D13 で simenv-data へ移す予定）。
# 先に scripts\windows\setup_build.ps1 でビルドしておく。
[CmdletBinding()]
param(
    [switch]$Build,
    [switch]$Shots,
    [switch]$Probe,
    [switch]$Open,
    [switch]$Compare,
    [string]$Tile = 'lola_site01_t01',
    [string]$MujocoRover,
    [string]$Glb,
    [string]$EngineRoot = (Join-Path $env:ProgramFiles 'Epic Games\UE_5.8'),
    [int]$ResX = 1920,
    [int]$ResY = 1080
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$uproject = Join-Path $root 'HakoniwaRover.uproject'
if (-not $MujocoRover) { $MujocoRover = Join-Path $root '..\hakoniwa-mujoco-rover' }
$MujocoRover = (Resolve-Path $MujocoRover).Path
$tileDir = Join-Path $MujocoRover "models\worlds\$Tile"
$grid = Join-Path $tileDir "$Tile.grid.json"
$sun = Join-Path $tileDir "$Tile.sun.json"
# ローバの GLB：-Glb で指定。無ければ mujoco-rover の bodies\<名前>\source\ にある 1 つを使う
if (-not $Glb) {
    $found = @(Get-ChildItem -Path (Join-Path $MujocoRover 'bodies\*\source\*.glb') -ErrorAction SilentlyContinue)
    if ($found.Count -ne 1) { throw "ローバの GLB が 1 つに決まりません（$($found.Count) 個）。-Glb で指定してください" }
    $Glb = $found[0].FullName
}
$glb = $Glb
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
$map = '/Game/Rover/Levels/LunarSouthPole'
foreach ($f in @($grid, $sun, $editor)) { if (-not (Test-Path $f)) { throw "見つかりません: $f" } }
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$logDir = Join-Path $root 'Saved\RoverLogs'
New-Item -ItemType Directory -Force $logDir | Out-Null

function Invoke-Editor([string[]]$Arguments, [string]$Log) {
    Write-Host "[rover_scene] $editor $($Arguments -join ' ')"
    $p = Start-Process -FilePath $editor -ArgumentList ($Arguments + "-abslog=$Log") -PassThru -Wait
    Select-String -Path $Log -Pattern 'LogRover' | ForEach-Object { Write-Host "  $($_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '')" }
    if ($p.ExitCode -ne 0) { throw "Unreal が終了コード $($p.ExitCode) で終わりました（$Log）" }
}

if ($Build) {
    Invoke-Editor @($uproject, '-unattended', '-nosplash', '-RoverBuildScene', '-RoverQuit',
        "-RoverGrid=$grid", "-RoverSun=$sun", "-RoverGlb=$glb", "-RoverMap=$map") (Join-Path $logDir "build_$stamp.log")
}

if ($Shots -or $Probe) {
    $out = Join-Path $root "Saved\RoverShots\$stamp"
    New-Item -ItemType Directory -Force $out | Out-Null
    $gameArgs = @($uproject, $map, '-game', '-windowed', "-ResX=$ResX", "-ResY=$ResY", '-nosplash', '-unattended')
    if ($Shots) { $gameArgs += "-RoverShot=$out" }
    if ($Probe) {
        $points = Join-Path $MujocoRover "models\worlds\$Tile.check_points.csv"
        if (-not (Test-Path $points)) { throw "検査点がありません（先に tools\check_terrain.py を流す）: $points" }
        $gameArgs += @("-RoverProbeIn=$points", "-RoverProbeOut=$out\unreal_heights.csv")
    }
    Invoke-Editor $gameArgs (Join-Path $out 'game.log')
    if ($Probe) {
        $py = Join-Path $MujocoRover 'deps\py\Scripts\python.exe'
        & $py -I (Join-Path $MujocoRover 'tools\check_terrain.py') (Join-Path $MujocoRover "models\worlds\$Tile.xml") --unreal "$out\unreal_heights.csv"
        if ($LASTEXITCODE -ne 0) { throw '地形の高さの検査が不合格です' }
    }
    Write-Host "[rover_scene] 出力: $out"
}

if ($Compare) {
    # 参照カメラ 1 台ごとに別の地図（/Game/Rover/Levels/LunarSouthPole_Ref<番号>。git 管理外）を作り、撮って並べる
    $refs = @(Get-ChildItem -Path (Join-Path $MujocoRover 'bodies\*\generated\ref_cameras.json') -ErrorAction SilentlyContinue)
    if ($refs.Count -ne 1) { throw "参照カメラの表が 1 つに決まりません（mujoco-rover で tools\ref_cameras.py を先に）" }
    $refJson = $refs[0].FullName
    $keys = (Get-Content -LiteralPath $refJson -Raw -Encoding UTF8 | ConvertFrom-Json).cameras.PSObject.Properties.Name
    $dirs = @()
    foreach ($k in $keys) {
        $refMap = "/Game/Rover/Levels/LunarSouthPole_Ref$k"
        Invoke-Editor @($uproject, '-unattended', '-nosplash', '-RoverBuildScene', '-RoverQuit',
            "-RoverGrid=$grid", "-RoverSun=$sun", "-RoverGlb=$glb", "-RoverMap=$refMap",
            "-RoverRefCams=$refJson", "-RoverRef=$k") (Join-Path $logDir "build_ref$($k)_$stamp.log")
        $out = Join-Path $root "Saved\RoverShots\$($stamp)_ref$k"
        New-Item -ItemType Directory -Force $out | Out-Null
        Invoke-Editor @($uproject, $refMap, '-game', '-windowed', "-ResX=$ResX", "-ResY=$ResY", '-nosplash', '-unattended',
            "-RoverShot=$out") (Join-Path $out 'game.log')
        $dirs += $out
    }
    $py = Join-Path $MujocoRover 'deps\py\Scripts\python.exe'
    & $py -I (Join-Path $MujocoRover 'tools\compare_ref_shots.py') @dirs
}

if ($Open) {
    Start-Process -FilePath $editor -ArgumentList @($uproject, $map)
}
