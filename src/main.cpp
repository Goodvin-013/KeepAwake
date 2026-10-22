// KeepAwake — значок в трее, который запрещает автоматический сон Windows
// и, по желанию, выключение экрана.
//
// Всё работает в одном потоке (UI-поток): SetThreadExecutionState привязан
// к вызвавшему потоку, поэтому вызывать его из других потоков нельзя.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shellscalingapi.h>
#include <wchar.h>
#include <strsafe.h>

#include "resource.h"

namespace {

// ---------------------------------------------------------------------------
// Константы
// ---------------------------------------------------------------------------

constexpr wchar_t kWindowClass[] = L"KeepAwake.TrayWindow";
constexpr wchar_t kMutexName[]   = L"Local\\KeepAwake.Tray";
constexpr wchar_t kRegistryKey[] = L"Software\\KeepAwake";
constexpr wchar_t kRegistryLang[] = L"Language";

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kCountdownTimerId = 1;
constexpr UINT kCountdownIntervalMs = 500;

// Пасхалка: щелчок по строке с версией на 3 секунды меняет значок на смайлик.
constexpr UINT_PTR kSmileTimerId = 2;
constexpr UINT kSmileDurationMs = 3000;

// Горячая клавиша Ctrl+Alt+K включает и выключает запрет сна,
// как двойной щелчок по значку.
constexpr int kHotkeyId = 1;
constexpr UINT kHotkeyModifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
constexpr UINT kHotkeyKey = 'K';
constexpr wchar_t kHotkeyText[] = L"Ctrl+Alt+K";

// При ручном запуске запрет сна включается на 10 часов.
constexpr int kStartupMinutes = 600;

// Защита батареи: на батарее с зарядом ниже порога запрет сна не держится.
constexpr BYTE kBatteryLowPercent = 20;

// Уведомления Windows об изменении заряда и источника питания.
// {A7AD8041-B45A-4CAE-87A3-EECBB468A9E1} — GUID_BATTERY_PERCENTAGE_REMAINING
// {5D3E9A59-E9D5-4B00-A6BD-FF34FF516548} — GUID_ACDC_POWER_SOURCE
constexpr GUID kGuidBatteryPercent =
    { 0xA7AD8041, 0xB45A, 0x4CAE, { 0x87, 0xA3, 0xEE, 0xCB, 0xB4, 0x68, 0xA9, 0xE1 } };
constexpr GUID kGuidPowerSource =
    { 0x5D3E9A59, 0xE9D5, 0x4B00, { 0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48 } };

constexpr int kTimerMinutes[] = { 30, 60, 120, 180, 240, 360, 480, 600, 1200 };
constexpr int kTimerCount = sizeof(kTimerMinutes) / sizeof(kTimerMinutes[0]);

enum CommandId : UINT {
    ID_VERSION = 999,
    ID_STATUS = 1000,
    ID_TIMER_STATUS,
    ID_ENABLE,
    ID_DISABLE,
    ID_KEEP_DISPLAY,
    ID_TIMER_CANCEL,
    ID_LANG_RU,
    ID_LANG_EN,
    ID_EXIT,
    ID_TIMER_UNTIL,
    ID_TIMER_EXTEND,
    ID_TIMER_FIRST = 2000, // ID_TIMER_FIRST + индекс в kTimerMinutes
};

// ---------------------------------------------------------------------------
// Строки интерфейса
// ---------------------------------------------------------------------------

enum Lang { LANG_RU_UI = 0, LANG_EN_UI = 1 };

enum StringId {
    S_ALREADY_RUNNING,
    S_STATUS_ON,
    S_STATUS_OFF,
    S_TIMER_NONE,
    S_TIMER_UNLIMITED,
    S_TIMER_FMT,
    S_ENABLE,
    S_DISABLE,
    S_KEEP_DISPLAY,
    S_AUTO_OFF,
    S_AFTER_FMT,
    S_TIMER_CANCEL,
    S_TIMER_UNTIL,
    S_TIMER_EXTEND,
    S_PICKER_UNTIL_LABEL,
    S_PICKER_OK,
    S_PICKER_CANCEL,
    S_LANGUAGE,
    S_EXIT,
    S_TIP_ON,
    S_TIP_OFF,
    S_TIP_COUNTDOWN_FMT,
    S_NOTIFY_TIMER_DONE,
    S_NOTIFY_BATTERY_OFF,
    S_NOTIFY_BATTERY_BLOCKED,
    S_ERROR_REQUEST,
    S_COUNT
};

const wchar_t* const kStrings[2][S_COUNT] = {
    {
        L"KeepAwake уже запущен.",
        L"Статус: сон запрещён",
        L"Статус: сон разрешён",
        L"Таймер: не установлен",
        L"Таймер: без ограничения",
        L"Таймер: %ls",
        L"Включить запрет сна",
        L"Выключить запрет сна",
        L"Не выключать экран",
        L"Автоотключение",
        L"Через %ls",
        L"Отменить таймер",
        L"До времени…",
        L"Продлить на 1 час",
        L"Выключить запрет сна в:",
        L"ОК",
        L"Отмена",
        L"Язык / Language",
        L"Выход",
        L"KeepAwake — сон запрещён",
        L"KeepAwake — сон разрешён",
        L"KeepAwake — %ls",
        L"Таймер завершён. Запрет сна автоматически выключен.",
        L"Заряд батареи ниже 20 %. Запрет сна выключен.",
        L"Заряд батареи ниже 20 %. Запрет сна не включён, подключите зарядку.",
        L"Windows не приняла запрос на изменение режима сна.",
    },
    {
        L"KeepAwake is already running.",
        L"Status: sleep blocked",
        L"Status: sleep allowed",
        L"Timer: not set",
        L"Timer: no limit",
        L"Timer: %ls",
        L"Block sleep",
        L"Allow sleep",
        L"Keep display on",
        L"Turn off automatically",
        L"After %ls",
        L"Cancel timer",
        L"Until a time…",
        L"Extend by 1 hour",
        L"Allow sleep at:",
        L"OK",
        L"Cancel",
        L"Язык / Language",
        L"Exit",
        L"KeepAwake — sleep blocked",
        L"KeepAwake — sleep allowed",
        L"KeepAwake — %ls",
        L"Timer finished. Sleep is allowed again.",
        L"Battery below 20%. Sleep is allowed again.",
        L"Battery below 20%. Sleep was not blocked. Connect the charger.",
        L"Windows rejected the sleep mode request.",
    },
};

// ---------------------------------------------------------------------------
// Состояние программы
// ---------------------------------------------------------------------------

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
UINT g_taskbarCreatedMsg = 0;

HICON g_iconOn = nullptr;
HICON g_iconOff = nullptr;
HICON g_iconOnDisplay = nullptr;  // запрет сна + «Не выключать экран»
HICON g_iconSmile = nullptr;      // пасхалка
bool g_showSmile = false;         // сейчас в трее смайлик

Lang g_lang = LANG_EN_UI;

bool g_awake = false;        // запрет сна включён
bool g_hotkey = false;       // Ctrl+Alt+K зарегистрирована (не занята другой программой)
bool g_keepDisplay = false;  // галочка «Не выключать экран» (не сохраняется)
bool g_timerRunning = false;
ULONGLONG g_deadlineMs = 0;  // по GetTickCount64

HMENU g_openMenu = nullptr;  // меню, открытое в данный момент
HBITMAP g_menuIconBitmap = nullptr;  // значок программы у строки с версией
HBITMAP g_menuDotBitmap = nullptr;   // цветная точка у строки статуса

const wchar_t* Str(StringId id) {
    return kStrings[g_lang][id];
}

// ---------------------------------------------------------------------------
// Язык
// ---------------------------------------------------------------------------

Lang DetectSystemLanguage() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN
        ? LANG_RU_UI
        : LANG_EN_UI;
}

