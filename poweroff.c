/* PowerOff — tiny auto shutdown scheduler (C version).
 *
 * 100% native Win32 GDI + common controls. The visuals copy the Windows 11
 * Settings app: rounded cards, WinUI-style toggles, system accent color,
 * Segoe UI Variable, light/dark theme following. No UI framework, no XAML,
 * no custom design language — just Settings rows painted by hand (Win32 has
 * no card or toggle controls to reuse).
 *
 * One file. Build with build.bat (MSVC). Scheduler/tray/CLI mirror the
 * Rust implementation feature-for-feature.
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <powrprof.h>
#include <dwmapi.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <io.h>
#include <strsafe.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

/* ------------------------------------------------------------------ model */

typedef enum { T_SHUTDOWN, T_RESTART, T_POWEROFF, T_LOGOFF, T_LOCK, T_SLEEP, T_HIBERNATE } Task;
typedef enum { M_DAILY, M_ONCE, M_COUNTDOWN, M_IDLE, M_INTERVAL } Mode;

static const wchar_t *TASK_NAMES[] = { L"Shut down", L"Restart", L"Power off", L"Log off", L"Lock", L"Sleep", L"Hibernate" };
static const wchar_t *MODE_NAMES[] = { L"Daily", L"Once", L"Countdown", L"Idle", L"Repeat every" };
static const wchar_t *MODE_HINTS[] = {
    L"Runs every day at this time",
    L"Runs once at the given date and time",
    L"Runs after the countdown elapses",
    L"Runs when mouse and keyboard are idle that long",
    L"Repeats the task until cancelled",
};
#define NTASKS 7
#define NMODES 5

typedef struct {
    Task task;
    Mode mode;
    wchar_t p1[64];
    wchar_t p2[64];
    wchar_t p3[64];
    int remind_min; /* 0 = off, else minutes (5) */
} Plan;

typedef struct {
    Plan plan;
    bool active;
    bool reminded;
    long long fire_at; /* local epoch seconds */
    long long span;    /* total span, for progress */
    long long delay;   /* extra delay from reminder dialog */
} Runtime;

/* reminder dialog -> scheduler signalling */
static volatile LONG g_fire_now = 0;
static volatile LONG g_delay_secs = 0;
static volatile LONG g_cancel_req = 0;

/* ------------------------------------------------------------------ globals */

typedef struct {
    HWND main;
    HWND task_combo, mode_combo;
    HWND etime, edate, etime2, eh, em, es, en;
    HWND prog, btn_start;
    HWND tgl_remind, tgl_startup;
    HWND chev_power;
    HWND row_action, row_sched, row_time, row_hero, row_big, row_remind, row_startup, row_powerlink;
    HWND card_hero, card_sched, card_status, card_opts;
    HFONT f_title, f_mid, f_ui, f_big;
    bool ready;
    bool dark;
    DWORD accent;      /* 0x00BBGGRR */
    bool remind_on;
    bool startup_on;
    Runtime rt;
} Globals;
static Globals G;

/* forward declarations (defined after wndproc) */
static void refresh_inputs(void);
static void read_plan(Plan *p);
static void save_config_current(void);
static void update_views(void);
static void tick(void);
static void toggle_start(void);

/* control ids */
enum {
    ID_CARD_HERO = 101, ID_CARD_SCHED, ID_CARD_STATUS, ID_CARD_OPTS,
    ID_ROW_HERO, ID_ROW_ACTION, ID_ROW_SCHED, ID_ROW_TIME, ID_ROW_BIG,
    ID_ROW_REMIND, ID_ROW_STARTUP, ID_ROW_POWERLINK,
    ID_TASK_COMBO, ID_MODE_COMBO,
    ID_ETIME, ID_EDATE, ID_ETIME2, ID_EDITH, ID_EDITM, ID_EDITS, ID_EDITN,
    ID_TGL_REMIND, ID_TGL_STARTUP, ID_CHEV_POWER,
    ID_PROG, ID_BTN_START,
    IDM_SHOW = 1001, IDM_START, IDM_NOW, IDM_EXIT,
};
#define WM_TRAY (WM_APP + 1)
#define TIMER_ID 1

/* ------------------------------------------------------------------ time */

static long long days_from_civil(int y, unsigned m, unsigned d) {
    int yy = (m <= 2) ? y - 1 : y;
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    int yoe = yy - era * 400;
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + (int)doy;
    return (long long)era * 146097 + doe - 719468;
}

static long long local_now_secs(void) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return days_from_civil(st.wYear, st.wMonth, st.wDay) * 86400LL
        + st.wHour * 3600LL + st.wMinute * 60LL + st.wSecond;
}

static unsigned long long idle_secs(void) {
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(lii);
    if (GetLastInputInfo(&lii))
        return (GetTickCount64() - lii.dwTime) / 1000;
    return 0;
}

static void fmt_dur(long long secs, wchar_t *out, size_t n) {
    if (secs < 0) secs = 0;
    long long h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    if (h > 0) StringCchPrintfW(out, n, L"%lld:%02lld:%02lld", h, m, s);
    else StringCchPrintfW(out, n, L"%lld:%02lld", m, s);
}

static bool parse_hhmm(const wchar_t *s, unsigned *h, unsigned *m) {
    while (*s == L' ' || *s == L'\t') s++;
    wchar_t *end = NULL;
    unsigned long hh = wcstoul(s, &end, 10);
    if (end == s || hh > 23) return false;
    unsigned long mm = 0;
    if (*end == L':') {
        const wchar_t *p = end + 1;
        while (*p == L' ' || *p == L'\t') p++;
        mm = wcstoul(p, &end, 10);
        if (end == p || mm > 59) return false;
    }
    while (*end == L' ' || *end == L'\t') end++;
    if (*end != L'\0') return false;
    *h = (unsigned)hh; *m = (unsigned)mm;
    return true;
}

static bool parse_ymd(const wchar_t *s, int *y, unsigned *mo, unsigned *d) {
    int yy = 0; unsigned mm = 0, dd = 0;
    if (swscanf_s(s, L"%d-%u-%u", &yy, &mm, &dd) != 3) return false;
    if (yy < 1970 || yy > 2100 || mm < 1 || mm > 12 || dd < 1 || dd > 31) return false;
    *y = yy; *mo = mm; *d = dd;
    return true;
}

/* ------------------------------------------------------------------ power */

static void enable_shutdown_privilege(void) {
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        LUID luid;
        if (LookupPrivilegeValueW(NULL, L"SeShutdownPrivilege", &luid)) {
            TOKEN_PRIVILEGES tp;
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        }
        CloseHandle(tok);
    }
}

static bool do_task(Task t) {
    switch (t) {
    case T_LOCK: return LockWorkStation() != 0;
    case T_SLEEP: return SetSuspendState(FALSE, TRUE, FALSE) != 0;
    case T_HIBERNATE: return SetSuspendState(TRUE, TRUE, FALSE) != 0;
    case T_LOGOFF: enable_shutdown_privilege(); return ExitWindowsEx(EWX_LOGOFF | EWX_FORCEIFHUNG, 0) != 0;
    case T_SHUTDOWN: enable_shutdown_privilege(); return ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCEIFHUNG, 0) != 0;
    case T_POWEROFF: enable_shutdown_privilege(); return ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG, 0) != 0;
    case T_RESTART: enable_shutdown_privilege(); return ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, 0) != 0;
    }
    return false;
}

/* ------------------------------------------------------------------ scheduler */

static void rt_describe(const Runtime *r, wchar_t *out, size_t n) {
    const wchar_t *tn = TASK_NAMES[r->plan.task];
    switch (r->plan.mode) {
    case M_DAILY: {
        unsigned h = 23, m = 0, a, b;
        if (parse_hhmm(r->plan.p1, &a, &b)) { /* p1 holds hour alone in CLI path */ }
        (void)a; (void)b;
        h = (unsigned)wcstoul(r->plan.p1, NULL, 10);
        m = (unsigned)wcstoul(r->plan.p2, NULL, 10);
        StringCchPrintfW(out, n, L"%s daily at %02u:%02u", tn, h, m);
        break;
    }
    case M_ONCE:
        StringCchPrintfW(out, n, L"%s once at %s %s", tn, r->plan.p1, r->plan.p2);
        break;
    case M_COUNTDOWN:
        StringCchPrintfW(out, n, L"%s in %sh %sm %ss", tn, r->plan.p1, r->plan.p2, r->plan.p3);
        break;
    case M_IDLE:
        StringCchPrintfW(out, n, L"%s after %s min idle", tn, r->plan.p1);
        break;
    case M_INTERVAL:
        StringCchPrintfW(out, n, L"%s every %s min", tn, r->plan.p1);
        break;
    }
}

