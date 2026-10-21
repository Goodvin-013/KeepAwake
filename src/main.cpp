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
#include <wchar.h>
#include <strsafe.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <vssym32.h>

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
    S_NOTIFY_ON,
    S_NOTIFY_ON_DISPLAY,
    S_NOTIFY_OFF,
    S_NOTIFY_TIMER_START_FMT,
    S_NOTIFY_TIMER_DONE,
    S_NOTIFY_TIMER_CANCEL,
    S_NOTIFY_TIMER_EXTEND_FMT,
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
        L"Запрет сна включён. Экран может отключаться.",
        L"Запрет сна включён. Экран не будет отключаться.",
        L"Запрет сна выключен. Действуют настройки Windows.",
        L"Запрет сна включён на %ls. Автоотключение в %ls.",
        L"Таймер завершён. Запрет сна автоматически выключен.",
        L"Таймер отменён. Запрет сна остаётся включённым.",
        L"Таймер продлён на 1 час. Автоотключение в %ls.",
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
        L"Sleep blocked. The display can still turn off.",
        L"Sleep blocked. The display will stay on.",
        L"Sleep allowed. Windows power settings apply.",
        L"Sleep blocked for %ls. Turns off at %ls.",
        L"Timer finished. Sleep is allowed again.",
        L"Timer cancelled. Sleep stays blocked.",
        L"Timer extended by 1 hour. Turns off at %ls.",
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

Lang g_lang = LANG_EN_UI;

bool g_awake = false;        // запрет сна включён
bool g_hotkey = false;       // Ctrl+Alt+K зарегистрирована (не занята другой программой)
bool g_keepDisplay = false;  // галочка «Не выключать экран» (не сохраняется)
bool g_timerRunning = false;
ULONGLONG g_deadlineMs = 0;  // по GetTickCount64

HMENU g_openMenu = nullptr;  // меню, открытое в данный момент

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

// Время окончания таймера по часам Windows, например 23:15:00.
void FormatEndTime(ULONGLONG durationMs, wchar_t* out, int outCount) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    FILETIME ft = {};
    SystemTimeToFileTime(&now, &ft);

    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    t.QuadPart += durationMs * 10000ULL;  // FILETIME считает в единицах по 100 нс
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;

    SYSTEMTIME end = {};
    FileTimeToSystemTime(&ft, &end);

    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &end, L"HH':'mm':'ss",
                        out, outCount) == 0) {
        StringCchPrintfW(out, static_cast<size_t>(outCount), L"%02u:%02u:%02u",
                 end.wHour, end.wMinute, end.wSecond);
    }
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
    g_iconOn = nullptr;
    g_iconOff = nullptr;
    g_iconOnDisplay = nullptr;
}

// LoadIconMetric берёт из .ico кадр нужного размера под текущий масштаб
// экрана (16, 20, 24, 32, 40 px), а не растягивает один маленький кадр.
void LoadIcons() {
    FreeIcons();
    LoadIconMetric(g_instance, MAKEINTRESOURCEW(IDI_KEEPAWAKE_ON), LIM_SMALL, &g_iconOn);
    LoadIconMetric(g_instance, MAKEINTRESOURCEW(IDI_KEEPAWAKE_OFF), LIM_SMALL, &g_iconOff);
    LoadIconMetric(g_instance, MAKEINTRESOURCEW(IDI_KEEPAWAKE_ON_DISPLAY), LIM_SMALL,
                   &g_iconOnDisplay);
}