Lang LoadLanguage() {
    wchar_t value[8] = {};
    DWORD size = sizeof(value);

    if (RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, kRegistryLang,
                     RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) {
        if (wcscmp(value, L"ru") == 0) return LANG_RU_UI;
        if (wcscmp(value, L"en") == 0) return LANG_EN_UI;
    }

    return DetectSystemLanguage();
}

void SaveLanguage(Lang lang) {
    HKEY key = nullptr;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    const wchar_t* value = (lang == LANG_RU_UI) ? L"ru" : L"en";
    RegSetValueExW(key, kRegistryLang, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value),
                   static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// Форматирование
// ---------------------------------------------------------------------------

// Русское слово для числа: 1 минута, 2 минуты, 5 минут, 21 минута.
const wchar_t* RuPlural(int n, const wchar_t* one, const wchar_t* few, const wchar_t* many) {
    const int mod100 = n % 100;
    const int mod10 = n % 10;
    if (mod100 >= 11 && mod100 <= 14) return many;
    if (mod10 == 1) return one;
    if (mod10 >= 2 && mod10 <= 4) return few;
    return many;
}

// Длительность словами: «30 минут», «2 часа», «2 ч 15 мин».
void FormatDuration(int minutes, wchar_t* out, size_t outCount) {
    const int hours = minutes / 60;
    const int rest = minutes % 60;

    if (g_lang == LANG_EN_UI) {
        if (hours == 0) {
            StringCchPrintfW(out, outCount, minutes == 1 ? L"%d minute" : L"%d minutes", minutes);
        } else if (rest == 0) {
            StringCchPrintfW(out, outCount, hours == 1 ? L"%d hour" : L"%d hours", hours);
        } else {
            StringCchPrintfW(out, outCount, L"%d h %d min", hours, rest);
        }
        return;
    }

    if (hours == 0) {
        StringCchPrintfW(out, outCount, L"%d %ls", minutes,
                         RuPlural(minutes, L"минуту", L"минуты", L"минут"));
    } else if (rest == 0) {
        StringCchPrintfW(out, outCount, L"%d %ls", hours,
                         RuPlural(hours, L"час", L"часа", L"часов"));
    } else {
        StringCchPrintfW(out, outCount, L"%d ч %d мин", hours, rest);
    }
}

// Оставшееся время в формате часы:минуты:секунды.
void FormatRemaining(ULONGLONG remainingMs, wchar_t* out, size_t outCount) {
    const ULONGLONG totalSeconds = (remainingMs + 999) / 1000;
    const unsigned hours = static_cast<unsigned>(totalSeconds / 3600);
    const unsigned minutes = static_cast<unsigned>((totalSeconds % 3600) / 60);
    const unsigned seconds = static_cast<unsigned>(totalSeconds % 60);

    StringCchPrintfW(out, outCount, L"%02u:%02u:%02u", hours, minutes, seconds);
}

ULONGLONG RemainingMs() {
    const ULONGLONG now = GetTickCount64();
    return (g_deadlineMs > now) ? (g_deadlineMs - now) : 0;
}

void TimerStatusText(wchar_t* out, size_t outCount) {
    if (g_timerRunning) {
        wchar_t countdown[32];
        FormatRemaining(RemainingMs(), countdown, 32);
        StringCchPrintfW(out, outCount, Str(S_TIMER_FMT), countdown);
    } else {
        StringCchCopyW(out, outCount, Str(g_awake ? S_TIMER_UNLIMITED : S_TIMER_NONE));
    }
}

// ---------------------------------------------------------------------------
// Значок в трее
// ---------------------------------------------------------------------------

void FreeIcons() {
    if (g_iconOn) DestroyIcon(g_iconOn);
    if (g_iconOff) DestroyIcon(g_iconOff);
    if (g_iconOnDisplay) DestroyIcon(g_iconOnDisplay);
    if (g_iconSmile) DestroyIcon(g_iconSmile);
    g_iconOn = nullptr;
    g_iconOff = nullptr;
    g_iconOnDisplay = nullptr;
    g_iconSmile = nullptr;
}

// LoadIconMetric берёт из .ico кадр нужного размера под текущий масштаб
// экрана (16, 20, 24, 32, 40 px), а не растягивает один маленький кадр.
void LoadIcons() {
    FreeIcons();
    LoadIconMetric(g_instance, MAKEINTRESOURCEW(IDI_KEEPAWAKE_ON), LIM_SMALL, &g_iconOn);
    LoadIconMetric(g_instance, MAKEINTRESOURCEW(IDI_KEEPAWAKE_OFF), LIM_SMALL, &g_iconOff);
    LoadIconMetric(g_instance, MAKEINTRESOURCEW(IDI_KEEPAWAKE_ON_DISPLAY), LIM_SMALL,
                   &g_iconOnDisplay);
    LoadIconMetric(g_instance, MAKEINTRESOURCEW(IDI_KEEPAWAKE_SMILE), LIM_SMALL, &g_iconSmile);
}

// Серый глаз — сон разрешён, зелёный — запрещён,
// синий — запрещены и сон, и выключение экрана.
HICON CurrentIcon() {
    if (g_showSmile && g_iconSmile) return g_iconSmile;
    if (!g_awake) return g_iconOff;
    return g_keepDisplay ? g_iconOnDisplay : g_iconOn;
}

NOTIFYICONDATAW MakeNotifyData(UINT flags) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_window;
    nid.uID = kTrayIconId;
    nid.uFlags = flags;
    return nid;
}