/* returns NULL on success (with desc filled), or a static error string */
static const wchar_t *rt_start(Runtime *r, wchar_t *desc, size_t n) {
    long long now = local_now_secs();
    r->reminded = false;
    r->delay = 0;
    switch (r->plan.mode) {
    case M_DAILY: {
        unsigned h, m;
        wchar_t both[128];
        StringCchPrintfW(both, 128, L"%s:%s", r->plan.p1, r->plan.p2);
        if (!parse_hhmm(both, &h, &m)) return L"Hour must be 0-23 and minute 0-59";
        r->fire_at = 0;
        r->span = 86400;
        break;
    }
    case M_ONCE: {
        int y; unsigned mo, d, h, mi;
        if (!parse_ymd(r->plan.p1, &y, &mo, &d)) return L"Date must be YYYY-MM-DD";
        if (!parse_hhmm(r->plan.p2, &h, &mi)) return L"Time must be HH:MM (24h)";
        long long at = days_from_civil(y, mo, d) * 86400LL + h * 3600LL + mi * 60LL;
        if (at <= now) return L"That date/time is in the past.";
        r->fire_at = at;
        r->span = at - now;
        break;
    }
    case M_COUNTDOWN: {
        long long h = wcstoll(r->plan.p1, NULL, 10);
        long long m = wcstoll(r->plan.p2, NULL, 10);
        long long s = wcstoll(r->plan.p3, NULL, 10);
        /* reject non-numeric junk: wcstoll silently yields 0, so re-check */
        if (r->plan.p1[0] == L'\0' || r->plan.p2[0] == L'\0' || r->plan.p3[0] == L'\0')
            return L"Countdown must be > 0.";
        long long total = h * 3600 + m * 60 + s;
        if (total <= 0) return L"Countdown must be > 0.";
        r->fire_at = now + total;
        r->span = total;
        break;
    }
    case M_IDLE: {
        long long mins = wcstoll(r->plan.p1, NULL, 10);
        if (mins <= 0) return L"Idle minutes must be > 0.";
        r->fire_at = 0;
        r->span = mins * 60;
        break;
    }
    case M_INTERVAL: {
        long long mins = wcstoll(r->plan.p1, NULL, 10);
        if (mins <= 0) return L"Interval minutes must be > 0.";
        r->fire_at = now + mins * 60;
        r->span = mins * 60;
        break;
    }
    }
    r->active = true;
    rt_describe(r, desc, n);
    return NULL;
}

static long long rt_remaining(Runtime *r) {
    long long now = local_now_secs();
    switch (r->plan.mode) {
    case M_DAILY: {
        long long h = wcstoll(r->plan.p1, NULL, 10);
        long long m = wcstoll(r->plan.p2, NULL, 10);
        SYSTEMTIME st;
        GetLocalTime(&st);
        long long nows = st.wHour * 3600LL + st.wMinute * 60LL + st.wSecond;
        long long d = h * 3600 + m * 60 - nows;
        if (d <= 0) d += 86400;
        return d;
    }
    case M_IDLE: {
        long long mins = wcstoll(r->plan.p1, NULL, 10);
        return mins * 60 - (long long)idle_secs();
    }
    default:
        return r->fire_at + r->delay - now;
    }
}

static double rt_fraction(Runtime *r, long long rem) {
    if (r->span <= 0) return 0.0;
    double f = (double)(r->span - rem) / (double)r->span;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return f;
}

static long long rt_daily_raw(Runtime *r) {
    long long h = wcstoll(r->plan.p1, NULL, 10);
    long long m = wcstoll(r->plan.p2, NULL, 10);
    SYSTEMTIME st;
    GetLocalTime(&st);
    long long nows = st.wHour * 3600LL + st.wMinute * 60LL + st.wSecond;
    long long d = h * 3600 + m * 60 - nows;
    if (d <= 0) d += 86400;
    return d;
}

static void rt_apply_signals(Runtime *r) {
    if (InterlockedExchange(&g_cancel_req, 0)) r->active = false;
    LONG d = InterlockedExchange(&g_delay_secs, 0);
    if (d != 0) {
        if (r->plan.mode == M_DAILY) {
            long long rem = rt_daily_raw(r) + d;
            r->plan.mode = M_COUNTDOWN;
            StringCchPrintfW(r->plan.p1, 64, L"%lld", rem / 3600);
            StringCchPrintfW(r->plan.p2, 64, L"%lld", (rem % 3600) / 60);
            StringCchPrintfW(r->plan.p3, 64, L"%lld", rem % 60);
            r->fire_at = local_now_secs() + rem;
            r->span = rem;
            r->delay = 0;
        } else {
            r->delay += d;
        }
        r->reminded = true;
    }
    if (InterlockedExchange(&g_fire_now, 0)) {
        r->fire_at = local_now_secs();
        r->delay = 0;
        if (r->plan.mode == M_DAILY || r->plan.mode == M_IDLE) {
            r->plan.mode = M_COUNTDOWN;
            wcscpy_s(r->plan.p1, 64, L"0");
            wcscpy_s(r->plan.p2, 64, L"0");
            wcscpy_s(r->plan.p3, 64, L"0");
        }
    }
}

/* ------------------------------------------------------------------ config */

static void ini_path(wchar_t *out, size_t n) {
    wchar_t base[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        StringCchPrintfW(out, n, L".\\poweroff.ini");
        return;
    }
    StringCchPrintfW(out, n, L"%s\\PowerOff\\poweroff.ini", base);
}

static void save_config(const Plan *p) {
    wchar_t path[MAX_PATH], dir[MAX_PATH];
    ini_path(path, MAX_PATH);
    StringCchCopyW(dir, MAX_PATH, path);
    wchar_t *bs = wcsrchr(dir, L'\\');
    if (bs) { *bs = L'\0'; CreateDirectoryW(dir, NULL); }
    wchar_t task[16], mode[16], rem[16];
    StringCchPrintfW(task, 16, L"%d", (int)p->task);
    StringCchPrintfW(mode, 16, L"%d", (int)p->mode);
    StringCchPrintfW(rem, 16, L"%d", p->remind_min);
    WritePrivateProfileStringW(L"PowerOff", L"task", task, path);
    WritePrivateProfileStringW(L"PowerOff", L"mode", mode, path);
    WritePrivateProfileStringW(L"PowerOff", L"p1", p->p1, path);
    WritePrivateProfileStringW(L"PowerOff", L"p2", p->p2, path);
    WritePrivateProfileStringW(L"PowerOff", L"p3", p->p3, path);
    WritePrivateProfileStringW(L"PowerOff", L"remind", rem, path);
}

static void load_config(Plan *p) {
    wchar_t path[MAX_PATH], buf[128];
    p->task = T_SLEEP; p->mode = M_DAILY;
    wcscpy_s(p->p1, 64, L"23"); wcscpy_s(p->p2, 64, L"00"); wcscpy_s(p->p3, 64, L"00");
    p->remind_min = 5;
    ini_path(path, MAX_PATH);
    int t = GetPrivateProfileIntW(L"PowerOff", L"task", T_SLEEP, path);
    int m = GetPrivateProfileIntW(L"PowerOff", L"mode", M_DAILY, path);
    if (t >= 0 && t < NTASKS) p->task = (Task)t;
    if (m >= 0 && m < NMODES) p->mode = (Mode)m;
    GetPrivateProfileStringW(L"PowerOff", L"p1", p->p1, p->p1, 64, path);
    GetPrivateProfileStringW(L"PowerOff", L"p2", p->p2, p->p2, 64, path);
    GetPrivateProfileStringW(L"PowerOff", L"p3", p->p3, p->p3, 64, path);
    (void)buf;
    p->remind_min = GetPrivateProfileIntW(L"PowerOff", L"remind", 5, path);
}

