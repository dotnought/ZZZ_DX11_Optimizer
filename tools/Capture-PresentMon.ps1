# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Roscaelifer', 'SixthStreet', 'Combat')][string]$Scene,
    [Parameter(Mandatory)][ValidateSet('vanilla', 'xxmi-empty', 'xxmi-mods', 'profiler-off', 'profiler-l1', 'profiler-l2', 'profiler-l3')][string]$Condition,
    [Parameter(Mandatory)][string]$SettingsId,
    [Parameter(Mandatory)][string]$GameVersion,
    [Parameter(Mandatory)][string]$Driver,
    [Parameter(Mandatory)][string]$Route,
    [ValidateSet('warm', 'cold')][string]$Cache = 'warm',
    [ValidateRange(1, 120)][int]$Seconds = 45,
    [string]$ImporterPath,
    [string]$GameProcessName = 'ZenlessZoneZero',
    [string]$OutputRoot = (Join-Path (Get-Location) 'runs'),
    [string]$PresentMon = (Join-Path $PSScriptRoot '..\third-party\PresentMon\PresentMon-2.6.0-x64.exe')
)
$ErrorActionPreference = 'Stop'
$expected = 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'
if (!(Test-Path -LiteralPath $PresentMon -PathType Leaf)) {
    throw 'PresentMon 2.6.0 x64 is missing. Run Get-PresentMon.ps1 or specify its full path.'
}
if ((Get-FileHash -LiteralPath $PresentMon -Algorithm SHA256).Hash -ne $expected) {
    throw 'PresentMon hash differs from the pinned GitHub release.'
}

$nativeLevel = 0
if ($Condition -match '^profiler-l([123])$') { $nativeLevel = [int]$Matches[1] }
if ($nativeLevel -eq 3 -and $Seconds -gt 20) { throw 'Use -Seconds 20 or less at Level 3.' }
$captureDir = $null
if ($nativeLevel) {
    if (!$ImporterPath) { throw 'Specify -ImporterPath for a native capture.' }
    $ImporterPath = (Resolve-Path -LiteralPath $ImporterPath).Path
    $captureDir = Join-Path $ImporterPath 'zzz-captures'
    $ini = Join-Path $ImporterPath 'zzz-profiler.ini'
    if (!(Test-Path -LiteralPath $ini -PathType Leaf)) { throw "Missing capture configuration at $ini" }
    $contents = Get-Content -LiteralPath $ini -Raw
    if ($contents -notmatch "(?m)^\s*Level\s*=\s*$nativeLevel\s*$" -or
        $contents -notmatch "(?m)^\s*Seconds\s*=\s*$Seconds\s*$") {
        throw 'Match Level and Seconds in zzz-profiler.ini to this run and restart the game.'
    }
}

$games = @(Get-Process -Name $GameProcessName -ErrorAction SilentlyContinue)
if ($games.Count -ne 1) { throw "Start exactly one $GameProcessName process before arming a capture." }
$game = $games[0]
$started = [DateTime]::UtcNow
$runName = '{0:yyyyMMdd-HHmmss-fff}-{1}-{2}' -f $started, $Scene, $Condition
$run = (New-Item -ItemType Directory -Path (Join-Path $OutputRoot $runName)).FullName
$csv = Join-Path $run 'presentmon.csv'
Write-Host "Return to the game. Hold Ctrl+Alt+F8 briefly at the start of the route, then record for $Seconds seconds."
Write-Host "Recording $Scene / $Condition for PID $($game.Id)."
$pmArgs = @('--process_id', $game.Id, '--output_file', $csv,
    '--session_name', "ZZZDX11-$($game.Id)-$($started.Ticks)", '--hotkey', 'CTRL+ALT+F8',
    '--timed', $Seconds, '--terminate_after_timed', '--terminate_on_proc_exit',
    '--qpc_time', '--no_console_stats', '--no_track_input')
& $PresentMon @pmArgs
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $csv -PathType Leaf)) {
    throw 'PresentMon did not finish successfully. Check the console output and Windows ETW permissions.'
}

$record = [ordered]@{
    scene = $Scene
    condition = $Condition
    settings_id = $SettingsId
    game_version = $GameVersion
    driver = $Driver
    route = $Route
    cache = $Cache
    planned_seconds = $Seconds
    pid = $game.Id
    created_utc = $started.ToString('o')
    presentmon = 'presentmon.csv'
}
if ($nativeLevel) {
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    do {
        $results = @(Get-ChildItem -LiteralPath $captureDir -Filter 'capture-*.json' -File -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTimeUtc -ge $started } |
            Sort-Object LastWriteTimeUtc -Descending)
        foreach ($file in $results) {
            $capture = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
            if ($capture.pid -ne $game.Id -or $capture.level -ne $nativeLevel) { continue }
            foreach ($suffix in @('-frames.csv', '-apis.csv', '-gpu.csv', '-memory.csv', '.json')) {
                $path = Join-Path $captureDir ($file.BaseName + $suffix)
                Copy-Item -LiteralPath $path -Destination $run
            }
            $record.native = $file.Name
            break
        }
        if (!$record.Contains('native')) { Start-Sleep -Milliseconds 500 }
    } until ($record.Contains('native') -or [DateTime]::UtcNow -ge $deadline)
    if (!$record.Contains('native')) {
        Write-Warning 'No completed native capture matched this run. The PresentMon baseline is available.'
        $record.native_missing = $true
    }
}
$record | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $run 'run.json') -Encoding UTF8
Write-Host "Saved $run"