void FillTip(wchar_t* out, size_t outCount) {
    if (g_awake && g_timerRunning) {
        wchar_t countdown[32];
        FormatRemaining(RemainingMs(), countdown, 32);
        StringCchPrintfW(out, outCount, Str(S_TIP_COUNTDOWN_FMT), countdown);
    } else {
        StringCchCopyW(out, outCount, Str(g_awake ? S_TIP_ON : S_TIP_OFF));
    }
}

void AddTrayIcon() {
    NOTIFYICONDATAW nid = MakeNotifyData(NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = CurrentIcon();
    FillTip(nid.szTip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid = MakeNotifyData(0);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void UpdateTrayIcon() {
    NOTIFYICONDATAW nid = MakeNotifyData(NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    nid.hIcon = CurrentIcon();
    FillTip(nid.szTip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Пасхалка: смайлик в трее на 3 секунды. Повторный щелчок продлевает показ.
void ShowSmile() {
    g_showSmile = true;
    UpdateTrayIcon();
    SetTimer(g_window, kSmileTimerId, kSmileDurationMs, nullptr);
}

void HideSmile() {
    KillTimer(g_window, kSmileTimerId);
    g_showSmile = false;
    UpdateTrayIcon();
}

void ShowNotification(const wchar_t* text) {
    NOTIFYICONDATAW nid = MakeNotifyData(NIF_INFO);
    StringCchCopyW(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), L"KeepAwake");
    StringCchCopyW(nid.szInfo, ARRAYSIZE(nid.szInfo), text);
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------------------
// Открытое меню: живой обратный отсчёт
// ---------------------------------------------------------------------------

BOOL CALLBACK FindMenuWindowProc(HWND hwnd, LPARAM lParam) {
    wchar_t className[16] = {};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));

    if (wcscmp(className, L"#32768") == 0 && IsWindowVisible(hwnd)) {
        InvalidateRect(hwnd, nullptr, TRUE);
        *reinterpret_cast<bool*>(lParam) = true;
    }

    return TRUE;
}

void RefreshOpenMenu() {
    if (!g_openMenu) return;

    wchar_t text[64];
    TimerStatusText(text, ARRAYSIZE(text));

    MENUITEMINFOW mii = {};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = text;
    SetMenuItemInfoW(g_openMenu, ID_TIMER_STATUS, FALSE, &mii);

    bool found = false;
    EnumThreadWindows(GetCurrentThreadId(), FindMenuWindowProc,
                      reinterpret_cast<LPARAM>(&found));
}

// ---------------------------------------------------------------------------
// Режим сна
// ---------------------------------------------------------------------------

void ShowError(const wchar_t* text) {
    MessageBoxW(nullptr, text, L"KeepAwake", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

// Передаёт Windows текущий запрос. ES_SYSTEM_REQUIRED всегда остаётся
// вместе с ES_DISPLAY_REQUIRED: одного запроса на экран мало.
bool ApplyAwakeState(bool enable) {
    EXECUTION_STATE flags = ES_CONTINUOUS;

    if (enable) {
        flags |= ES_SYSTEM_REQUIRED;
        if (g_keepDisplay) flags |= ES_DISPLAY_REQUIRED;
    }

    if (SetThreadExecutionState(flags) == 0) {
        ShowError(Str(S_ERROR_REQUEST));
        return false;
    }

    g_awake = enable;
    // Галочка «Не выключать экран» действует только во время запрета сна.
    if (!enable) g_keepDisplay = false;
    UpdateTrayIcon();
    return true;
}

// true, если ноутбук работает от батареи и заряд ниже порога.
// Если источник питания или заряд неизвестны, защита не срабатывает.
bool BatteryLow() {
    SYSTEM_POWER_STATUS ps = {};
    if (!GetSystemPowerStatus(&ps)) return false;
    if (ps.ACLineStatus != 0) return false;          // 1 — от сети, 255 — неизвестно
    if (ps.BatteryFlag & 128) return false;          // батареи нет
    if (ps.BatteryLifePercent == 255) return false;  // заряд неизвестен
    return ps.BatteryLifePercent < kBatteryLowPercent;
}

void CheckBattery();

// Уведомления показываем только о том, что произошло без участия
// пользователя: таймер закончился, села батарея, батарея не дала включить
// запрет. О действиях, выбранных самим пользователем, не сообщаем.
bool SetAwake(bool enable) {
    // На батарее ниже порога запрет сна не включается (в том числе при запуске
    // программы и по таймеру), об этом всегда сообщает уведомление.
    if (enable && BatteryLow()) {
        if (g_awake) {
            CheckBattery();
        } else {
            ShowNotification(Str(S_NOTIFY_BATTERY_BLOCKED));
        }
        return false;
    }

    return ApplyAwakeState(enable);
}

enum class TimerStop { Expired, Silent };

void StopTimer(TimerStop how) {
    KillTimer(g_window, kCountdownTimerId);
    g_timerRunning = false;
    g_deadlineMs = 0;

    if (how == TimerStop::Expired) {
        // Открытое меню показывает устаревшее состояние, закрываем его.
        if (g_openMenu) EndMenu();

        if (SetAwake(false)) {
            ShowNotification(Str(S_NOTIFY_TIMER_DONE));
        }
        return;
    }

    UpdateTrayIcon();
}

// Защита батареи: снимает запрет сна, если ноутбук перешёл на батарею
// или заряд упал ниже порога. Обратно запрет сам не включается.
void CheckBattery() {
    if (!g_awake || !BatteryLow()) return;

    // Открытое меню показывает устаревшее состояние, закрываем его.
    if (g_openMenu) EndMenu();

    StopTimer(TimerStop::Silent);
    if (ApplyAwakeState(false)) {
        ShowNotification(Str(S_NOTIFY_BATTERY_OFF));
    }
}

void StartTimerMs(ULONGLONG durationMs) {
    if (!SetAwake(true)) return;

    g_deadlineMs = GetTickCount64() + durationMs;
    g_timerRunning = true;
    SetTimer(g_window, kCountdownTimerId, kCountdownIntervalMs, nullptr);
    UpdateTrayIcon();
}

void StartTimer(int minutes) {
    StartTimerMs(static_cast<ULONGLONG>(minutes) * 60000ULL);
}

// «Продлить на 1 час»: работает только при идущем таймере.
void ExtendTimer() {
    if (!g_timerRunning) return;

    constexpr ULONGLONG kExtendMs = 60ULL * 60000ULL;
    g_deadlineMs += kExtendMs;
    UpdateTrayIcon();
}

void OnCountdownTick() {
    if (!g_timerRunning) return;

    if (RemainingMs() == 0) {
        StopTimer(TimerStop::Expired);
        return;
    }

    UpdateTrayIcon();
    RefreshOpenMenu();
}

void ToggleKeepDisplay() {
    // Пункт доступен только во время запрета сна.
    if (!g_awake) return;

    g_keepDisplay = !g_keepDisplay;

    // Сразу передаём Windows новые флаги.
    if (!ApplyAwakeState(true)) {
        g_keepDisplay = !g_keepDisplay;
    }
}

void ToggleAwake() {
    if (g_awake) {
        StopTimer(TimerStop::Silent);
        SetAwake(false);
    } else {
        SetAwake(true);
    }
}

// ---------------------------------------------------------------------------
// Картинки в меню: значок у версии и цветная точка у статуса.
// Это обычные картинки пунктов (32 бита с прозрачностью), поэтому Windows
// рисует меню в родном стиле. Рисование пунктов самой программой
// переключило бы всё меню в классический стиль.
// ---------------------------------------------------------------------------

constexpr COLORREF kDotOnColor  = RGB(0x0E, 0x96, 0x68);  // сон запрещён
constexpr COLORREF kDotOffColor = RGB(0x9A, 0x9A, 0x9A);  // сон разрешён
constexpr int kDotDiameter = 8;  // при масштабе 100 %

// Пустая картинка 32 бита сверху вниз; bits указывает на её пиксели.
HBITMAP CreateMenuBitmap(int size, UINT32** bits) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* data = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &data, nullptr, 0);
    if (!bitmap) return nullptr;
    ZeroMemory(data, static_cast<size_t>(size) * size * 4);
    *bits = static_cast<UINT32*>(data);
    return bitmap;
}

// Значок «открытый глаз» нужного размера в виде картинки для меню.
HBITMAP CreateIconMenuBitmap(UINT dpi) {
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    HICON icon = nullptr;
    if (FAILED(LoadIconWithScaleDown(g_instance, MAKEINTRESOURCEW(IDI_KEEPAWAKE_ON),
                                     size, size, &icon))) {
        return nullptr;
    }

    ICONINFO info = {};
    if (!GetIconInfo(icon, &info)) {
        DestroyIcon(icon);
        return nullptr;
    }

    UINT32* bits = nullptr;
    HBITMAP bitmap = CreateMenuBitmap(size, &bits);
    bool ok = false;
    if (bitmap && info.hbmColor) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = size;
        bmi.bmiHeader.biHeight = -size;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, info.hbmColor, 0, size, bits, &bmi, DIB_RGB_COLORS) == size;

        const int count = size * size;
        bool hasAlpha = false;
        for (int i = 0; ok && i < count && !hasAlpha; ++i) {
            hasAlpha = (bits[i] >> 24) != 0;
        }

        if (ok && !hasAlpha && info.hbmMask) {
            // Значок без прозрачности: берём её из маски.
            UINT32* mask = static_cast<UINT32*>(
                HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(count) * 4));
            if (mask && GetDIBits(dc, info.hbmMask, 0, size, mask, &bmi, DIB_RGB_COLORS) == size) {
                for (int i = 0; i < count; ++i) {
                    const UINT32 alpha = (mask[i] & 0x00FFFFFF) ? 0 : 0xFF;
                    bits[i] = (bits[i] & 0x00FFFFFF) | (alpha << 24);
                }
            }
            if (mask) HeapFree(GetProcessHeap(), 0, mask);
        }
        ReleaseDC(nullptr, dc);

        // Меню ждёт цвета, уже умноженные на прозрачность.
        for (int i = 0; ok && i < count; ++i) {
            const UINT32 a = bits[i] >> 24;
            const UINT32 r = ((bits[i] >> 16) & 0xFF) * a / 255;
            const UINT32 g = ((bits[i] >> 8) & 0xFF) * a / 255;
            const UINT32 b = (bits[i] & 0xFF) * a / 255;
            bits[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    DestroyIcon(icon);

    if (!ok && bitmap) {
        DeleteObject(bitmap);
        bitmap = nullptr;
    }
    return bitmap;
}

// Круглая точка по центру картинки размером со значок, со сглаженным краем.
HBITMAP CreateDotMenuBitmap(UINT dpi, COLORREF color) {
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    UINT32* bits = nullptr;
    HBITMAP bitmap = CreateMenuBitmap(size, &bits);
    if (!bitmap) return nullptr;

    const double diameter = static_cast<double>(MulDiv(kDotDiameter, dpi, USER_DEFAULT_SCREEN_DPI));
    const double radius = diameter / 2.0;
    const double center = size / 2.0;
    constexpr int kSamples = 4;  // 4×4 точки на пиксель для гладкого края

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int inside = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const double px = x + (sx + 0.5) / kSamples - center;
                    const double py = y + (sy + 0.5) / kSamples - center;
                    if (px * px + py * py <= radius * radius) ++inside;
                }
            }
            const UINT32 a = static_cast<UINT32>(inside * 255 / (kSamples * kSamples));
            const UINT32 r = GetRValue(color) * a / 255;
            const UINT32 g = GetGValue(color) * a / 255;
            const UINT32 b = GetBValue(color) * a / 255;
            bits[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return bitmap;
}

void SetMenuItemBitmap(HMENU menu, UINT id, HBITMAP bitmap) {
    if (!bitmap) return;
    MENUITEMINFOW mii = {};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = bitmap;
    SetMenuItemInfoW(menu, id, FALSE, &mii);
}

void FreeMenuBitmaps() {
    if (g_menuIconBitmap) {
        DeleteObject(g_menuIconBitmap);
        g_menuIconBitmap = nullptr;
    }
    if (g_menuDotBitmap) {
        DeleteObject(g_menuDotBitmap);
        g_menuDotBitmap = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Контекстное меню
// ---------------------------------------------------------------------------

HMENU BuildMenu() {
    HMENU menu = CreatePopupMenu();
    wchar_t text[128];

    // Название и версия программы, чтобы было видно, какая версия запущена.
    // Информационные строки сделаны обычными пунктами без действия: любые
    // неактивные пункты Windows 11 выцветает вместе с картинкой, а так текст
    // остаётся тёмным, а значок и точка цветными. Щелчок по ним только
    // закрывает меню (HandleCommand их пропускает; щелчок по версии
    // показывает пасхалку).
    AppendMenuW(menu, MF_STRING, ID_VERSION, L"KeepAwake " KA_VERSION_STRING);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, ID_STATUS,
                Str(g_awake ? S_STATUS_ON : S_STATUS_OFF));

    TimerStatusText(text, ARRAYSIZE(text));
    AppendMenuW(menu, MF_STRING, ID_TIMER_STATUS, text);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Справа от пунктов показываем горячую клавишу, если она работает.
    wchar_t enableText[96];
    wchar_t disableText[96];
    if (g_hotkey) {
        StringCchPrintfW(enableText, ARRAYSIZE(enableText), L"%ls\t%ls", Str(S_ENABLE), kHotkeyText);
        StringCchPrintfW(disableText, ARRAYSIZE(disableText), L"%ls\t%ls", Str(S_DISABLE), kHotkeyText);
    } else {
        StringCchCopyW(enableText, ARRAYSIZE(enableText), Str(S_ENABLE));
        StringCchCopyW(disableText, ARRAYSIZE(disableText), Str(S_DISABLE));
    }
    AppendMenuW(menu, MF_STRING | (g_awake ? MF_GRAYED : 0), ID_ENABLE, enableText);
    AppendMenuW(menu, MF_STRING | (g_awake ? 0 : MF_GRAYED), ID_DISABLE, disableText);
    AppendMenuW(menu, MF_STRING | (g_awake ? 0 : MF_GRAYED) | (g_keepDisplay ? MF_CHECKED : 0),
                ID_KEEP_DISPLAY,
                Str(S_KEEP_DISPLAY));

    HMENU timerMenu = CreatePopupMenu();
    for (int i = 0; i < kTimerCount; ++i) {
        wchar_t duration[32];
        FormatDuration(kTimerMinutes[i], duration, ARRAYSIZE(duration));
        StringCchPrintfW(text, ARRAYSIZE(text), Str(S_AFTER_FMT), duration);
        AppendMenuW(timerMenu, MF_STRING, ID_TIMER_FIRST + i, text);
    }
    AppendMenuW(timerMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(timerMenu, MF_STRING, ID_TIMER_UNTIL, Str(S_TIMER_UNTIL));
    AppendMenuW(timerMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(timerMenu, MF_STRING | (g_timerRunning ? 0 : MF_GRAYED), ID_TIMER_EXTEND,
                Str(S_TIMER_EXTEND));
    AppendMenuW(timerMenu, MF_STRING | (g_timerRunning ? 0 : MF_GRAYED), ID_TIMER_CANCEL,
                Str(S_TIMER_CANCEL));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(timerMenu), Str(S_AUTO_OFF));

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU langMenu = CreatePopupMenu();
    AppendMenuW(langMenu, MF_STRING, ID_LANG_RU, L"Русский");
    AppendMenuW(langMenu, MF_STRING, ID_LANG_EN, L"English");
    CheckMenuRadioItem(langMenu, ID_LANG_RU, ID_LANG_EN,
                       g_lang == LANG_RU_UI ? ID_LANG_RU : ID_LANG_EN, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(langMenu), Str(S_LANGUAGE));

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, Str(S_EXIT));

    // Значок у версии и точка у статуса. Галочка и картинка делят одно
    // место слева, чтобы меню не становилось шире.
    SetMenuItemBitmap(menu, ID_VERSION, g_menuIconBitmap);
    SetMenuItemBitmap(menu, ID_STATUS, g_menuDotBitmap);

    MENUINFO info = {};
    info.cbSize = sizeof(info);
    info.fMask = MIM_STYLE;
    info.dwStyle = MNS_CHECKORBMP;
    SetMenuInfo(menu, &info);

    return menu;
}

// ---------------------------------------------------------------------------
// Окно «До времени…»: время меняется шагом 15 минут колёсиком мыши,
// стрелками ↑↓ на клавиатуре или кнопками справа от поля.
// ---------------------------------------------------------------------------

constexpr int kPickerStepMinutes = 15;
constexpr int kMinutesPerDay = 24 * 60;

struct TimePicker {
    int value = 0;       // минуты от начала суток, кратно 15
    int wheelDelta = 0;  // накопленный поворот колёсика (для тачпадов)
};

HWND g_pickerWindow = nullptr;  // открытое окно выбора времени

void PickerShowValue(HWND dlg, const TimePicker* picker) {
    wchar_t text[16];
    StringCchPrintfW(text, ARRAYSIZE(text), L"%02d:%02d", picker->value / 60, picker->value % 60);
    SetDlgItemTextW(dlg, IDC_TIME_VALUE, text);
}

void PickerStep(HWND dlg, TimePicker* picker, int steps) {
    const int value = picker->value + steps * kPickerStepMinutes;
    picker->value = ((value % kMinutesPerDay) + kMinutesPerDay) % kMinutesPerDay;
    PickerShowValue(dlg, picker);
}

INT_PTR CALLBACK TimePickerProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* picker = reinterpret_cast<TimePicker*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG:
        picker = reinterpret_cast<TimePicker*>(lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, lParam);
        g_pickerWindow = dlg;

        SetDlgItemTextW(dlg, IDC_TIME_LABEL, Str(S_PICKER_UNTIL_LABEL));
        SetDlgItemTextW(dlg, IDOK, Str(S_PICKER_OK));
        SetDlgItemTextW(dlg, IDCANCEL, Str(S_PICKER_CANCEL));

        // Позиция счётчика не используется: шаг считаем сами.
        SendDlgItemMessageW(dlg, IDC_TIME_SPIN, UDM_SETRANGE32, 0, 1000);
        SendDlgItemMessageW(dlg, IDC_TIME_SPIN, UDM_SETPOS32, 0, 500);
        PickerShowValue(dlg, picker);

        SetForegroundWindow(dlg);
        SetFocus(GetDlgItem(dlg, IDC_TIME_VALUE));
        SendDlgItemMessageW(dlg, IDC_TIME_VALUE, EM_SETSEL, static_cast<WPARAM>(-1), 0);
        return FALSE;  // фокус уже установлен

    case WM_MOUSEWHEEL:
        picker->wheelDelta += GET_WHEEL_DELTA_WPARAM(wParam);
        while (picker->wheelDelta >= WHEEL_DELTA) {
            PickerStep(dlg, picker, 1);
            picker->wheelDelta -= WHEEL_DELTA;
        }
        while (picker->wheelDelta <= -WHEEL_DELTA) {
            PickerStep(dlg, picker, -1);
            picker->wheelDelta += WHEEL_DELTA;
        }
        return TRUE;

    case WM_NOTIFY: {
        const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
        if (hdr->idFrom == IDC_TIME_SPIN && hdr->code == UDN_DELTAPOS) {
            const auto* ud = reinterpret_cast<const NMUPDOWN*>(lParam);
            PickerStep(dlg, picker, ud->iDelta > 0 ? 1 : -1);
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dlg, LOWORD(wParam));
            return TRUE;
        }
        break;

    case WM_DESTROY:
        g_pickerWindow = nullptr;
        break;

    default:
        break;
    }
    return FALSE;
}

// Запрет сна до выбранного времени. Если это время сегодня уже прошло,
// запрет действует до этого времени завтра.
void ShowUntilPicker() {
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    // По умолчанию: ближайшая четверть часа плюс один час.
    const int nowMinutes = now.wHour * 60 + now.wMinute;
    TimePicker picker;
    picker.value = ((nowMinutes / kPickerStepMinutes + 1) * kPickerStepMinutes + 60) % kMinutesPerDay;

    const INT_PTR result = DialogBoxParamW(g_instance, MAKEINTRESOURCEW(IDD_TIME_PICKER), g_window,
                                           TimePickerProc, reinterpret_cast<LPARAM>(&picker));
    if (result != IDOK) return;

    GetLocalTime(&now);
    const LONGLONG nowMs =
        ((now.wHour * 60LL + now.wMinute) * 60LL + now.wSecond) * 1000LL + now.wMilliseconds;
    LONGLONG durationMs = picker.value * 60000LL - nowMs;
    if (durationMs <= 0) durationMs += kMinutesPerDay * 60000LL;

    StartTimerMs(static_cast<ULONGLONG>(durationMs));
}

void SetLanguage(Lang lang) {
    g_lang = lang;
    SaveLanguage(lang);
    UpdateTrayIcon();
}

void HandleCommand(UINT id) {
    if (id >= ID_TIMER_FIRST && id < ID_TIMER_FIRST + kTimerCount) {
        StartTimer(kTimerMinutes[id - ID_TIMER_FIRST]);
        return;
    }

    switch (id) {
    case ID_ENABLE:
        SetAwake(true);
        break;
    case ID_DISABLE:
        StopTimer(TimerStop::Silent);
        SetAwake(false);
        break;
    case ID_KEEP_DISPLAY:
        ToggleKeepDisplay();
        break;
    case ID_TIMER_CANCEL:
        StopTimer(TimerStop::Silent);
        break;
    case ID_TIMER_UNTIL:
        ShowUntilPicker();
        break;
    case ID_TIMER_EXTEND:
        ExtendTimer();
        break;
    case ID_LANG_RU:
        SetLanguage(LANG_RU_UI);
        break;
    case ID_LANG_EN:
        SetLanguage(LANG_EN_UI);
        break;
    case ID_VERSION:
        ShowSmile();
        break;
    case ID_EXIT:
        DestroyWindow(g_window);
        break;
    default:
        break;
    }
}

void ShowTrayMenu(int x, int y) {
    // Картинки рисуем под масштаб монитора, на котором откроется меню.
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    const POINT pt = { x, y };
    if (FAILED(GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST),
                                MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpiY = USER_DEFAULT_SCREEN_DPI;
    }
    FreeMenuBitmaps();
    g_menuIconBitmap = CreateIconMenuBitmap(dpiY);
    g_menuDotBitmap = CreateDotMenuBitmap(dpiY, g_awake ? kDotOnColor : kDotOffColor);

    HMENU menu = BuildMenu();

    // Без этого меню не закрывается при щелчке мимо него.
    SetForegroundWindow(g_window);

    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;

    g_openMenu = menu;
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenuEx(menu, flags, x, y, g_window, nullptr));
    g_openMenu = nullptr;

    DestroyMenu(menu);
    FreeMenuBitmaps();
    PostMessageW(g_window, WM_NULL, 0, 0);

    if (cmd != 0) HandleCommand(cmd);
}

// ---------------------------------------------------------------------------
// Окно и цикл сообщений
// ---------------------------------------------------------------------------

void ReleaseAwake() {
    KillTimer(g_window, kCountdownTimerId);
    SetThreadExecutionState(ES_CONTINUOUS);
    g_awake = false;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg != 0) {
        // Проводник перезапущен или сменился масштаб: возвращаем значок.
        LoadIcons();
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_APP_TRAY:
        // Пока открыто окно выбора времени, щелчок по значку показывает его.
        if (g_pickerWindow) {
            if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                SetForegroundWindow(g_pickerWindow);
            }
            return 0;
        }
        switch (LOWORD(lParam)) {
        case WM_CONTEXTMENU:
            ShowTrayMenu(static_cast<short>(LOWORD(wParam)),
                         static_cast<short>(HIWORD(wParam)));
            break;
        case WM_LBUTTONDBLCLK:
            ToggleAwake();
            break;
        default:
            break;
        }
        return 0;

    case WM_HOTKEY:
        if (wParam == kHotkeyId) {
            // Открытое меню показывало бы устаревшее состояние.
            if (g_openMenu) EndMenu();
            ToggleAwake();
        }
        return 0;

    case WM_TIMER:
        if (wParam == kCountdownTimerId) OnCountdownTick();
        if (wParam == kSmileTimerId) HideSmile();
        return 0;

    case WM_POWERBROADCAST:
        // Сменился источник питания или заряд батареи.
        CheckBattery();
        return TRUE;

    case WM_ENDSESSION:
        // Выход из Windows или установщик просит закрыть программу.
        if (wParam) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        ReleaseAwake();
        if (g_hotkey) UnregisterHotKey(hwnd, kHotkeyId);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    g_lang = LoadLanguage();

    // Запрещаем запуск нескольких копий программы.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, Str(S_ALREADY_RUNNING), L"KeepAwake",
                    MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // Обычное скрытое окно, а не message-only: только такое окно получает
    // сообщение TaskbarCreated после перезапуска Проводника.
    g_window = CreateWindowExW(0, kWindowClass, L"KeepAwake", WS_OVERLAPPED,
                               0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        CloseHandle(mutex);
        return 1;
    }

    LoadIcons();
    AddTrayIcon();

    // Если сочетание уже занято другой программой, работаем без него.
    g_hotkey = RegisterHotKey(g_window, kHotkeyId, kHotkeyModifiers, kHotkeyKey) != FALSE;

    // Подписываемся на изменения заряда и источника питания для защиты батареи.
    HPOWERNOTIFY batteryNotify =
        RegisterPowerSettingNotification(g_window, &kGuidBatteryPercent, DEVICE_NOTIFY_WINDOW_HANDLE);
    HPOWERNOTIFY powerSourceNotify =
        RegisterPowerSettingNotification(g_window, &kGuidPowerSource, DEVICE_NOTIFY_WINDOW_HANDLE);

    // Ручной запуск включает запрет сна на 10 часов, без уведомления.
    StartTimer(kStartupMinutes);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (batteryNotify) UnregisterPowerSettingNotification(batteryNotify);
    if (powerSourceNotify) UnregisterPowerSettingNotification(powerSourceNotify);
    FreeIcons();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