// Серый глаз — сон разрешён, зелёный — запрещён,
// синий — запрещены и сон, и выключение экрана.
HICON CurrentIcon() {
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

    // Строку таймера рисуем сами: достаточно перерисовать окно меню.
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

bool SetAwake(bool enable, bool notify) {
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

    if (!ApplyAwakeState(enable)) return false;

    if (notify) {
        if (enable) {
            ShowNotification(Str(g_keepDisplay ? S_NOTIFY_ON_DISPLAY : S_NOTIFY_ON));
        } else {
            ShowNotification(Str(S_NOTIFY_OFF));
        }
    }

    return true;
}

enum class TimerStop { Expired, Cancelled, Silent };

void StopTimer(TimerStop how) {
    const bool wasRunning = g_timerRunning;

    KillTimer(g_window, kCountdownTimerId);
    g_timerRunning = false;
    g_deadlineMs = 0;

    if (how == TimerStop::Expired) {
        // Открытое меню показывает устаревшее состояние, закрываем его.
        if (g_openMenu) EndMenu();

        if (SetAwake(false, false)) {
            ShowNotification(Str(S_NOTIFY_TIMER_DONE));
        }
        return;
    }

    UpdateTrayIcon();

    if (wasRunning && how == TimerStop::Cancelled) {
        ShowNotification(Str(S_NOTIFY_TIMER_CANCEL));
    }
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

void StartTimerMs(ULONGLONG durationMs, bool notify) {
    if (!SetAwake(true, false)) return;

    g_deadlineMs = GetTickCount64() + durationMs;
    g_timerRunning = true;
    SetTimer(g_window, kCountdownTimerId, kCountdownIntervalMs, nullptr);
    UpdateTrayIcon();

    if (notify) {
        wchar_t duration[32];
        wchar_t endTime[32];
        wchar_t text[256];

        // Длительность округляем до минуты вверх: 2 ч 14 мин 30 с → 2 ч 15 мин.
        FormatDuration(static_cast<int>((durationMs + 59999ULL) / 60000ULL),
                       duration, ARRAYSIZE(duration));
        FormatEndTime(durationMs, endTime, ARRAYSIZE(endTime));
        StringCchPrintfW(text, ARRAYSIZE(text), Str(S_NOTIFY_TIMER_START_FMT), duration, endTime);
        ShowNotification(text);
    }
}

void StartTimer(int minutes, bool notify) {
    StartTimerMs(static_cast<ULONGLONG>(minutes) * 60000ULL, notify);
}

// «Продлить на 1 час»: работает только при идущем таймере.
void ExtendTimer() {
    if (!g_timerRunning) return;

    constexpr ULONGLONG kExtendMs = 60ULL * 60000ULL;
    g_deadlineMs += kExtendMs;
    UpdateTrayIcon();

    wchar_t endTime[32];
    wchar_t text[256];
    FormatEndTime(RemainingMs(), endTime, ARRAYSIZE(endTime));
    StringCchPrintfW(text, ARRAYSIZE(text), Str(S_NOTIFY_TIMER_EXTEND_FMT), endTime);
    ShowNotification(text);
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
        SetAwake(false, true);
    } else {
        SetAwake(true, true);
    }
}

// ---------------------------------------------------------------------------
// Контекстное меню
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Верх меню, который рисуем сами, чтобы он не выглядел недоступным:
// шапка (зелёный глаз, «KeepAwake» жирным, версия), строка статуса
// с цветной точкой и строка таймера. Пункты не нажимаются.
// ---------------------------------------------------------------------------

constexpr wchar_t kHeaderName[] = L"KeepAwake";
constexpr wchar_t kHeaderVersion[] = L"" KA_VERSION_STRING;

constexpr COLORREF kDotOnColor = RGB(0x0E, 0x96, 0x68);   // зелёный, как значок
constexpr COLORREF kDotOffColor = RGB(0x9A, 0x9A, 0x9A);  // серый, как значок
constexpr int kDotSize = 8;                               // диаметр точки при 100 %

UINT g_menuDpi = USER_DEFAULT_SCREEN_DPI;  // масштаб монитора, где открыто меню
HICON g_menuIcon = nullptr;                // значок для шапки под этот масштаб

// Размеры обычных пунктов меню, чтобы наши строки стояли с ними ровно.
struct MenuMetrics {
    int checkWidth = 0;  // колонка слева, где у пунктов галочка
    int textX = 0;       // начало текста от левого края пункта
    int checkHeight = 0;
    int padY = 0;        // отступы сверху и снизу у текста пункта
};
MenuMetrics g_menuMetrics;

int MenuScale(int px) {
    return MulDiv(px, static_cast<int>(g_menuDpi), USER_DEFAULT_SCREEN_DPI);
}

// Тема меню под масштаб монитора. OpenThemeDataForDpi берём из uxtheme.dll
// во время работы: её нет в библиотеках MinGW.
HTHEME OpenMenuTheme(HWND hwnd) {
    using OpenForDpi = HTHEME(WINAPI*)(HWND, LPCWSTR, UINT);
    static const auto openForDpi = reinterpret_cast<OpenForDpi>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), "OpenThemeDataForDpi")));
    if (openForDpi) return openForDpi(hwnd, L"MENU", g_menuDpi);
    return OpenThemeData(hwnd, L"MENU");
}

