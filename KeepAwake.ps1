#requires -Version 7.0

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

if (-not ([System.Management.Automation.PSTypeName]'KeepAwakeNative').Type) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class KeepAwakeNative
{
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint SetThreadExecutionState(uint esFlags);
}
"@
}

# ES_CONTINUOUS сохраняет состояние до следующего вызова
# или до завершения процесса.
$script:ES_CONTINUOUS      = [Convert]::ToUInt32('80000000', 16)
$script:ES_SYSTEM_REQUIRED = [uint32]1

$script:AwakeEnabled = $false

$script:TimerRunning = $false
$script:TimerDeadline = $null
$script:CountdownTimer = $null
$script:TimerStatusItem = $null
$script:TimerMenuItem = $null
$script:TimerCancelItem = $null

$script:ErrorLogPath = Join-Path $PSScriptRoot 'KeepAwake-error.log'

$script:IconOffPath = Join-Path $PSScriptRoot 'Icons\KeepAwake-Off.ico'
$script:IconOnPath  = Join-Path $PSScriptRoot 'Icons\KeepAwake-On.ico'

if (-not (Test-Path -LiteralPath $script:IconOffPath)) {
    throw "Не найдена иконка обычного режима: $script:IconOffPath"
}

if (-not (Test-Path -LiteralPath $script:IconOnPath)) {
    throw "Не найдена иконка запрета сна: $script:IconOnPath"
}

$script:IconOff = [System.Drawing.Icon]::new($script:IconOffPath)
$script:IconOn  = [System.Drawing.Icon]::new($script:IconOnPath)

function Write-KeepAwakeError {
    param(
        [Parameter(Mandatory)]
        [string] $Message
    )

    try {
        $record = '{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $Message
        Add-Content -LiteralPath $script:ErrorLogPath -Value $record -Encoding utf8
    }
    catch {
        # Ошибка записи журнала не должна аварийно завершать утилиту.
    }
}

function Show-KeepAwakeNotification {
    param(
        [Parameter(Mandatory)]
        [string] $Text
    )

    try {
        $script:NotifyIcon.ShowBalloonTip(
            2000,
            'KeepAwake',
            $Text,
            [System.Windows.Forms.ToolTipIcon]::Info
        )
    }
    catch {
        Write-KeepAwakeError "Не удалось показать уведомление: $($_.Exception.Message)"
    }
}

