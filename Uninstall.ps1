#requires -Version 7.0

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$InstallPath = Join-Path $env:LOCALAPPDATA 'Programs\KeepAwake'
$DesktopPath = [Environment]::GetFolderPath('Desktop')
$ShortcutPath = Join-Path $DesktopPath 'KeepAwake.lnk'

$RunningProcesses = @(
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $null -ne $_.CommandLine -and
            $_.CommandLine.IndexOf(
                $InstallPath,
                [StringComparison]::OrdinalIgnoreCase
            ) -ge 0 -and
            $_.CommandLine.IndexOf(
                'KeepAwake.ps1',
                [StringComparison]::OrdinalIgnoreCase
            ) -ge 0
        }
)

if ($RunningProcesses.Count -gt 0) {
    $ProcessIds = $RunningProcesses.ProcessId -join ', '

    throw "KeepAwake запущен. Закройте его через трей. PID: $ProcessIds"
}

if (Test-Path -LiteralPath $ShortcutPath) {
    Remove-Item `
        -LiteralPath $ShortcutPath `
        -Force
}

if (Test-Path -LiteralPath $InstallPath) {
    Remove-Item `
        -LiteralPath $InstallPath `
        -Recurse `
        -Force
}

Write-Host 'KeepAwake удалён.' -ForegroundColor Green
Write-Host "Каталог: $InstallPath"
Write-Host "Ярлык:  $ShortcutPath"
Write-Host 'Исходный Git-репозиторий не изменялся.'