// Берём размеры из темы меню Windows; без темы — из системных метрик.
void ComputeMenuMetrics() {
    MenuMetrics m;
    HTHEME theme = OpenMenuTheme(g_window);
    if (theme) {
        SIZE check = {};
        MARGINS checkMargins = {};
        MARGINS checkBgMargins = {};
        MARGINS itemMargins = {};
        int gutter = 0;
        GetThemePartSize(theme, nullptr, MENU_POPUPCHECK, 0, nullptr, TS_TRUE, &check);
        GetThemeMargins(theme, nullptr, MENU_POPUPCHECK, 0, TMT_CONTENTMARGINS, nullptr,
                        &checkMargins);
        GetThemeMargins(theme, nullptr, MENU_POPUPCHECKBACKGROUND, 0, TMT_CONTENTMARGINS,
                        nullptr, &checkBgMargins);
        GetThemeMargins(theme, nullptr, MENU_POPUPITEM, 0, TMT_CONTENTMARGINS, nullptr,
                        &itemMargins);
        GetThemeInt(theme, MENU_POPUPBACKGROUND, 0, TMT_BORDERSIZE, &gutter);
        CloseThemeData(theme);

        m.checkWidth = check.cx + checkMargins.cxLeftWidth + checkMargins.cxRightWidth +
                       checkBgMargins.cxLeftWidth + checkBgMargins.cxRightWidth;
        m.checkHeight = check.cy + checkMargins.cyTopHeight + checkMargins.cyBottomHeight +
                        checkBgMargins.cyTopHeight + checkBgMargins.cyBottomHeight;
        m.textX = m.checkWidth + gutter + itemMargins.cxLeftWidth;
        m.padY = itemMargins.cyTopHeight + itemMargins.cyBottomHeight;
    }
    if (m.checkWidth <= 0) {
        m.checkWidth = GetSystemMetricsForDpi(SM_CXMENUCHECK, g_menuDpi) + MenuScale(4);
        m.checkHeight = GetSystemMetricsForDpi(SM_CYMENUCHECK, g_menuDpi);
        m.textX = m.checkWidth + MenuScale(4);
        m.padY = MenuScale(6);
    }
    g_menuMetrics = m;
}

// Шрифт меню Windows под масштаб монитора; bold — жирный вариант.
HFONT CreateMenuFont(bool bold) {
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_menuDpi)) {
        return nullptr;
    }
    if (bold) ncm.lfMenuFont.lfWeight = FW_BOLD;
    return CreateFontIndirectW(&ncm.lfMenuFont);
}

SIZE TextSize(HDC dc, HFONT font, const wchar_t* text) {
    SIZE size = {};
    HGDIOBJ old = SelectObject(dc, font);
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
    SelectObject(dc, old);
    return size;
}

// Текст строки статуса или таймера.
void InfoLineText(UINT id, wchar_t* out, size_t outCount) {
    if (id == ID_STATUS) {
        StringCchCopyW(out, outCount, Str(g_awake ? S_STATUS_ON : S_STATUS_OFF));
    } else {
        TimerStatusText(out, outCount);
    }
}

