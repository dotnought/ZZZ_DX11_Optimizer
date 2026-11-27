# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\dist'),
    [string]$MSBuild = 'MSBuild.exe'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pin = '8bd31f74727f033b3d9049509e19da3563c8b98c'
Get-Command $MSBuild -ErrorAction Stop | Out-Null
function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
$checkout = Join-Path $root ('build\xxmi-v1.3.3-' + [Guid]::NewGuid().ToString('N'))
Invoke-Checked git @('clone', '--depth', '1', '--branch', 'v1.3.3',
    'https://github.com/SpectrumQT/XXMI-Libs-Package.git', $checkout)
$commit = & git -C $checkout rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $commit -ne $pin) { throw "XXMI source did not match $pin" }
Invoke-Checked git @('-C', $checkout, 'apply', '--check', (Join-Path $root 'patches\xxmi-v1.3.3.patch'))
Invoke-Checked git @('-C', $checkout, 'apply', (Join-Path $root 'patches\xxmi-v1.3.3.patch'))
Copy-Item -Path (Join-Path $root 'native\ZzzCapture.*') -Destination (Join-Path $checkout 'DirectX11')
Push-Location $checkout
try {
    Invoke-Checked $MSBuild @('StereovisionHacks.sln', '/m', '/p:Configuration=Zip Release',
        '/p:Platform=x64', '/v:minimal')
} finally { Pop-Location }
$bin = (New-Item -ItemType Directory -Path (Join-Path $OutputDir 'bin') -Force).FullName
$dll = Join-Path $bin 'd3d11.dll'
Copy-Item -LiteralPath (Join-Path $checkout 'x64\Zip Release\d3d11.dll') -Destination $dll -Force
(Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash | Set-Content (Join-Path $bin 'd3d11.sha256') -Encoding Ascii
Write-Host "Built $dll from $pin"