function Format-KeepAwakeRemaining {
    param(
        [Parameter(Mandatory)]
        [TimeSpan] $Remaining
    )

    $totalSeconds = [Math]::Max(
        0,
        [Math]::Ceiling($Remaining.TotalSeconds)
    )

    $hours = [Math]::Floor($totalSeconds / 3600)
    $minutes = [Math]::Floor(($totalSeconds % 3600) / 60)
    $seconds = $totalSeconds % 60

    return '{0:00}:{1:00}:{2:00}' -f `
        [int] $hours,
        [int] $minutes,
        [int] $seconds
}

function Update-KeepAwakeTimerDisplay {
    if (
        -not $script:TimerRunning -or
        $null -eq $script:TimerDeadline
    ) {
        if ($null -ne $script:TimerStatusItem) {
            if ($script:AwakeEnabled) {
                $script:TimerStatusItem.Text = 'Таймер: без ограничения'
            }
            else {
                $script:TimerStatusItem.Text = 'Таймер: не установлен'
            }
        }

        if ($null -ne $script:TimerCancelItem) {
            $script:TimerCancelItem.Enabled = $false
        }

        return
    }

    $remaining = $script:TimerDeadline - [DateTime]::Now

    if ($remaining.TotalSeconds -le 0) {
        Stop-KeepAwakeTimer -Expired
        return
    }

    $countdown = Format-KeepAwakeRemaining -Remaining $remaining

    $script:TimerStatusItem.Text = "Таймер: $countdown"
    $script:TimerCancelItem.Enabled = $true

    if ($script:AwakeEnabled) {
        # NotifyIcon.Text имеет ограничение длины,
        # поэтому используем компактный вариант.
        $script:NotifyIcon.Text = "KeepAwake — $countdown"
    }
}

function Update-KeepAwakeInterface {
    if ($script:AwakeEnabled) {
        $script:StatusItem.Text = 'Статус: сон запрещён'
        $script:EnableItem.Enabled = $false
        $script:DisableItem.Enabled = $true

        $script:NotifyIcon.Text = 'KeepAwake — сон запрещён'
        $script:NotifyIcon.Icon = $script:IconOn
    }
    else {
        $script:StatusItem.Text = 'Статус: сон разрешён'
        $script:EnableItem.Enabled = $true
        $script:DisableItem.Enabled = $false

        $script:NotifyIcon.Text = 'KeepAwake — сон разрешён'
        $script:NotifyIcon.Icon = $script:IconOff
    }

    Update-KeepAwakeTimerDisplay
}

function Set-KeepAwakeState {
    param(
        [Parameter(Mandatory)]
        [bool] $Enabled,

        [switch] $SuppressNotification
    )

    if ($Enabled) {
        # Запрещаем только автоматический сон системы.
        # ES_DISPLAY_REQUIRED намеренно не используется:
        # экран сможет отключаться по обычному таймеру.
        $flags = $script:ES_CONTINUOUS -bor $script:ES_SYSTEM_REQUIRED
    }
    else {
        # Снимаем запрос и возвращаем стандартное поведение Windows.
        $flags = $script:ES_CONTINUOUS
    }

    $result = [KeepAwakeNative]::SetThreadExecutionState($flags)

    if ($result -eq 0) {
        $message = 'Windows не приняла запрос на изменение режима сна.'
        Write-KeepAwakeError $message

        [System.Windows.Forms.MessageBox]::Show(
            $message,
            'KeepAwake',
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Error
        ) | Out-Null

        return $false
    }

    $script:AwakeEnabled = $Enabled
    Update-KeepAwakeInterface

    if (-not $SuppressNotification) {
        if ($Enabled) {
            Show-KeepAwakeNotification 'Запрет сна включён. Экран может отключаться.'
        }
        else {
            Show-KeepAwakeNotification 'Запрет сна выключен. Действуют настройки Windows.'
        }
    }

    return $true
}

function Stop-KeepAwakeTimer {
    param(
        [switch] $Expired,
        [switch] $Silent
    )

    $wasRunning = $script:TimerRunning

    if ($null -ne $script:CountdownTimer) {
        $script:CountdownTimer.Stop()
    }

    $script:TimerRunning = $false
    $script:TimerDeadline = $null

    if ($Expired) {
        $disabled = Set-KeepAwakeState `
            -Enabled $false `
            -SuppressNotification

        if ($disabled) {
            Show-KeepAwakeNotification `
                'Таймер завершён. Запрет сна автоматически выключен.'
        }

        return
    }

    Update-KeepAwakeInterface

    if ($wasRunning -and -not $Silent) {
        Show-KeepAwakeNotification `
            'Таймер отменён. Запрет сна остаётся включённым.'
    }
}

function Start-KeepAwakeTimer {
    param(
        [Parameter(Mandatory)]
        [ValidateSet(30, 60, 120, 180, 240, 360, 480, 600)]
        [int] $Minutes,

        [switch] $SuppressNotification
    )

    $enabled = Set-KeepAwakeState `
        -Enabled $true `
        -SuppressNotification

    if (-not $enabled) {
        return $false
    }

    $script:TimerDeadline = [DateTime]::Now.AddMinutes($Minutes)
    $script:TimerRunning = $true

    $script:CountdownTimer.Start()
    Update-KeepAwakeInterface

    if ($Minutes -eq 30) {
        $durationText = '30 минут'
    }
    else {
        $hours = [int] ($Minutes / 60)
        $hourWord = 'часов'

        if ($hours -eq 1) {
            $hourWord = 'час'
        }

        if ($hours -ge 2 -and $hours -le 4) {
            $hourWord = 'часа'
        }

        $durationText = "$hours $hourWord"
    }

    if (-not $SuppressNotification) {
        $endTime = $script:TimerDeadline.ToString('HH:mm:ss')

        Show-KeepAwakeNotification (
            "Запрет сна включён на $durationText. " +
            "Автоотключение в $endTime."
        )
    }

    return $true
}

[System.Windows.Forms.Application]::EnableVisualStyles()

# Запрещаем запуск нескольких копий программы.
$createdNew = $false
$mutex = [System.Threading.Mutex]::new(
    $true,
    'Local\KeepAwake.PowerShell.Tray',
    [ref] $createdNew
)

if (-not $createdNew) {
    [System.Windows.Forms.MessageBox]::Show(
        'KeepAwake уже запущен.',
        'KeepAwake',
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Information
    ) | Out-Null

    $mutex.Dispose()
    return
}

$appContext = [System.Windows.Forms.ApplicationContext]::new()
$menu = [System.Windows.Forms.ContextMenuStrip]::new()

$script:StatusItem = [System.Windows.Forms.ToolStripMenuItem]::new(
    'Статус: сон разрешён'
)
$script:StatusItem.Enabled = $false

$script:TimerStatusItem = [System.Windows.Forms.ToolStripMenuItem]::new(
    'Таймер: не установлен'
)
$script:TimerStatusItem.Enabled = $false

$script:EnableItem = [System.Windows.Forms.ToolStripMenuItem]::new(
    'Включить запрет сна'
)

$script:DisableItem = [System.Windows.Forms.ToolStripMenuItem]::new(
    'Выключить запрет сна'
)

$script:TimerMenuItem = [System.Windows.Forms.ToolStripMenuItem]::new(
    'Автоотключение'
)

$timerOptions = @(
    [pscustomobject]@{ Minutes = 30;  Label = 'Через 30 минут' },
    [pscustomobject]@{ Minutes = 60;  Label = 'Через 1 час' },
    [pscustomobject]@{ Minutes = 120; Label = 'Через 2 часа' },
    [pscustomobject]@{ Minutes = 180; Label = 'Через 3 часа' },
    [pscustomobject]@{ Minutes = 240; Label = 'Через 4 часа' },
    [pscustomobject]@{ Minutes = 360; Label = 'Через 6 часов' },
    [pscustomobject]@{ Minutes = 480; Label = 'Через 8 часов' },
    [pscustomobject]@{ Minutes = 600; Label = 'Через 10 часов' }
)

foreach ($timerOption in $timerOptions) {
    $timerChoice = [System.Windows.Forms.ToolStripMenuItem]::new(
        $timerOption.Label
    )

    $timerChoice.Tag = $timerOption.Minutes

    $timerChoice.add_Click({
        param($sender, $eventArgs)

        $selectedMinutes = [int] $sender.Tag

        [void] (
            Start-KeepAwakeTimer -Minutes $selectedMinutes
        )
    })

    [void] $script:TimerMenuItem.DropDownItems.Add($timerChoice)
}

[void] $script:TimerMenuItem.DropDownItems.Add(
    [System.Windows.Forms.ToolStripSeparator]::new()
)

$script:TimerCancelItem = [System.Windows.Forms.ToolStripMenuItem]::new(
    'Отменить таймер'
)
$script:TimerCancelItem.Enabled = $false

[void] $script:TimerMenuItem.DropDownItems.Add(
    $script:TimerCancelItem
)

$exitItem = [System.Windows.Forms.ToolStripMenuItem]::new('Выход')

[void] $menu.Items.Add($script:StatusItem)
[void] $menu.Items.Add($script:TimerStatusItem)

[void] $menu.Items.Add(
    [System.Windows.Forms.ToolStripSeparator]::new()
)

[void] $menu.Items.Add($script:EnableItem)
[void] $menu.Items.Add($script:DisableItem)
[void] $menu.Items.Add($script:TimerMenuItem)

[void] $menu.Items.Add(
    [System.Windows.Forms.ToolStripSeparator]::new()
)

[void] $menu.Items.Add($exitItem)

$script:NotifyIcon = [System.Windows.Forms.NotifyIcon]::new()
$script:NotifyIcon.ContextMenuStrip = $menu
$script:NotifyIcon.Icon = $script:IconOff
$script:NotifyIcon.Text = 'KeepAwake — сон разрешён'
$script:NotifyIcon.Visible = $true

$script:CountdownTimer = [System.Windows.Forms.Timer]::new()
$script:CountdownTimer.Interval = 1000

$script:CountdownTimer.add_Tick({
    Update-KeepAwakeTimerDisplay
})

$script:EnableItem.add_Click({
    [void] (Set-KeepAwakeState -Enabled $true)
})

$script:DisableItem.add_Click({
    Stop-KeepAwakeTimer -Silent
    [void] (Set-KeepAwakeState -Enabled $false)
})

$script:TimerCancelItem.add_Click({
    Stop-KeepAwakeTimer
})

# Двойной щелчок по значку переключает режим.
$script:NotifyIcon.add_DoubleClick({
    if ($script:AwakeEnabled) {
        Stop-KeepAwakeTimer -Silent
        [void] (Set-KeepAwakeState -Enabled $false)
    }
    else {
        [void] (Set-KeepAwakeState -Enabled $true)
    }
})

$exitItem.add_Click({
    $appContext.ExitThread()
})

Update-KeepAwakeInterface

# Ручной запуск программы означает временный запрет автоматического сна.
# По умолчанию используем максимальный безопасный интервал — 10 часов.
[void] (
    Start-KeepAwakeTimer `
        -Minutes 600 `
        -SuppressNotification
)

try {
    [System.Windows.Forms.Application]::Run($appContext)
}
catch {
    Write-KeepAwakeError "Критическая ошибка: $($_.Exception)"
    throw
}
finally {
    # Явно снимаем запрет сна при любом завершении программы.
    try {
        [void] [KeepAwakeNative]::SetThreadExecutionState(
            $script:ES_CONTINUOUS
        )
    }
    catch {
        Write-KeepAwakeError "Не удалось снять запрос бодрствования: $($_.Exception.Message)"
    }

    try {
        if ($null -ne $script:CountdownTimer) {
            $script:CountdownTimer.Stop()
            $script:CountdownTimer.Dispose()
        }

        $script:NotifyIcon.Visible = $false
        $script:NotifyIcon.Dispose()
        $script:IconOff.Dispose()
        $script:IconOn.Dispose()
        $menu.Dispose()
        $appContext.Dispose()
    }
    catch {
        Write-KeepAwakeError "Ошибка освобождения интерфейса: $($_.Exception.Message)"
    }

    try {
        $mutex.ReleaseMutex()
    }
    catch {
        # При аварийном завершении владение mutex могло быть уже потеряно.
    }
    finally {
        $mutex.Dispose()
    }
}