void MeasureMenuLine(MEASUREITEMSTRUCT* mis) {
    HFONT bold = CreateMenuFont(true);
    HFONT regular = CreateMenuFont(false);
    HDC dc = GetDC(nullptr);

    int width = 0;
    int textHeight = 0;
    if (mis->itemID == ID_VERSION) {
        const SIZE name = TextSize(dc, bold, kHeaderName);
        const SIZE version = TextSize(dc, regular, kHeaderVersion);
        width = g_menuMetrics.textX + name.cx + MenuScale(6) + version.cx;
        textHeight = name.cy > version.cy ? name.cy : version.cy;
    } else {
        wchar_t text[64];
        InfoLineText(mis->itemID, text, ARRAYSIZE(text));
        const SIZE size = TextSize(dc, regular, text);
        width = g_menuMetrics.textX + size.cx;
        textHeight = size.cy;
    }

    ReleaseDC(nullptr, dc);
    if (bold) DeleteObject(bold);
    if (regular) DeleteObject(regular);

    int height = textHeight + g_menuMetrics.padY;
    if (height < g_menuMetrics.checkHeight) height = g_menuMetrics.checkHeight;
    if (mis->itemID == ID_VERSION) {
        const int iconHeight = GetSystemMetricsForDpi(SM_CYSMICON, g_menuDpi) + MenuScale(8);
        if (height < iconHeight) height = iconHeight;
    }

    // Windows сама добавляет к ширине колонку галочки, вычитаем её.
    width += MenuScale(16) - GetSystemMetricsForDpi(SM_CXMENUCHECK, g_menuDpi);
    mis->itemWidth = static_cast<UINT>(width > 0 ? width : 0);
    mis->itemHeight = static_cast<UINT>(height);
}

// Круглая точка со сглаженным краем.
void DrawDot(HDC dc, int centerX, int centerY, int size, COLORREF color) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return;

    // Покрытие пикселя считаем по сетке 4×4 и записываем цвет с альфой.
    auto* pixels = static_cast<DWORD*>(bits);
    const double radius = size / 2.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int inside = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const double dx = x + (sx + 0.5) / 4.0 - radius;
                    const double dy = y + (sy + 0.5) / 4.0 - radius;
                    if (dx * dx + dy * dy <= radius * radius) ++inside;
                }
            }
            const DWORD a = static_cast<DWORD>(inside * 255 / 16);
            const DWORD r = GetRValue(color) * a / 255;
            const DWORD g = GetGValue(color) * a / 255;
            const DWORD b = GetBValue(color) * a / 255;
            pixels[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    HDC memDc = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(memDc, bitmap);
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, centerX - size / 2, centerY - size / 2, size, size, memDc, 0, 0, size, size,
               blend);
    SelectObject(memDc, old);
    DeleteDC(memDc);
    DeleteObject(bitmap);
}

void DrawMenuLine(const DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    const RECT rc = dis->rcItem;

    // Фон и цвет текста берём из темы меню, как у обычных пунктов.
    // Фон рисуем сами, чтобы при перерисовке текст не накладывался.
    // Без темы оставляем фон, который нарисовала Windows.
    COLORREF textColor = GetSysColor(COLOR_MENUTEXT);
    HWND menuWindow = WindowFromDC(dc);
    HTHEME theme = menuWindow ? OpenMenuTheme(menuWindow) : nullptr;
    if (theme) {
        RECT menuRect = rc;
        GetClientRect(menuWindow, &menuRect);
        DrawThemeBackground(theme, dc, MENU_POPUPBACKGROUND, 0, &menuRect, &rc);
        GetThemeColor(theme, MENU_POPUPITEM, MPI_NORMAL, TMT_TEXTCOLOR, &textColor);
        CloseThemeData(theme);
    }

    HFONT bold = CreateMenuFont(true);
    HFONT regular = CreateMenuFont(false);
    const int middle = (rc.top + rc.bottom) / 2;
    const int checkCenter = rc.left + g_menuMetrics.checkWidth / 2;
    int x = rc.left + g_menuMetrics.textX;

    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, textColor);
    HGDIOBJ oldFont = SelectObject(dc, regular);

    if (dis->itemID == ID_VERSION) {
        // Значок в колонке галочки, название и версия — по линии текста пунктов.
        const int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, g_menuDpi);
        if (g_menuIcon) {
            DrawIconEx(dc, checkCenter - iconSize / 2, middle - iconSize / 2, g_menuIcon,
                       iconSize, iconSize, 0, nullptr, DI_NORMAL);
        }
        const SIZE name = TextSize(dc, bold, kHeaderName);
        SelectObject(dc, bold);
        TextOutW(dc, x, middle - name.cy / 2, kHeaderName, ARRAYSIZE(kHeaderName) - 1);
        x += name.cx + MenuScale(6);
        const SIZE version = TextSize(dc, regular, kHeaderVersion);
        SelectObject(dc, regular);
        TextOutW(dc, x, middle - version.cy / 2, kHeaderVersion, ARRAYSIZE(kHeaderVersion) - 1);
    } else {
        if (dis->itemID == ID_STATUS) {
            DrawDot(dc, checkCenter, middle, MenuScale(kDotSize),
                    g_awake ? kDotOnColor : kDotOffColor);
        }
        wchar_t text[64];
        InfoLineText(dis->itemID, text, ARRAYSIZE(text));
        const SIZE size = TextSize(dc, regular, text);
        TextOutW(dc, x, middle - size.cy / 2, text, static_cast<int>(wcslen(text)));
    }

    SelectObject(dc, oldFont);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    if (bold) DeleteObject(bold);
    if (regular) DeleteObject(regular);
}