static void set_startup(bool on) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
        if (on) {
            wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16];
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            StringCchPrintfW(cmd, MAX_PATH + 16, L"\"%s\" --tray", exe);
            RegSetValueExW(hk, L"PowerOff", 0, REG_SZ,
                (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hk, L"PowerOff");
        }
        RegCloseKey(hk);
    }
}

static bool get_startup(void) {
    HKEY hk;
    bool ok = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
        DWORD type = 0, cb = 0;
        ok = RegQueryValueExW(hk, L"PowerOff", NULL, &type, NULL, &cb) == ERROR_SUCCESS;
        RegCloseKey(hk);
    }
    return ok;
}

static bool run_schtasks(const wchar_t *args, wchar_t *err, size_t n) {
    wchar_t cmd[1024];
    StringCchPrintfW(cmd, 1024, L"schtasks.exe %s", args);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        StringCchPrintfW(err, n, L"schtasks failed to start (0x%08X)", GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) {
        StringCchPrintfW(err, n, L"schtasks exited with code %u (run elevated?)", code);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ theme */

static COLORREF app_bg(void) { return G.dark ? RGB(32, 32, 32) : RGB(243, 243, 243); }
static COLORREF card_bg(void) { return G.dark ? RGB(45, 45, 45) : RGB(255, 255, 255); }
static COLORREF card_edge(void) { return G.dark ? RGB(70, 70, 70) : RGB(229, 229, 229); }
static COLORREF ink(void) { return G.dark ? RGB(255, 255, 255) : RGB(27, 27, 27); }
static COLORREF ink2(void) { return G.dark ? RGB(173, 171, 164) : RGB(96, 94, 92); }

static void read_theme(void) {
    HKEY hk;
    G.dark = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
        DWORD type = 0, data = 1, cb = sizeof(data);
        if (RegQueryValueExW(hk, L"AppsUseLightTheme", NULL, &type, (BYTE *)&data, &cb) == ERROR_SUCCESS)
            G.dark = (data == 0);
        RegCloseKey(hk);
    }
}

typedef int (WINAPI *SetPrefModeFn)(int);
typedef int (WINAPI *AllowDarkFn)(HWND, BOOL);
typedef void (WINAPI *RefreshPolicyFn)(void);

static void apply_dark_mode(HWND hwnd) {
    BOOL on = G.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &on, sizeof(on));
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (ux) {
        SetPrefModeFn set_mode = (SetPrefModeFn)GetProcAddress(ux, (LPCSTR)135);
        if (set_mode) set_mode(G.dark ? 1 : 0);
        AllowDarkFn allow = (AllowDarkFn)GetProcAddress(ux, (LPCSTR)133);
        if (allow) allow(hwnd, on);
        if (G.dark) {
            SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
        } else {
            SetWindowTheme(hwnd, NULL, NULL);
        }
        RefreshPolicyFn refresh = (RefreshPolicyFn)GetProcAddress(ux, (LPCSTR)104);
        if (refresh) refresh();
    }
}

static void read_accent(void) {
    DWORD color = 0x0078D4;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        /* ARGB -> COLORREF */
        unsigned r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
        G.accent = RGB(r, g, b);
    } else {
        G.accent = RGB(0, 120, 212);
    }
}

/* ------------------------------------------------------------------ GDI helpers */

static HFONT make_font(const wchar_t *primary, const wchar_t *fallback, int px, int weight) {
    HFONT f = CreateFontW(px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, primary);
    if (!f) f = CreateFontW(px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, fallback);
    return f;
}

static void fill_round(HDC hdc, const RECT *rc, int rad, COLORREF fill, COLORREF edge) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, rc->left, rc->top, rc->right - 1, rc->bottom - 1, rad, rad);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void text_at(HDC hdc, const wchar_t *s, RECT rc, HFONT font, COLORREF color, UINT flags) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    SelectObject(hdc, font);
    DrawTextW(hdc, s, -1, &rc, flags);
}

static void hline_at(HDC hdc, int x0, int x1, int y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ op = SelectObject(hdc, pen);
    MoveToEx(hdc, x0, y, NULL);
    LineTo(hdc, x1, y);
    SelectObject(hdc, op);
    DeleteObject(pen);
}

/* small monochrome glyphs, 20x20 box at (x,y) */
typedef enum { GL_POWER, GL_CLOCK, GL_CAL, GL_BELL, GL_ARROW, GL_MON } Glyph;

static void draw_glyph(HDC hdc, int x, int y, Glyph g, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ op = SelectObject(hdc, pen);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    int cx = x + 10, cy = y + 10;
    switch (g) {
    case GL_POWER: {
        /* arc ~300deg + stem */
        Arc(hdc, cx - 7, cy - 6, cx + 7, cy + 8, cx + 7, cy - 6, cx - 7, cy - 6);
        MoveToEx(hdc, cx, y + 1, NULL);
        LineTo(hdc, cx, cy - 1);
        break;
    }
    case GL_CLOCK:
        Ellipse(hdc, cx - 7, cy - 7, cx + 7, cy + 7);
        MoveToEx(hdc, cx, cy, NULL);
        LineTo(hdc, cx, cy - 5);
        MoveToEx(hdc, cx, cy, NULL);
        LineTo(hdc, cx + 4, cy + 1);
        break;
    case GL_CAL: {
        RECT r = { x + 3, y + 5, x + 17, y + 17 };
        fill_round(hdc, &r, 6, color, color); /* solid mini calendar */
        break;
    }
    case GL_BELL: {
        POINT pts[5] = { {cx - 6, cy + 4}, {cx - 4, cy - 4}, {cx - 2, cy - 7},
                         {cx + 2, cy - 7}, {cx + 4, cy - 4} };
        /* dome-ish: use chord + clapper */
        Ellipse(hdc, cx - 6, cy - 7, cx + 6, cy + 5);
        Ellipse(hdc, cx - 2, cy + 5, cx + 2, cy + 9);
        (void)pts;
        break;
    }
    case GL_ARROW:
        Ellipse(hdc, cx - 7, cy - 7, cx + 7, cy + 7);
        MoveToEx(hdc, cx - 3, cy - 3, NULL);
        LineTo(hdc, cx + 3, cy - 3);
        LineTo(hdc, cx + 3, cy + 3);
        break;
    case GL_MON:
        Rectangle(hdc, x + 3, y + 4, x + 17, y + 13);
        MoveToEx(hdc, cx, y + 13, NULL);
        LineTo(hdc, cx, y + 17);
        MoveToEx(hdc, cx - 4, y + 17, NULL);
        LineTo(hdc, cx + 4, y + 17);
        break;
    }
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(pen);
}

/* ------------------------------------------------------------------ controls */

static HWND mk_child(const wchar_t *cls, const wchar_t *text, DWORD style,
        int x, int y, int w, int h, HWND parent, int id, HINSTANCE hi, HFONT font) {
    HWND c = CreateWindowExW(0, cls, text, style, x, y, w, h,
        parent, (HMENU)(INT_PTR)id, hi, NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, 1);
    return c;
}

static void set_text(HWND h, const wchar_t *s) { SetWindowTextW(h, s); }

static void get_text(HWND h, wchar_t *out, size_t n) {
    out[0] = L'\0';
    GetWindowTextW(h, out, (int)n);
}

static void combo_add(HWND h, const wchar_t *s) {
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)s);
}

static int combo_get(HWND h) {
    LRESULT r = SendMessageW(h, CB_GETCURSEL, 0, 0);
    return r < 0 ? 0 : (int)r;
}

static void combo_set(HWND h, int i) {
    SendMessageW(h, CB_SETCURSEL, (WPARAM)i, 0);
}

static void show_ctl(HWND h, bool on) {
    ShowWindow(h, on ? SW_SHOW : SW_HIDE);
}

