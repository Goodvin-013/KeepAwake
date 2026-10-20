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

// При ручном запуске запрет сна включается на 10 часов.
constexpr int kStartupMinutes = 600;

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

void FormatDuration(int minutes, wchar_t* out, size_t outCount) {
    if (minutes < 60) {
        StringCchPrintfW(out, outCount,
                 g_lang == LANG_RU_UI ? L"%d минут" : L"%d minutes", minutes);
        return;
    }

    const int hours = minutes / 60;

    if (g_lang == LANG_EN_UI) {
        StringCchPrintfW(out, outCount, hours == 1 ? L"%d hour" : L"%d hours", hours);
        return;
    }

    const wchar_t* word = L"часов";
    if (hours == 1) word = L"час";
    else if (hours >= 2 && hours <= 4) word = L"часа";

    StringCchPrintfW(out, outCount, L"%d %ls", hours, word);
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
void FormatEndTime(int minutes, wchar_t* out, int outCount) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    FILETIME ft = {};
    SystemTimeToFileTime(&now, &ft);

    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    t.QuadPart += static_cast<ULONGLONG>(minutes) * 60ULL * 10000000ULL;
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
// зелёный в синей кайме — запрещены и сон, и выключение экрана.
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

bool SetAwake(bool enable, bool notify) {
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

void StartTimer(int minutes, bool notify) {
    if (!SetAwake(true, false)) return;

    g_deadlineMs = GetTickCount64() + static_cast<ULONGLONG>(minutes) * 60000ULL;
    g_timerRunning = true;
    SetTimer(g_window, kCountdownTimerId, kCountdownIntervalMs, nullptr);
    UpdateTrayIcon();

    if (notify) {
        wchar_t duration[32];
        wchar_t endTime[32];
        wchar_t text[256];

        FormatDuration(minutes, duration, ARRAYSIZE(duration));
        FormatEndTime(minutes, endTime, ARRAYSIZE(endTime));
        StringCchPrintfW(text, ARRAYSIZE(text), Str(S_NOTIFY_TIMER_START_FMT), duration, endTime);
        ShowNotification(text);
    }
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

HMENU BuildMenu() {
    HMENU menu = CreatePopupMenu();
    wchar_t text[128];

    // Название и версия программы, чтобы было видно, какая версия запущена.
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_VERSION, L"KeepAwake " KA_VERSION_STRING);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_STATUS,
                Str(g_awake ? S_STATUS_ON : S_STATUS_OFF));

    TimerStatusText(text, ARRAYSIZE(text));
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_TIMER_STATUS, text);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (g_awake ? MF_GRAYED : 0), ID_ENABLE, Str(S_ENABLE));
    AppendMenuW(menu, MF_STRING | (g_awake ? 0 : MF_GRAYED), ID_DISABLE, Str(S_DISABLE));
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

    case WM_TIMER:
        if (wParam == kCountdownTimerId) OnCountdownTick();
        return 0;

    case WM_ENDSESSION:
        // Выход из Windows или установщик просит закрыть программу.
        if (wParam) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        ReleaseAwake();
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

    // Ручной запуск включает запрет сна на 10 часов, без уведомления.
    StartTimer(kStartupMinutes, false);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    FreeIcons();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
