# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][string]$ImporterPath,
    [string]$ProfilerDll = (Join-Path $PSScriptRoot '..\bin\d3d11.dll'),
    [switch]$Restore
)
$ErrorActionPreference = 'Stop'
$stockHash = '74a01bc38bbc2f85e4c15b7c22976f5a68ce3686faa2b973ed4e944c7c70976c'
if (Get-Process -Name 'ZenlessZoneZero','ZenlessZoneZeroBeta','XXMI Launcher' -ErrorAction SilentlyContinue) {
    throw 'Close the game and XXMI Launcher before switching DLLs.'
}
$dir = (Resolve-Path -LiteralPath $ImporterPath).Path
$dll = Join-Path $dir 'd3d11.dll'
$backup = Join-Path $dir 'd3d11.zzz-profiler-stock'
$marker = Join-Path $dir 'd3d11.zzz-profiler-hash'
$ini = Join-Path $dir 'zzz-profiler.ini'
if (!(Test-Path -LiteralPath (Join-Path $dir 'd3dx.ini') -PathType Leaf)) {
    throw 'Select the ZZMI importer folder containing d3dx.ini.'
}
if ($Restore) {
    if (!(Test-Path -LiteralPath $backup -PathType Leaf) -or
        (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne $stockHash) {
        throw 'A verified stock v1.3.3 backup is required. Use the launcher package repair if it is missing.'
    }
    $current = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
    if ($current -ne $stockHash -and (!(Test-Path -LiteralPath $marker) -or
        (Get-Content -LiteralPath $marker -Raw).Trim() -ne $current)) {
        throw 'The current d3d11.dll differs from the installed profiler. Use the launcher package repair.'
    }
    if ($PSCmdlet.ShouldProcess($dll, 'Restore verified stock XXMI v1.3.3')) {
        if ($current -ne $stockHash) {
            $staged = Join-Path $dir ('stock-' + [Guid]::NewGuid().ToString('N') + '.tmp')
            try {
                Copy-Item -LiteralPath $backup -Destination $staged
                [IO.File]::Replace($staged, $dll, $null)
            } finally {
                Remove-Item -LiteralPath $staged -ErrorAction SilentlyContinue
            }
        }
        Remove-Item -LiteralPath $backup, $marker -ErrorAction SilentlyContinue
        Write-Host 'Stock XXMI restored. Return the launcher to its normal signed-library setting.'
    }
    return
}

if ((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -ne $stockHash) {
    throw 'This capture build is pinned to the official XXMI Libraries v1.3.3 DLL. Current DLL was left intact.'
}
$ProfilerDll = (Resolve-Path -LiteralPath $ProfilerDll).Path
$hash = (Get-FileHash -LiteralPath $ProfilerDll -Algorithm SHA256).Hash
if ($hash -eq $stockHash) { throw 'Select the compiled profiler DLL, not the stock DLL.' }
if ((Test-Path -LiteralPath $backup) -or (Test-Path -LiteralPath $marker)) {
    throw 'Restore the previous profiler installation before starting another one.'
}
if ($PSCmdlet.ShouldProcess($dll, 'Back up stock and install the capture build')) {
    $staged = Join-Path $dir ('profiler-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Copy-Item -LiteralPath $ProfilerDll -Destination $staged
        Set-Content -LiteralPath $marker -Value $hash -Encoding Ascii
        [IO.File]::Replace($staged, $dll, $backup)
        if (!(Test-Path -LiteralPath $ini)) {
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\config\zzz-profiler.ini') -Destination $ini
        }
        Write-Host "Installed $dll"
    } finally {
        Remove-Item -LiteralPath $staged -ErrorAction SilentlyContinue
        if (!(Test-Path -LiteralPath $backup)) {
            Remove-Item -LiteralPath $marker -ErrorAction SilentlyContinue
        }
    }
}