static bool is_checked(HWND h) {
    return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void set_checked(HWND h, bool on) {
    SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* ------------------------------------------------------------------ plan <-> UI */

/* ------------------------------------------------------------------ tray */

static void add_tray(HWND hwnd) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy_s(nid.szTip, 128, L"PowerOff");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void del_tray(HWND hwnd) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void show_tray_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    wchar_t start[32];
    wcscpy_s(start, 32, G.rt.active ? L"Cancel task" : L"Start task");
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Show window");
    AppendMenuW(menu, MF_STRING, IDM_START, start);
    AppendMenuW(menu, MF_STRING, IDM_NOW, L"Sleep now");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

static DWORD WINAPI reminder_thread(LPVOID param) {
    wchar_t *msg = (wchar_t *)param;
    int r = MessageBoxW(NULL, msg,
        L"PowerOff \x2014 reminder", MB_YESNOCANCEL | MB_ICONWARNING | MB_SYSTEMMODAL);
    free(msg);
    if (r == IDYES) InterlockedExchange(&g_fire_now, 1);
    else if (r == IDNO) InterlockedExchange(&g_delay_secs, 600);
    else InterlockedExchange(&g_cancel_req, 1);
    return 0;
}

static void spawn_reminder(const wchar_t *task_name, long long secs) {
    wchar_t dur[32], *msg;
    fmt_dur(secs, dur, 32);
    msg = (wchar_t *)malloc(512 * sizeof(wchar_t));
    if (!msg) return;
    StringCchPrintfW(msg, 512,
        L"%s in %s.\n\nYes = do it now   \x2022   No = delay 10 min   \x2022   Cancel = cancel task",
        task_name, dur);
    HANDLE th = CreateThread(NULL, 0, reminder_thread, msg, 0, NULL);
    if (th) CloseHandle(th);
    else free(msg);
}

/* ------------------------------------------------------------------ tick/views */

static void update_views(void) {
    Runtime *r = &G.rt;
    if (r->active) {
        long long rem = rt_remaining(r);
        set_text(G.btn_start, L"Cancel task");
        SendMessageW(G.prog, PBM_SETPOS, (WPARAM)(int)(rt_fraction(r, rem) * 100.0), 0);
        if (r->plan.remind_min > 0 && !r->reminded &&
                rem <= (long long)r->plan.remind_min * 60 && rem > 5) {
            r->reminded = true;
            spawn_reminder(TASK_NAMES[r->plan.task], rem);
        }
    } else {
        set_text(G.btn_start, L"Start task");
        SendMessageW(G.prog, PBM_SETPOS, 0, 0);
    }
    /* hero + big number are painted live from G.rt */
    InvalidateRect(G.row_hero, NULL, FALSE);
    InvalidateRect(G.row_big, NULL, FALSE);
}

static void tick(void) {
    Runtime *r = &G.rt;
    rt_apply_signals(r);
    if (r->active && rt_remaining(r) <= 0) {
        Task task = r->plan.task;
        Mode mode = r->plan.mode;
        if (!do_task(task)) {
            wchar_t m[256];
            StringCchPrintfW(m, 256,
                L"Failed to execute '%s'. (Sleep/Hibernate may be disabled on this PC.)",
                TASK_NAMES[task]);
            MessageBoxW(G.main, m, L"PowerOff", MB_OK | MB_ICONERROR);
        }
        switch (mode) {
        case M_INTERVAL: {
            long long mins = wcstoll(r->plan.p1, NULL, 10);
            if (mins < 1) mins = 60;
            r->fire_at = local_now_secs() + mins * 60;
            r->span = mins * 60;
            r->delay = 0;
            r->reminded = false;
            break;
        }
        case M_DAILY:
            r->reminded = false;
            r->delay = 0;
            break;
        case M_IDLE:
            if (task == T_SLEEP || task == T_HIBERNATE || task == T_LOCK)
                r->reminded = false;
            else
                r->active = false;
            break;
        default:
            r->active = false;
            break;
        }
        save_config(&r->plan);
    }
    update_views();
}

static void toggle_start(void) {
    Runtime *r = &G.rt;
    if (r->active) {
        r->active = false;
        update_views();
        return;
    }
    Plan p;
    read_plan(&p);
    r->plan = p;
    {
        wchar_t desc[256];
        const wchar_t *err = rt_start(r, desc, 256);
        if (!err) {
            save_config(&r->plan);
        } else {
            MessageBoxW(G.main, err, L"PowerOff", MB_OK | MB_ICONWARNING);
        }
    }
    update_views();
}

/* ------------------------------------------------------------------ painters */

static void paint_card(HDC hdc, const RECT *rc) {
    fill_round(hdc, rc, 16, card_bg(), G.dark ? RGB(64, 64, 64) : RGB(229, 229, 229));
}

/* settings row: glyph + title + subtitle + optional bottom divider */
static void paint_row(HDC hdc, const RECT *rc, Glyph g, const wchar_t *title,
        const wchar_t *sub, bool divider) {
    draw_glyph(hdc, rc->left + 16, (rc->top + rc->bottom) / 2 - 10, g,
        G.dark ? RGB(173, 171, 164) : RGB(96, 94, 92));
    RECT tr = { rc->left + 48, rc->top + 8, rc->right - 120, rc->bottom - 8 };
    text_at(hdc, title, tr, G.f_ui, ink(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sr = tr;
    sr.top += 18;
    text_at(hdc, sub, sr, G.f_ui, ink2(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (divider)
        hline_at(hdc, rc->left, rc->right, rc->bottom - 1,
            G.dark ? RGB(62, 62, 62) : RGB(234, 234, 234));
}

static void paint_hero(HDC hdc, const RECT *rc) {
    /* accent circle + white power glyph */
    int cx = rc->left + 44, cy = (rc->top + rc->bottom) / 2, rad = 26;
    HBRUSH br = CreateSolidBrush(G.accent);
    HGDIOBJ ob = SelectObject(hdc, br);
    HPEN pen = CreatePen(PS_SOLID, 1, G.accent);
    HGDIOBJ op = SelectObject(hdc, pen);
    Ellipse(hdc, cx - rad, cy - rad, cx + rad, cy + rad);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
    HPEN wp = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
    op = SelectObject(hdc, wp);
    HGDIOBJ ob2 = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Arc(hdc, cx - 12, cy - 11, cx + 12, cy + 13, cx + 12, cy - 11, cx - 12, cy - 11);
    MoveToEx(hdc, cx, cy - 18, NULL);
    LineTo(hdc, cx, cy - 4);
    SelectObject(hdc, op);
    SelectObject(hdc, ob2);
    DeleteObject(wp);
    /* title + subtitle */
    wchar_t title[128], sub[128], dur[32];
    Runtime *r = &G.rt;
    if (r->active) {
        long long rem = rt_remaining(r);
        rt_describe(r, title, 128);
        fmt_dur(rem, dur, 32);
        StringCchPrintfW(sub, 128, L"Next run in %s", dur);
    } else {
        const Plan *p = &r->plan;
        if (p->mode == M_DAILY) {
            unsigned h = (unsigned)wcstoul(p->p1, NULL, 10);
            unsigned m = (unsigned)wcstoul(p->p2, NULL, 10);
            if (h > 23) h = 23;
            if (m > 59) m = 59;
            StringCchPrintfW(title, 128, L"%s daily", TASK_NAMES[p->task]);
            StringCchPrintfW(sub, 128, L"Runs every day at %02u:%02u", h, m);
        } else {
            rt_describe(r, title, 128);
            StringCchCopyW(sub, 128, L"No task scheduled");
        }
    }
    RECT tr = { rc->left + 88, rc->top + 14, rc->right - 150, rc->top + 44 };
    text_at(hdc, title, tr, G.f_mid, ink(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sr = { rc->left + 88, rc->top + 44, rc->right - 150, rc->top + 66 };
    text_at(hdc, sub, sr, G.f_ui, ink2(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* WinUI-style toggle: On/Off label + pill. Returns nothing; state in *on. */
static void paint_toggle(HDC hdc, const RECT *rc, bool on, bool pressed) {
    RECT lr = { rc->left, rc->top, rc->left + 30, rc->bottom };
    text_at(hdc, on ? L"On" : L"Off", lr, G.f_ui, on ? ink() : ink2(),
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    int px = rc->left + 32, pw = 40, ph = 20;
    int py = (rc->top + rc->bottom) / 2 - ph / 2;
    RECT pr = { px, py, px + pw, py + ph };
    if (on) {
        fill_round(hdc, &pr, 20, pressed ? G.accent : G.accent, G.accent);
    } else {
        COLORREF edge = G.dark ? RGB(153, 153, 153) : RGB(96, 94, 92);
        fill_round(hdc, &pr, 20, card_bg(), edge);
    }
    int kx = on ? (px + pw - 8 - 2) : (px + 2 + 8);
    int ky = py + ph / 2;
    COLORREF kc = on ? RGB(255, 255, 255) : (G.dark ? RGB(200, 200, 200) : RGB(96, 94, 92));
    HBRUSH br = CreateSolidBrush(kc);
    HPEN pen = CreatePen(PS_SOLID, 1, kc);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    Ellipse(hdc, kx - 8, ky - 8, kx + 8, ky + 8);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void paint_chev(HDC hdc, const RECT *rc, bool pressed) {
    wchar_t s[] = L"\x203A";
    RECT r = *rc;
    text_at(hdc, s, r, G.f_mid, pressed ? ink() : ink2(), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

/* ------------------------------------------------------------------ window proc */

static HFONT load_ui_font(int px, int weight, bool big) {
    HFONT f = make_font(big ? L"Segoe UI Variable Display" : L"Segoe UI Variable Text",
        L"Segoe UI", px, weight);
    (void)big;
    return f;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandleW(NULL);
        G.f_ui = make_font(L"Segoe UI Variable Text", L"Segoe UI", -12, FW_NORMAL);
        G.f_mid = make_font(L"Segoe UI Variable Text", L"Segoe UI", -17, FW_SEMIBOLD);
        G.f_title = make_font(L"Segoe UI Variable Display", L"Segoe UI", -27, FW_SEMIBOLD);
        G.f_big = make_font(L"Segoe UI Variable Display", L"Segoe UI", -29, FW_SEMIBOLD);
        DWORD btn = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
        DWORD chk = WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX;
        DWORD obtn = WS_CHILD | WS_VISIBLE | BS_OWNERDRAW;
        DWORD owner = WS_CHILD | WS_VISIBLE | SS_OWNERDRAW;
        DWORD edit_ro = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_NUMBER;
        DWORD edit_tx = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT;
        DWORD combo_s = WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS;
        DWORD list_s = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT;
        (void)list_s;

        /* cards (painted chrome) */
        G.card_hero = mk_child(L"STATIC", L"", owner, 16, 52, 536, 92, hwnd, ID_CARD_HERO, hi, G.f_ui);
        G.card_sched = mk_child(L"STATIC", L"", owner, 16, 156, 536, 168, hwnd, ID_CARD_SCHED, hi, G.f_ui);
        G.card_status = mk_child(L"STATIC", L"", owner, 16, 336, 536, 104, hwnd, ID_CARD_STATUS, hi, G.f_ui);
        G.card_opts = mk_child(L"STATIC", L"", owner, 16, 452, 536, 156, hwnd, ID_CARD_OPTS, hi, G.f_ui);
        /* hero row + start button */
        G.row_hero = mk_child(L"STATIC", L"", owner, 16, 52, 536, 92, hwnd, ID_ROW_HERO, hi, G.f_ui);
        G.btn_start = mk_child(L"BUTTON", L"Start task", btn | BS_DEFPUSHBUTTON, 404, 84, 116, 32, hwnd, ID_BTN_START, hi, G.f_ui);
        /* schedule rows + inputs */
        G.row_action = mk_child(L"STATIC", L"", owner, 16, 156, 536, 52, hwnd, ID_ROW_ACTION, hi, G.f_ui);
        G.task_combo = mk_child(L"COMBOBOX", L"", combo_s, 372, 168, 148, 160, hwnd, ID_TASK_COMBO, hi, G.f_ui);
        for (int i = 0; i < NTASKS; i++) combo_add(G.task_combo, TASK_NAMES[i]);
        G.row_sched = mk_child(L"STATIC", L"", owner, 16, 208, 536, 52, hwnd, ID_ROW_SCHED, hi, G.f_ui);
        G.mode_combo = mk_child(L"COMBOBOX", L"", combo_s, 372, 220, 148, 160, hwnd, ID_MODE_COMBO, hi, G.f_ui);
        for (int i = 0; i < NMODES; i++) combo_add(G.mode_combo, MODE_NAMES[i]);
        G.row_time = mk_child(L"STATIC", L"", owner, 16, 260, 536, 64, hwnd, ID_ROW_TIME, hi, G.f_ui);
        G.etime = mk_child(L"EDIT", L"23:00", edit_tx, 420, 272, 100, 24, hwnd, ID_ETIME, hi, G.f_ui);
        G.edate = mk_child(L"EDIT", L"2026-10-04", edit_tx, 308, 272, 104, 24, hwnd, ID_EDATE, hi, G.f_ui);
        G.etime2 = mk_child(L"EDIT", L"23:00", edit_tx, 420, 272, 100, 24, hwnd, ID_ETIME2, hi, G.f_ui);
        G.eh = mk_child(L"EDIT", L"1", edit_ro, 340, 272, 56, 24, hwnd, ID_EDITH, hi, G.f_ui);
        G.em = mk_child(L"EDIT", L"0", edit_ro, 404, 272, 56, 24, hwnd, ID_EDITM, hi, G.f_ui);
        G.es = mk_child(L"EDIT", L"0", edit_ro, 468, 272, 52, 24, hwnd, ID_EDITS, hi, G.f_ui);
        G.en = mk_child(L"EDIT", L"15", edit_ro, 440, 272, 80, 24, hwnd, ID_EDITN, hi, G.f_ui);
        /* status rows (big number + desc are painted live in ID_ROW_BIG) */
        G.row_big = mk_child(L"STATIC", L"", owner, 16, 336, 536, 58, hwnd, ID_ROW_BIG, hi, G.f_ui);
        G.prog = mk_child(L"msctls_progress32", NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            32, 410, 480, 16, hwnd, ID_PROG, hi, G.f_ui);
        SendMessageW(G.prog, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        /* options rows + toggles + chevron */
        G.row_remind = mk_child(L"STATIC", L"", owner | SS_NOTIFY, 16, 452, 536, 52, hwnd, ID_ROW_REMIND, hi, G.f_ui);
        G.tgl_remind = mk_child(L"BUTTON", L"", obtn, 448, 462, 72, 28, hwnd, ID_TGL_REMIND, hi, G.f_ui);
        G.row_startup = mk_child(L"STATIC", L"", owner | SS_NOTIFY, 16, 504, 536, 52, hwnd, ID_ROW_STARTUP, hi, G.f_ui);
        G.tgl_startup = mk_child(L"BUTTON", L"", obtn, 448, 514, 72, 28, hwnd, ID_TGL_STARTUP, hi, G.f_ui);
        G.row_powerlink = mk_child(L"STATIC", L"", owner | SS_NOTIFY, 16, 556, 536, 52, hwnd, ID_ROW_POWERLINK, hi, G.f_ui);
        G.chev_power = mk_child(L"BUTTON", L"", obtn, 484, 566, 36, 28, hwnd, ID_CHEV_POWER, hi, G.f_ui);

        /* commit; handlers may run from here */
        G.ready = true;

        /* restore last config */
        {
            Plan p;
            load_config(&p);
            G.rt.plan = p;
            int ti = (int)p.task;
            if (ti < 0 || ti >= NTASKS) ti = (int)T_SLEEP;
            combo_set(G.task_combo, ti);
            int mi = (int)p.mode;
            if (mi < 0 || mi >= NMODES) mi = 0;
            combo_set(G.mode_combo, mi);
            wchar_t ts[128];
            int yy;
            unsigned a, b;
            if (parse_ymd(p.p1, &yy, &a, &b)) {
                StringCchCopyW(ts, 128, p.p2);
            } else if (parse_hhmm(p.p1[0] || p.p2[0] ? ts : ts, &a, &b)) {
                /* placeholder, replaced below */
            }
            /* daily/et footprint: HH:MM */
            {
                wchar_t both[128];
                StringCchPrintfW(both, 128, L"%s:%s", p.p1, p.p2);
                if (parse_ymd(p.p1, &yy, &a, &b)) {
                    StringCchCopyW(ts, 128, p.p2);
                } else if (parse_hhmm(both, &a, &b)) {
                    StringCchPrintfW(ts, 128, L"%02u:%02u", a, b);
                } else {
                    StringCchPrintfW(ts, 128, L"%s:%s", p.p1, p.p2);
                }
            }
            set_text(G.etime, ts);
            set_text(G.edate, p.p1);
            set_text(G.etime2, p.p2);
            set_text(G.eh, p.p1);
            set_text(G.em, p.p2);
            set_text(G.es, p.p3);
            set_text(G.en, p.p1);
            G.remind_on = p.remind_min > 0;
            G.startup_on = get_startup();
        }

        refresh_inputs();
        update_views();
        SetTimer(hwnd, TIMER_ID, 1000, NULL);
        add_tray(hwnd);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(app_bg());
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT tr = { 16, 10, 420, 42 };
        text_at(hdc, L"PowerOff", tr, G.f_title, ink(), DT_LEFT | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        if (!G.ready) return 0;
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        HDC hdc = di->hDC;
        RECT rc = di->rcItem;
        int id = (int)di->CtlID;
        bool pressed = (di->itemState & ODS_SELECTED) != 0;
        (void)pressed;
        if (id == ID_CARD_HERO || id == ID_CARD_SCHED || id == ID_CARD_STATUS || id == ID_CARD_OPTS) {
            fill_round(hdc, &rc, 16, card_bg(),
                G.dark ? RGB(66, 66, 66) : RGB(229, 229, 229));
        } else if (id == ID_ROW_HERO) {
            paint_hero(hdc, &rc);
        } else if (id == ID_ROW_ACTION) {
            paint_row(hdc, &rc, GL_POWER, L"Action", L"What the PC should do", true);
        } else if (id == ID_ROW_SCHED) {
            paint_row(hdc, &rc, GL_CAL, L"Schedule", L"When it runs", true);
        } else if (id == ID_ROW_TIME) {
            paint_row(hdc, &rc, GL_CLOCK, L"Time", MODE_HINTS[G.rt.plan.mode], false);
        } else if (id == ID_ROW_BIG) {
            wchar_t big[32], desc[256];
            if (G.rt.active) {
                long long rem = rt_remaining(&G.rt);
                fmt_dur(rem, big, 32);
                rt_describe(&G.rt, desc, 256);
            } else {
                const Plan *p = &G.rt.plan;
                if (p->mode == M_DAILY) {
                    unsigned h = (unsigned)wcstoul(p->p1, NULL, 10);
                    unsigned m = (unsigned)wcstoul(p->p2, NULL, 10);
                    if (h > 23) h = 23;
                    if (m > 59) m = 59;
                    StringCchPrintfW(big, 32, L"%02u:%02u", h, m);
                    StringCchPrintfW(desc, 256, L"%s daily at %s", TASK_NAMES[p->task], big);
                } else if (p->mode == M_ONCE) {
                    StringCchCopyW(big, 32, p->p2);
                    StringCchPrintfW(desc, 256, L"%s once at %s %s",
                        TASK_NAMES[p->task], p->p1, p->p2);
                } else {
                    wcscpy_s(big, 32, L"--:--");
                    rt_describe(&G.rt, desc, 256);
                }
            }
            RECT br2 = { rc.left + 16, rc.top + 2, rc.right - 16, rc.top + 40 };
            text_at(hdc, big, br2, G.f_big, ink(), DT_LEFT | DT_SINGLELINE);
            RECT dr = { rc.left + 16, rc.top + 40, rc.right - 16, rc.top + 58 };
            text_at(hdc, desc, dr, G.f_ui, ink2(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (id == ID_ROW_REMIND) {
            paint_row(hdc, &rc, GL_BELL, L"Reminders", L"Warn 5 minutes before it runs", true);
        } else if (id == ID_ROW_STARTUP) {
            paint_row(hdc, &rc, GL_ARROW, L"Run at startup", L"Start minimized to the tray", true);
        } else if (id == ID_ROW_POWERLINK) {
            paint_row(hdc, &rc, GL_MON, L"Power options", L"Open Windows Settings", false);
        } else if (id == ID_TGL_REMIND) {
            paint_toggle(hdc, &rc, G.remind_on, pressed);
        } else if (id == ID_TGL_STARTUP) {
            paint_toggle(hdc, &rc, G.startup_on, pressed);
        } else if (id == ID_CHEV_POWER) {
            paint_chev(hdc, &rc, pressed);
        }
        return 0;
    }
    case WM_COMMAND: {
        if (!G.ready) return 0;
        int id = (int)(wp & 0xFFFF);
        int notif = (int)((wp >> 16) & 0xFFFF);
        switch (id) {
        case IDM_SHOW:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            break;
        case IDM_START:
            toggle_start();
            break;
        case IDM_NOW:
            do_task(T_SLEEP);
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        default:
            break;
        }
        if (id == ID_TASK_COMBO && notif == CBN_SELCHANGE) {
            int ti = combo_get(G.task_combo);
            if (ti >= 0 && ti < NTASKS) {
                G.rt.plan.task = (Task)ti;
                update_views();
            }
        }
        if (id == ID_MODE_COMBO && notif == CBN_SELCHANGE) {
            int mi = combo_get(G.mode_combo);
            if (mi >= 0 && mi < NMODES) {
                G.rt.plan.mode = (Mode)mi;
                refresh_inputs();
                update_views();
            }
        }
        if (id == ID_BTN_START && notif == BN_CLICKED) toggle_start();
        if ((id == ID_TGL_REMIND || id == ID_ROW_REMIND) && notif == BN_CLICKED) {
            G.remind_on = !G.remind_on;
            save_config_current();
            InvalidateRect(G.tgl_remind, NULL, FALSE);
        }
        if ((id == ID_TGL_STARTUP || id == ID_ROW_STARTUP) && notif == BN_CLICKED) {
            G.startup_on = !G.startup_on;
            set_startup(G.startup_on);
            save_config_current();
            InvalidateRect(G.tgl_startup, NULL, FALSE);
        }
        if ((id == ID_CHEV_POWER || id == ID_ROW_POWERLINK) && notif == BN_CLICKED) {
            ShellExecuteW(NULL, L"open", L"ms-settings:powersleep", NULL, NULL, SW_SHOW);
        }
        if ((id == ID_ETIME || id == ID_EDATE || id == ID_ETIME2 ||
                id == ID_EDITH || id == ID_EDITM || id == ID_EDITS || id == ID_EDITN)
                && notif == EN_CHANGE) {
            Plan p;
            read_plan(&p);
            G.rt.plan.p1[0] = L'\0';
            wcscpy_s(G.rt.plan.p1, 64, p.p1);
            wcscpy_s(G.rt.plan.p2, 64, p.p2);
            wcscpy_s(G.rt.plan.p3, 64, p.p3);
        }
        return 0;
    }
    case WM_TIMER:
        tick();
        return 0;
    case WM_TRAY:
        if ((wp & 0xFFFF) == WM_LBUTTONDBLCLK || (wp & 0xFFFF) == WM_RBUTTONUP)
            show_tray_menu(hwnd);
        return 0;
    case WM_SETTINGCHANGE:
        read_theme();
        apply_dark_mode(hwnd);
        update_views();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_CLOSE:
        if (G.rt.active) { /* keep running in the tray, like Wise */
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        del_tray(hwnd);
        KillTimer(hwnd, TIMER_ID);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* read current UI into *p (raw text; validated in rt_start) */
static void read_plan(Plan *p) {
    int ti = combo_get(G.task_combo);
    if (ti < 0) ti = 0;
    if (ti >= NTASKS) ti = NTASKS - 1;
    p->task = (Task)ti;
    p->mode = G.rt.plan.mode;
    wchar_t buf[128];
    switch (p->mode) {
    case M_DAILY:
        get_text(G.etime, buf, 128);
        {
            unsigned h, m;
            const wchar_t *s = buf;
            while (*s == L' ' || *s == L'\t') s++;
            if (parse_hhmm(s, &h, &m)) {
                StringCchPrintfW(p->p1, 64, L"%u", h);
                StringCchPrintfW(p->p2, 64, L"%u", m);
            } else {
                StringCchCopyW(p->p1, 64, s);
                p->p2[0] = L'\0';
            }
        }
        wcscpy_s(p->p3, 64, L"0");
        break;
    case M_ONCE:
        get_text(G.edate, p->p1, 64);
        get_text(G.etime2, p->p2, 64);
        p->p3[0] = L'\0';
        break;
    case M_COUNTDOWN:
        get_text(G.eh, p->p1, 64);
        get_text(G.em, p->p2, 64);
        get_text(G.es, p->p3, 64);
        break;
    case M_IDLE:
    case M_INTERVAL:
        get_text(G.en, p->p1, 64);
        p->p2[0] = L'\0';
        p->p3[0] = L'\0';
        break;
    }
    p->remind_min = G.remind_on ? 5 : 0;
}

static void refresh_inputs(void) {
    Mode m = G.rt.plan.mode;
    show_ctl(G.etime, m == M_DAILY);
    show_ctl(G.edate, m == M_ONCE);
    show_ctl(G.etime2, m == M_ONCE);
    bool cd = (m == M_COUNTDOWN);
    show_ctl(G.eh, cd);
    show_ctl(G.em, cd);
    show_ctl(G.es, cd);
    show_ctl(G.en, m == M_IDLE || m == M_INTERVAL);
}

static void save_config_current(void) {
    Plan p;
    read_plan(&p);
    save_config(&p);
}

/* ------------------------------------------------------------------ run */

static void run_gui(bool tray) {
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    HINSTANCE hi = GetModuleHandleW(NULL);
    wchar_t cls[] = L"PowerOffWnd";
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    read_theme();
    read_accent();
    memset(&G.rt, 0, sizeof(G.rt));
    {
        Plan p;
        load_config(&p);
        G.rt.plan = p;
    }

    HWND hwnd = CreateWindowExW(0, cls, L"PowerOff \x2014 tiny auto shutdown",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 584, 668,
        NULL, NULL, hi, NULL);
    if (!hwnd) return;
    apply_dark_mode(hwnd);
    ShowWindow(hwnd, tray ? SW_HIDE : SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* ------------------------------------------------------------------ headless */

static void run_headless(Plan *plan) {
    Runtime r;
    memset(&r, 0, sizeof(r));
    r.plan = *plan;
    {
        wchar_t desc[256];
        const wchar_t *err = rt_start(&r, desc, 256);
        if (err) {
            fwprintf(stderr, L"PowerOff: %s\n", err);
            exit(2);
        }
        wprintf(L"PowerOff: %s\n", desc);
    }
    for (;;) {
        rt_apply_signals(&r);
        if (!r.active) {
            wprintf(L"PowerOff: cancelled.\n");
            break;
        }
        long long rem = rt_remaining(&r);
        if (rem <= 0) {
            if (do_task(r.plan.task)) {
                wprintf(L"PowerOff: executed '%s'.\n", TASK_NAMES[r.plan.task]);
            } else {
                fwprintf(stderr, L"PowerOff: failed to execute '%s'.\n", TASK_NAMES[r.plan.task]);
                exit(3);
            }
            if (r.plan.mode == M_INTERVAL || r.plan.mode == M_DAILY) {
                r.reminded = false;
                if (r.plan.mode == M_INTERVAL) {
                    long long mins = wcstoll(r.plan.p1, NULL, 10);
                    if (mins < 1) mins = 60;
                    r.fire_at = local_now_secs() + mins * 60;
                    r.span = mins * 60;
                }
                Sleep(61000);
                continue;
            }
            if (r.plan.mode == M_IDLE &&
                    (r.plan.task == T_SLEEP || r.plan.task == T_HIBERNATE || r.plan.task == T_LOCK)) {
                r.reminded = false;
                Sleep(61000);
                continue;
            }
            break;
        }
        if (r.plan.remind_min > 0 && !r.reminded &&
                rem <= (long long)r.plan.remind_min * 60 && rem > 5) {
            wchar_t dur[32];
            fmt_dur(rem, dur, 32);
            wprintf(L"PowerOff: '%s' in %s \x2026\n", TASK_NAMES[r.plan.task], dur);
            r.reminded = true;
        }
        Sleep(1000);
    }
}

static void warn_then_fire(Task task, unsigned warn_secs) {
    if (warn_secs > 0) {
        wchar_t msg[256];
        StringCchPrintfW(msg, 256, L"%s in %u seconds.\n\nPress Cancel to abort.",
            TASK_NAMES[task], warn_secs);
        if (MessageBoxW(NULL, msg, L"PowerOff", MB_OKCANCEL | MB_ICONWARNING | MB_SYSTEMMODAL) == IDCANCEL) {
            wprintf(L"PowerOff: aborted by user.\n");
            return;
        }
    }
    if (do_task(task)) {
        wprintf(L"PowerOff: executed '%s'.\n", TASK_NAMES[task]);
    } else {
        fwprintf(stderr, L"PowerOff: failed to execute '%s'.\n", TASK_NAMES[task]);
        exit(3);
    }
}

/* ------------------------------------------------------------------ CLI */

static void print_help(void) {
    wprintf(
        L"PowerOff 1.0 \x2014 tiny Wise Auto Shutdown clone (native Win32, C)\n"
        L"\n"
        L"GUI (no args, defaults to Sleep / Daily 23:00):\n"
        L"  PowerOff.exe [--tray] [--dark|--light]\n"
        L"\n"
        L"Headless scheduler (runs in console until the task fires):\n"
        L"  PowerOff.exe --task sleep --daily 23:00\n"
        L"  PowerOff.exe --task shutdown --once \"2026-10-05 23:00\"\n"
        L"  PowerOff.exe --task sleep --in 3600 | --countdown 1:00:00\n"
        L"  PowerOff.exe --task lock --idle 15 | --task sleep --every 60\n"
        L"\n"
        L"Fire immediately (for Task Scheduler / scripts):\n"
        L"  PowerOff.exe --task sleep --fire-now [--warn-secs 60]\n"
        L"\n"
        L"Daily 11 PM sleep that survives reboot (recommended):\n"
        L"  PowerOff.exe --install-daily 23:00 --task sleep\n"
        L"  PowerOff.exe --uninstall\n"
        L"\n"
        L"Tasks: shutdown restart poweroff logoff lock sleep hibernate\n");
}

static bool task_from_arg(const wchar_t *s, Task *out);

static const wchar_t *arg_val(int argc, wchar_t **argv, const wchar_t *flag) {
    for (int i = 1; i + 1 < argc; i++)
        if (wcscmp(argv[i], flag) == 0)
            return argv[i + 1];
    return NULL;
}

static bool has_flag(int argc, wchar_t **argv, const wchar_t *flag) {
    for (int i = 1; i < argc; i++)
        if (wcscmp(argv[i], flag) == 0)
            return true;
    return false;
}

static bool task_from_arg(const wchar_t *s, Task *out) {
    wchar_t low[32];
    size_t k = 0;
    for (; s[k] && k < 31; k++) {
        wchar_t c = s[k];
        if (c >= L'A' && c <= L'Z') c += 32;
        if (c == L'_' || c == L'-') continue;
        low[k] = c;
    }
    /* NOTE: keeps '_'/'-' skipping simple (see below) */
    low[k] = L'\0';
    /* rebuild without separators properly */
    {
        wchar_t clean[32];
        size_t j = 0;
        for (size_t i = 0; s[i] && j < 31; i++) {
            wchar_t c = s[i];
            if (c >= L'A' && c <= L'Z') c += 32;
            if (c == L'_' || c == L'-') continue;
            clean[j++] = c;
        }
        clean[j] = L'\0';
        if (!wcscmp(clean, L"shutdown")) *out = T_SHUTDOWN;
        else if (!wcscmp(clean, L"restart") || !wcscmp(clean, L"reboot")) *out = T_RESTART;
        else if (!wcscmp(clean, L"poweroff")) *out = T_POWEROFF;
        else if (!wcscmp(clean, L"logoff") || !wcscmp(clean, L"logout")) *out = T_LOGOFF;
        else if (!wcscmp(clean, L"lock")) *out = T_LOCK;
        else if (!wcscmp(clean, L"sleep") || !wcscmp(clean, L"suspend")) *out = T_SLEEP;
        else if (!wcscmp(clean, L"hibernate")) *out = T_HIBERNATE;
        else return false;
        (void)low;
        return true;
    }
}

int wmain(int argc, wchar_t **argv) {
    /* Unicode console output (em-dashes etc. would otherwise fail wprintf) */
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
    if (has_flag(argc, argv, L"--help") || has_flag(argc, argv, L"-h") || has_flag(argc, argv, L"/?")) {
        print_help();
        return 0;
    }
    if (has_flag(argc, argv, L"--uninstall")) {
        wchar_t err[256];
        if (run_schtasks(L"/delete /tn \"PowerOff daily\" /f", err, 256)) {
            wprintf(L"PowerOff: Removed 'PowerOff daily' scheduled task.\n");
            return 0;
        }
        fwprintf(stderr, L"PowerOff: %s\n", err);
        return 1;
    }
    {
        const wchar_t *st = arg_val(argc, argv, L"--install-daily");
        if (st) {
            Task task = T_SLEEP;
            const wchar_t *tn = arg_val(argc, argv, L"--task");
            if (tn && !task_from_arg(tn, &task)) {
                fwprintf(stderr, L"PowerOff: unknown task '%s'.\n", tn);
                return 2;
            }
            unsigned h, m;
            if (!parse_hhmm(st, &h, &m)) {
                fwprintf(stderr, L"PowerOff: use HH:MM, e.g. 23:00\n");
                return 2;
            }
            wchar_t exe[MAX_PATH], tr[1024], args[1536], err[256], hhmm[16];
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            const wchar_t *names[] = { L"shutdown", L"restart", L"poweroff", L"logoff", L"lock", L"sleep", L"hibernate" };
            StringCchPrintfW(tr, 1024, L"\"%s\" --task %s --fire-now --warn-secs 60",
                exe, names[task]);
            StringCchPrintfW(hhmm, 16, L"%02u:%02u", h, m);
            StringCchPrintfW(args, 1536,
                L"/create /tn \"PowerOff daily\" /sc daily /st %s /f /tr \"%s\"", hhmm, tr);
            if (run_schtasks(args, err, 256)) {
                wprintf(L"PowerOff: Scheduled '%s' daily at %s (Task Scheduler: 'PowerOff daily').\n",
                    TASK_NAMES[task], hhmm);
                return 0;
            }
            fwprintf(stderr, L"PowerOff: %s\n", err);
            return 1;
        }
    }

    const wchar_t *task_raw = arg_val(argc, argv, L"--task");
    Task task_opt = T_SLEEP;
    bool have_task = false;
    if (task_raw) {
        if (!task_from_arg(task_raw, &task_opt)) {
            fwprintf(stderr, L"PowerOff: unknown task '%s'. Tasks: shutdown restart poweroff logoff lock sleep hibernate\n", task_raw);
            return 2;
        }
        have_task = true;
    }
    if (has_flag(argc, argv, L"--fire-now")) {
        if (!have_task) {
            fwprintf(stderr, L"PowerOff: --fire-now needs --task <name>\n");
            return 2;
        }
        const wchar_t *w = arg_val(argc, argv, L"--warn-secs");
        warn_then_fire(task_opt, w ? (unsigned)wcstoul(w, NULL, 10) : 0);
        return 0;
    }

    /* timing flags */
    Mode mode = M_DAILY;
    const wchar_t *raw = NULL;
    const wchar_t *v;
    if ((v = arg_val(argc, argv, L"--daily")) != NULL) { mode = M_DAILY; raw = v; }
    else if ((v = arg_val(argc, argv, L"--once")) != NULL) { mode = M_ONCE; raw = v; }
    else if ((v = arg_val(argc, argv, L"--countdown")) != NULL) { mode = M_COUNTDOWN; raw = v; }
    else if ((v = arg_val(argc, argv, L"--in")) != NULL) { mode = M_COUNTDOWN; raw = v; }
    else if ((v = arg_val(argc, argv, L"--idle")) != NULL) { mode = M_IDLE; raw = v; }
    else if ((v = arg_val(argc, argv, L"--every")) != NULL) { mode = M_INTERVAL; raw = v; }
    if (have_task && raw) {
        Plan plan;
        memset(&plan, 0, sizeof(plan));
        plan.task = task_opt;
        plan.mode = mode;
        plan.remind_min = 1;
        switch (mode) {
        case M_DAILY: {
            unsigned h, m;
            if (!parse_hhmm(raw, &h, &m)) {
                fwprintf(stderr, L"PowerOff: --daily needs HH:MM\n");
                return 2;
            }
            StringCchPrintfW(plan.p1, 64, L"%u", h);
            StringCchPrintfW(plan.p2, 64, L"%u", m);
            StringCchCopyW(plan.p3, 64, L"0");
            break;
        }
        case M_ONCE: {
            const wchar_t *sp = wcschr(raw, L' ');
            if (!sp) {
                fwprintf(stderr, L"PowerOff: --once needs \"YYYY-MM-DD HH:MM\"\n");
                return 2;
            }
            StringCchCopyNW(plan.p1, 64, raw, sp - raw);
            StringCchCopyW(plan.p2, 64, sp + 1);
            plan.p3[0] = L'\0';
            break;
        }
        case M_COUNTDOWN: {
            long long h = 0, m = 0, s = -1;
            unsigned ph, pm;
            if (parse_hhmm(raw, &ph, &pm)) {
                m = ph; s = pm;
            } else if (wcschr(raw, L':')) {
                if (swscanf_s(raw, L"%lld:%lld:%lld", &h, &m, &s) != 3) {
                    fwprintf(stderr, L"PowerOff: --countdown needs seconds or H:M:S\n");
                    return 2;
                }
            } else {
                s = _wcstoi64(raw, NULL, 10);
                if (s <= 0 && raw[0] != L'0') {
                    /* _wcstoi64 yields 0 on garbage; treat unparseable as bad */
                    const wchar_t *p2 = raw;
                    bool alld = true;
                    for (; *p2; p2++)
                        if (*p2 < L'0' || *p2 > L'9') { alld = false; break; }
                    if (!alld || raw[0] == L'\0') {
                        fwprintf(stderr, L"PowerOff: bad countdown '%s'\n", raw);
                        return 2;
                    }
                }
            }
            if (h < 0 || m < 0 || (s <= 0 && h == 0 && m == 0)) {
                fwprintf(stderr, L"PowerOff: bad countdown '%s'\n", raw);
                return 2;
            }
            StringCchPrintfW(plan.p1, 64, L"%lld", h);
            StringCchPrintfW(plan.p2, 64, L"%lld", m);
            StringCchPrintfW(plan.p3, 64, L"%lld", s);
            plan.remind_min = 0;
            break;
        }
        case M_IDLE:
        case M_INTERVAL:
            StringCchCopyW(plan.p1, 64, raw);
            plan.p2[0] = L'\0';
            plan.p3[0] = L'\0';
            plan.remind_min = 0;
            break;
        }
        run_headless(&plan);
        return 0;
    }
    if (have_task) {
        fwprintf(stderr, L"PowerOff: --task needs a timing flag (see --help).\n");
        return 2;
    }

    /* GUI path */
    {
        HWND c = GetConsoleWindow();
        if (c) ShowWindow(c, SW_HIDE);
    }
    read_theme();
    if (has_flag(argc, argv, L"--dark")) G.dark = true;
    if (has_flag(argc, argv, L"--light")) G.dark = false;
    read_accent();
    run_gui(has_flag(argc, argv, L"--tray") || has_flag(argc, argv, L"-tray"));
    return 0;
}