bool IsCustomMenuLine(UINT id) {
    return id == ID_VERSION || id == ID_STATUS || id == ID_TIMER_STATUS;
}

HMENU BuildMenu() {
    HMENU menu = CreatePopupMenu();
    wchar_t text[128];

    // Шапка, статус и таймер рисуем сами (DrawMenuLine), чтобы они
    // не выглядели как недоступные пункты.
    AppendMenuW(menu, MF_OWNERDRAW | MF_DISABLED, ID_VERSION, nullptr);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | MF_DISABLED, ID_STATUS, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | MF_DISABLED, ID_TIMER_STATUS, nullptr);

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

    StartTimerMs(static_cast<ULONGLONG>(durationMs), true);
}

void SetLanguage(Lang lang) {
    g_lang = lang;
    SaveLanguage(lang);
    UpdateTrayIcon();
}

void HandleCommand(UINT id) {
    if (id >= ID_TIMER_FIRST && id < ID_TIMER_FIRST + kTimerCount) {
        StartTimer(kTimerMinutes[id - ID_TIMER_FIRST], true);
        return;
    }

    switch (id) {
    case ID_ENABLE:
        SetAwake(true, true);
        break;
    case ID_DISABLE:
        StopTimer(TimerStop::Silent);
        SetAwake(false, true);
        break;
    case ID_KEEP_DISPLAY:
        ToggleKeepDisplay();
        break;
    case ID_TIMER_CANCEL:
        StopTimer(TimerStop::Cancelled);
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
    case ID_EXIT:
        DestroyWindow(g_window);
        break;
    default:
        break;
    }
}

void ShowTrayMenu(int x, int y) {
    // Масштаб монитора, на котором откроется меню, — для шапки.
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    const POINT pt = { x, y };
    if (FAILED(GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST),
                                MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpiY = USER_DEFAULT_SCREEN_DPI;
    }
    g_menuDpi = dpiY;
    ComputeMenuMetrics();
    const int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, g_menuDpi);
    LoadIconWithScaleDown(g_instance, MAKEINTRESOURCEW(IDI_KEEPAWAKE_ON), iconSize, iconSize,
                          &g_menuIcon);

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
    if (g_menuIcon) {
        DestroyIcon(g_menuIcon);
        g_menuIcon = nullptr;
    }
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

    case WM_MEASUREITEM: {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis->CtlType == ODT_MENU && IsCustomMenuLine(mis->itemID)) {
            MeasureMenuLine(mis);
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_DRAWITEM: {
        const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlType == ODT_MENU && IsCustomMenuLine(dis->itemID)) {
            DrawMenuLine(dis);
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_HOTKEY:
        if (wParam == kHotkeyId) {
            // Открытое меню показывало бы устаревшее состояние.
            if (g_openMenu) EndMenu();
            ToggleAwake();
        }
        return 0;

    case WM_TIMER:
        if (wParam == kCountdownTimerId) OnCountdownTick();
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
    StartTimer(kStartupMinutes, false);

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
