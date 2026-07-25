#requires -Version 7.0

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$SourcePath = $PSScriptRoot
$InstallPath = Join-Path $env:LOCALAPPDATA 'Programs\KeepAwake'
$InstallIconsPath = Join-Path $InstallPath 'Icons'

$RuntimeFiles = @(
    'KeepAwake.ps1',
    'Launch-KeepAwake.ps1',
    'Uninstall.ps1',
    'Icons\KeepAwake-Off.ico',
    'Icons\KeepAwake-On.ico'
)

foreach ($RelativePath in $RuntimeFiles) {
    $SourceFile = Join-Path $SourcePath $RelativePath

    if (-not (Test-Path -LiteralPath $SourceFile)) {
        throw "Не найден обязательный файл: $SourceFile"
    }
}

$PwshCommand = Get-Command pwsh.exe -ErrorAction SilentlyContinue

if ($null -eq $PwshCommand) {
    throw 'PowerShell 7 не найден. Установите PowerShell 7 и повторите установку.'
}

$RunningInstalledCopies = @(
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

if ($RunningInstalledCopies.Count -gt 0) {
    throw 'Установленная копия KeepAwake запущена. Закройте её через значок в трее.'
}

New-Item `
    -ItemType Directory `
    -Path $InstallIconsPath `
    -Force | Out-Null

foreach ($RelativePath in $RuntimeFiles) {
    $SourceFile = Join-Path $SourcePath $RelativePath
    $DestinationFile = Join-Path $InstallPath $RelativePath

    Copy-Item `
        -LiteralPath $SourceFile `
        -Destination $DestinationFile `
        -Force
}

foreach ($ScriptName in @(
    'KeepAwake.ps1',
    'Launch-KeepAwake.ps1',
    'Uninstall.ps1'
)) {
    $ScriptPath = Join-Path $InstallPath $ScriptName
    $Tokens = $null
    $ParseErrors = $null

    [System.Management.Automation.Language.Parser]::ParseFile(
        $ScriptPath,
        [ref] $Tokens,
        [ref] $ParseErrors
    ) | Out-Null

    if (@($ParseErrors).Count -gt 0) {
        $Details = $ParseErrors |
            ForEach-Object {
                "Строка $($_.Extent.StartLineNumber): $($_.Message)"
            }

        throw "Ошибка синтаксиса в $ScriptName`n$($Details -join "`n")"
    }
}

$DesktopPath = [Environment]::GetFolderPath('Desktop')
$ShortcutPath = Join-Path $DesktopPath 'KeepAwake.lnk'
$MainScriptPath = Join-Path $InstallPath 'KeepAwake.ps1'
$IconPath = Join-Path $InstallIconsPath 'KeepAwake-Off.ico'

$Shell = New-Object -ComObject WScript.Shell
$Shortcut = $Shell.CreateShortcut($ShortcutPath)

try {
    $Shortcut.TargetPath = $PwshCommand.Source
    $Shortcut.Arguments = (
        '-NoProfile -STA -WindowStyle Hidden ' +
        '-ExecutionPolicy Bypass -File "{0}"'
    ) -f $MainScriptPath

    $Shortcut.WorkingDirectory = $InstallPath
    $Shortcut.IconLocation = "$IconPath,0"
    $Shortcut.Description = 'KeepAwake — управление автоматическим сном'
    $Shortcut.WindowStyle = 7
    $Shortcut.Save()
}
finally {
    [System.Runtime.InteropServices.Marshal]::FinalReleaseComObject(
        $Shortcut
    ) | Out-Null

    [System.Runtime.InteropServices.Marshal]::FinalReleaseComObject(
        $Shell
    ) | Out-Null
}

if (-not (Test-Path -LiteralPath $ShortcutPath)) {
    throw 'Windows не создала ярлык KeepAwake.'
}

$InstalledFiles = foreach ($RelativePath in $RuntimeFiles) {
    Get-Item -LiteralPath (Join-Path $InstallPath $RelativePath)
}

$TotalBytes = (
    $InstalledFiles |
        Measure-Object -Property Length -Sum
).Sum

Write-Host ''
Write-Host 'KeepAwake установлен.' -ForegroundColor Green
Write-Host "Каталог: $InstallPath"
Write-Host "Ярлык:  $ShortcutPath"
Write-Host ''

$InstalledFiles |
    Select-Object Name, Length, FullName |
    Format-Table -AutoSize

Write-Host (
    'Общий размер: {0:N0} байт ({1:N2} КиБ)' -f `
        $TotalBytes,
        ($TotalBytes / 1KB)
) -ForegroundColor Green
