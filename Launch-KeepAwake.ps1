Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$KeepAwakeScript = Join-Path $PSScriptRoot 'KeepAwake.ps1'
$PwshPath = (Get-Command pwsh.exe -ErrorAction Stop).Source

if (-not (Test-Path -LiteralPath $KeepAwakeScript)) {
    throw "Не найден KeepAwake.ps1: $KeepAwakeScript"
}

Start-Process `
    -FilePath $PwshPath `
    -ArgumentList @(
        '-NoProfile',
        '-STA',
        '-ExecutionPolicy', 'Bypass',
        '-File', $KeepAwakeScript
    ) `
    -WindowStyle Hidden
