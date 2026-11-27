# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([string]$OutputDir = (Join-Path $PSScriptRoot '..\dist'))
$ErrorActionPreference = 'Stop'
Get-Command cl.exe -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$destination = (Resolve-Path -LiteralPath $OutputDir).Path
$exe = Join-Path $destination 'Start Here.exe'
$obj = Join-Path $destination 'FirstCapture.obj'
$source = Join-Path $PSScriptRoot 'windows\FirstCapture.cpp'
& cl.exe /nologo /std:c++17 /EHsc /O2 /MT "/Fo$obj" "/Fe$exe" $source /link bcrypt.lib ole32.lib uuid.lib
if ($LASTEXITCODE -ne 0) { throw "Could not build Start Here.exe (exit $LASTEXITCODE)" }
Write-Host "Built $exe"
