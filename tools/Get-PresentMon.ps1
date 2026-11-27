# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([string]$Destination = (Join-Path $PSScriptRoot '..\third-party\PresentMon'))
$ErrorActionPreference = 'Stop'
$expected = 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
$exe = Join-Path $Destination 'PresentMon-2.6.0-x64.exe'
$temp = [IO.Path]::GetTempFileName()
try {
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/GameTechDev/PresentMon/releases/download/v2.6.0/PresentMon-2.6.0-x64.exe' -OutFile $temp
    if ((Get-FileHash -LiteralPath $temp -Algorithm SHA256).Hash -ne $expected) {
        throw 'Downloaded PresentMon did not match the release asset digest.'
    }
    Copy-Item -LiteralPath $temp -Destination $exe -Force
    Write-Host "Verified and saved $exe"
} finally {
    Remove-Item -LiteralPath $temp -ErrorAction SilentlyContinue
}
