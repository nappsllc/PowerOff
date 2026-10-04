/* PowerOff — tiny auto shutdown scheduler (C version).
 *
 * UI copies the "PowerOff scheduling utility" design artifact exactly:
 * Settings-style page (title "Schedule"), hero card with gradient-style
 * action icon, Task rows (Action / Schedule / Time with segmented 12-hour
 * picker and day-of-week buttons), Options toggles (Remind / Force apps /
 * Start with Windows), Related settings cards. Fluent light/dark tokens,
 * system accent, Segoe UI Variable. All interactive elements are real
 * native controls (edits, buttons) over hand-painted chrome — Win32 has no
 * cards/toggles/flyouts to reuse, and WinUI can't be hosted from plain C.
 *
 * Modes (GUI): At a specific time (daily repeat), After a countdown
 * (presets), On a weekly schedule (day picker), When the PC is idle
 * (presets). CLI additionally keeps --once and --every.
 *
 * One file. Build with build.bat (MSVC).
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

typedef enum { T_SHUTDOWN, T_RESTART, T_SLEEP, T_HIBERNATE, T_LOGOFF, T_LOCK, T_DISPLAY, NTASKS } Task;
typedef enum { M_DAILY, M_ONCE, M_COUNTDOWN, M_IDLE, M_INTERVAL, M_WEEKLY, NMODES_ALL } Mode;

static const wchar_t *TASK_NAMES[] = {
    L"Shut down", L"Restart", L"Sleep", L"Hibernate",
    L"Sign out", L"Lock", L"Turn off display",
};
/* CLI spellings, index-aligned with TASK_NAMES */
static const wchar_t *TASK_CLI[] = {
    L"shutdown", L"restart", L"sleep", L"hibernate", L"logoff", L"lock", L"display",
};

/* GUI combo lists (artifact set) */
static const int GUI_MODES[] = { M_DAILY, M_COUNTDOWN, M_WEEKLY, M_IDLE };
static const wchar_t *GUI_MODE_NAMES[] = {
    L"At a specific time", L"After a countdown", L"On a weekly schedule", L"When the PC is idle",
};
#define NGUI_MODES 4

static const int CDS[] = { 5, 10, 15, 30, 45, 60, 90, 120, 180, 240 };
#define NCDS 10
static const int IDLES[] = { 5, 10, 15, 30, 60 };
#define NIDLES 5
static const wchar_t *DAYS[] = { L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun" };

typedef struct {
    Task task;
    Mode mode;
    int h, m;          /* daily/weekly target time, 24h */
    int cd;            /* countdown minutes (preset) */
    int idle;          /* idle minutes (preset) */
    int days;          /* weekly: bitmask Mon=1..Sun=64 */
    int remind_min;    /* 0 = off */
    bool force;        /* EWX_FORCE for logoff-style tasks */
    /* CLI-only raw fields */
    wchar_t p1[64], p2[64], p3[64];
} Plan;

typedef struct {
    Plan plan;
    bool active;
    bool reminded;
    long long fire_at;
    long long span;
    long long delay;
} Runtime;

static volatile LONG g_fire_now = 0;
static volatile LONG g_delay_secs = 0;
static volatile LONG g_cancel_req = 0;

/* ------------------------------------------------------------------ icons */

typedef enum {
    IC_POWER, IC_RESTART, IC_MOON, IC_SIGNOUT, IC_LOCK, IC_MONITOR,
    IC_ALERT, IC_APPS, IC_ROCKET, IC_CALCLK, IC_CLOCK, IC_BATT, IC_INFO,
} IconId;

static IconId TASK_ICON[] = {
    IC_POWER, IC_RESTART, IC_MOON, IC_MOON, IC_SIGNOUT, IC_LOCK, IC_MONITOR,
};
/* single-color approximations of the artifact's gradients */
static COLORREF TASK_COLOR[] = {
    0x005A48E0, /* shutdown  (BBGGRR: #E0485A) */
    0x00C46025, /* restart   (#2560C4) */
    0x00D47F7B, /* sleep     (#7B7FD4) */
    0x00B86F6E, /* hibernate (#6E6FB8) */
    0x001F6AC4, /* sign out  (#C46A1F) */
    0x00988F8A, /* lock      (#8A8F98) */
    0x009A7D2D, /* display   (#2D7D9A) */
};
#define C_ALERT   0x003080E0  /* #E08030 */
#define C_APPS    0x00968880  /* #808896 */
#define C_ROCKET  0x00D05F7A  /* #7A5FD0 */
#define C_CALCLK  0x00D48F4C  /* #4C8FD4 */
#define C_CLOCK   0x008A9A2D  /* #2D9A8A */
#define C_BATT    0x00639E5F  /* #5F9E63 */
#define C_INFO    0x00CD8A10  /* #108ACD */

/* ------------------------------------------------------------------ globals */

typedef struct {
    HWND main;
    /* interactive children */
    HWND combo_action, combo_mode, combo_cd, combo_idle;
    HWND eh, em, apm;                 /* segmented time picker */
    HWND day[7];
    HWND tray;                        /* tray icon owner, exists only while the icon is shown */
    HWND tg_remind, tg_force, tg_auto, tg_tray;
    HWND btn_start, rel_power, rel_about;
    /* fonts */
    HFONT f_h1, f_hero, f_row, f_sub, f_seg;
    /* cached brushes for WM_CTLCOLOR* */
    HBRUSH br_ctl;
    /* layout rects (client coords) */
    RECT rc_hero, rc_task_hdr, rc_action, rc_sched, rc_time, rc_days;
    RECT rc_opts_hdr, rc_remind, rc_force, rc_auto, rc_tray;
    RECT rc_rel_hdr, rc_rel1, rc_rel2;
    RECT rc_time_ctl;   /* right-side control slot in the Time row */
    int client_h;
    /* state */
    bool ready, dark;
    DWORD accent;
    Runtime rt;
    bool remind_on, force_on, auto_on;
    bool tray_on;     /* keep a tray icon after the window closes; off = process exits */
    bool delegated;   /* active task handed to Task Scheduler; this process only displays it */
} Globals;
static Globals G;

enum {
    ID_COMBO_ACTION = 200, ID_COMBO_MODE, ID_COMBO_CD, ID_COMBO_IDLE,
    ID_EH = 210, ID_EM, ID_APM,
    ID_DAY0 = 220, /* ..226 */
    ID_TG_REMIND = 230, ID_TG_FORCE, ID_TG_AUTO, ID_TG_TRAY,
    ID_START = 240, ID_REL_POWER = 250, ID_REL_ABOUT,
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

static void civil_from_days(long long z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned y_ = yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp < 10 ? mp + 3 : mp - 9;
    if (mm <= 2) y_++;
    *y = (int)y_; *m = mm; *d = dd;
}

static long long local_now_secs(void) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return days_from_civil(st.wYear, st.wMonth, st.wDay) * 86400LL
        + st.wHour * 3600LL + st.wMinute * 60LL + st.wSecond;
}

static void civil_today(int *y, unsigned *mo, unsigned *d) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    *y = st.wYear; *mo = st.wMonth; *d = st.wDay;
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

static void fmt12(int h, int m, wchar_t *out, size_t n) {
    int h12 = h % 12;
    if (h12 == 0) h12 = 12;
    StringCchPrintfW(out, n, L"%d:%02d %ls", h12, m, h < 12 ? L"AM" : L"PM");
}

static void dur_label(int mins, wchar_t *out, size_t n) {
    if (mins < 60) StringCchPrintfW(out, n, L"%d minutes", mins);
    else if (mins == 60) StringCchCopyW(out, n, L"1 hour");
    else if (mins % 60 == 0) StringCchPrintfW(out, n, L"%d hours", mins / 60);
    else StringCchPrintfW(out, n, L"%d h %d min", mins / 60, mins % 60);
}

static void days_text(int days, wchar_t *out, size_t n) {
    if (days == 0x7F) { StringCchCopyW(out, n, L"every day"); return; }
    if (days == 0x1F) { StringCchCopyW(out, n, L"weekdays"); return; }
    if (days == 0x60) { StringCchCopyW(out, n, L"weekends"); return; }
    wchar_t buf[128]; buf[0] = L'\0';
    for (int i = 0; i < 7; i++) {
        if (days & (1 << i)) {
            if (buf[0]) StringCchCatW(buf, 128, L", ");
            StringCchCatW(buf, 128, DAYS[i]);
        }
    }
    StringCchPrintfW(out, n, L"%s", buf[0] ? buf : L"no days");
}

/* "Today, 11:30 PM" for a local-epoch target */
static void day_label(long long target, wchar_t *out, size_t n) {
    int y; unsigned mo, d;
    civil_from_days(target / 86400, &y, &mo, &d);
    int ty; unsigned tmo, td;
    civil_today(&ty, &tmo, &td);
    long long diff = days_from_civil(y, mo, d) - days_from_civil(ty, tmo, td);
    long long sod = target % 86400;
    int h = (int)(sod / 3600), m = (int)((sod % 3600) / 60);
    wchar_t t[32];
    fmt12(h, m, t, 32);
    const wchar_t *day;
    if (diff == 0) day = L"Today";
    else if (diff == 1) day = L"Tomorrow";
    else {
        /* weekday from civil: 1970-01-01 = Thursday (4) */
        int wd = (int)((target / 86400 + 3) % 7); /* 0=Mon */
        static const wchar_t *FULL[] = { L"Monday", L"Tuesday", L"Wednesday",
            L"Thursday", L"Friday", L"Saturday", L"Sunday" };
        day = FULL[wd];
    }
    StringCchPrintfW(out, n, L"%ls, %ls", day, t);
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

static bool do_task_ex(Task t, bool force) {
    DWORD extra = force ? EWX_FORCE : EWX_FORCEIFHUNG;
    switch (t) {
    case T_LOCK: return LockWorkStation() != 0;
    case T_SLEEP: return SetSuspendState(FALSE, TRUE, FALSE) != 0;
    case T_HIBERNATE: return SetSuspendState(TRUE, TRUE, FALSE) != 0;
    case T_DISPLAY:
        SendMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
        return true;
    case T_LOGOFF: enable_shutdown_privilege(); return ExitWindowsEx(EWX_LOGOFF | extra, 0) != 0;
    case T_SHUTDOWN: enable_shutdown_privilege(); return ExitWindowsEx(EWX_SHUTDOWN | extra, 0) != 0;
    case T_RESTART: enable_shutdown_privilege(); return ExitWindowsEx(EWX_REBOOT | extra, 0) != 0;
    }
    return false;
}

static bool do_task(Task t) { return do_task_ex(t, G.rt.plan.force); }

/* ------------------------------------------------------------------ scheduler */

static void rt_describe(const Runtime *r, wchar_t *out, size_t n) {
    const wchar_t *tn = TASK_NAMES[r->plan.task];
    wchar_t t[32];
    wchar_t d[96];
    switch (r->plan.mode) {
    case M_DAILY:
        fmt12(r->plan.h, r->plan.m, t, 32);
        StringCchPrintfW(out, n, L"%ls at %ls", tn, t);
        break;
    case M_WEEKLY:
        fmt12(r->plan.h, r->plan.m, t, 32);
        days_text(r->plan.days, d, 96);
        StringCchPrintfW(out, n, L"%ls at %ls on %ls", tn, t, d);
        break;
    case M_ONCE:
        StringCchPrintfW(out, n, L"%ls once at %ls %ls", tn, r->plan.p1, r->plan.p2);
        break;
    case M_COUNTDOWN:
        dur_label(r->plan.cd, d, 96);
        StringCchPrintfW(out, n, L"%ls in %ls", tn, d);
        break;
    case M_IDLE:
        dur_label(r->plan.idle, d, 96);
        StringCchPrintfW(out, n, L"%ls after %ls idle", tn, d);
        break;
    case M_INTERVAL:
        StringCchPrintfW(out, n, L"%ls every %ls min", tn, r->plan.p1);
        break;
    }
}

/* next occurrence for weekly/daily, local epoch seconds */
static long long next_at(int h, int m, int days) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    long long today = days_from_civil(st.wYear, st.wMonth, st.wDay);
    long long now = local_now_secs();
    for (int i = 0; i < 8; i++) {
        long long d = today + i;
        if (days & (1 << ((int)((d + 3) % 7)))) { /* day-of-week, 0=Mon */
            long long at = d * 86400 + h * 3600 + m * 60;
            if (at > now) return at;
        }
    }
    return 0;
}

static const wchar_t *rt_start(Runtime *r, wchar_t *desc, size_t n) {
    long long now = local_now_secs();
    r->reminded = false;
    r->delay = 0;
    switch (r->plan.mode) {
    case M_DAILY:
        if (r->plan.h < 0 || r->plan.h > 23 || r->plan.m < 0 || r->plan.m > 59)
            return L"Hour must be 0-23 and minute 0-59";
        r->fire_at = 0;
        r->span = 86400;
        break;
    case M_WEEKLY:
        if (r->plan.h < 0 || r->plan.h > 23 || r->plan.m < 0 || r->plan.m > 59)
            return L"Hour must be 0-23 and minute 0-59";
        if (!r->plan.days) return L"Select at least one day of the week";
        r->fire_at = 0; /* computed live */
        r->span = 7 * 86400;
        break;
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
    case M_COUNTDOWN:
        if (r->plan.cd <= 0) return L"Choose a countdown duration";
        r->fire_at = now + (long long)r->plan.cd * 60;
        r->span = r->plan.cd * 60;
        break;
    case M_IDLE:
        if (r->plan.idle <= 0) return L"Choose an idle duration";
        r->fire_at = 0;
        r->span = r->plan.idle * 60;
        break;
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
    case M_DAILY:
        return next_at(r->plan.h, r->plan.m, 0x7F) - now;
    case M_WEEKLY:
        return next_at(r->plan.h, r->plan.m, r->plan.days) - now;
    case M_IDLE: {
        long long mins = r->plan.idle > 0 ? r->plan.idle : 15;
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

static void rt_apply_signals(Runtime *r) {
    if (InterlockedExchange(&g_cancel_req, 0)) r->active = false;
    LONG d = InterlockedExchange(&g_delay_secs, 0);
    if (d != 0) {
        if (r->plan.mode == M_DAILY || r->plan.mode == M_WEEKLY) {
            long long rem = rt_remaining(r) + d;
            r->plan.mode = M_COUNTDOWN;
            r->plan.cd = (int)(rem / 60 + 1);
            r->fire_at = local_now_secs() + (long long)r->plan.cd * 60;
            r->span = r->plan.cd * 60;
            r->delay = 0;
        } else {
            r->delay += d;
        }
        r->reminded = true;
    }
    if (InterlockedExchange(&g_fire_now, 0)) {
        r->fire_at = local_now_secs();
        r->delay = 0;
        if (r->plan.mode == M_DAILY || r->plan.mode == M_WEEKLY || r->plan.mode == M_IDLE) {
            r->plan.mode = M_COUNTDOWN;
            r->plan.cd = 0;
            r->fire_at = local_now_secs() - 1;
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
    wchar_t v[80];
    StringCchPrintfW(v, 80, L"%d", (int)p->task);
    WritePrivateProfileStringW(L"PowerOff", L"task", v, path);
    StringCchPrintfW(v, 80, L"%d", (int)p->mode);
    WritePrivateProfileStringW(L"PowerOff", L"mode", v, path);
    StringCchPrintfW(v, 80, L"%d", p->h);
    WritePrivateProfileStringW(L"PowerOff", L"h", v, path);
    StringCchPrintfW(v, 80, L"%d", p->m);
    WritePrivateProfileStringW(L"PowerOff", L"m", v, path);
    StringCchPrintfW(v, 80, L"%d", p->cd);
    WritePrivateProfileStringW(L"PowerOff", L"cd", v, path);
    StringCchPrintfW(v, 80, L"%d", p->idle);
    WritePrivateProfileStringW(L"PowerOff", L"idle", v, path);
    StringCchPrintfW(v, 80, L"%d", p->days);
    WritePrivateProfileStringW(L"PowerOff", L"days", v, path);
    StringCchPrintfW(v, 80, L"%d", p->remind_min);
    WritePrivateProfileStringW(L"PowerOff", L"remind", v, path);
    StringCchPrintfW(v, 80, L"%d", p->force ? 1 : 0);
    WritePrivateProfileStringW(L"PowerOff", L"force", v, path);
}

static void load_config(Plan *p) {
    wchar_t path[MAX_PATH];
    p->task = T_SLEEP; p->mode = M_DAILY;
    p->h = 23; p->m = 0; p->cd = 60; p->idle = 15; p->days = 0x7F;
    p->remind_min = 5; p->force = false;
    p->p1[0] = p->p2[0] = p->p3[0] = L'\0';
    ini_path(path, MAX_PATH);
    /* legacy file (pre-1.0 UI): task enum order differed and time lived in
     * p1/p2 — detect and translate instead of misreading it */
    if (GetPrivateProfileIntW(L"PowerOff", L"h", -1, path) == -1) {
        wchar_t p1[64], p2[64];
        GetPrivateProfileStringW(L"PowerOff", L"p1", L"", p1, 64, path);
        if (p1[0]) {
            static const Task LEG[] = { T_SHUTDOWN, T_RESTART, T_SHUTDOWN,
                T_LOGOFF, T_LOCK, T_SLEEP, T_HIBERNATE };
            int t = GetPrivateProfileIntW(L"PowerOff", L"task", 5, path);
            if (t >= 0 && t < 7) p->task = LEG[t];
            GetPrivateProfileStringW(L"PowerOff", L"p2", L"0", p2, 64, path);
            p->h = _wtoi(p1);
            p->m = _wtoi(p2);
            if (p->h < 0 || p->h > 23) p->h = 23;
            if (p->m < 0 || p->m > 59) p->m = 0;
            p->remind_min = GetPrivateProfileIntW(L"PowerOff", L"remind", 5, path);
            return;
        }
    }
    int t = GetPrivateProfileIntW(L"PowerOff", L"task", T_SLEEP, path);
    int m = GetPrivateProfileIntW(L"PowerOff", L"mode", M_DAILY, path);
    if (t >= 0 && t < NTASKS) p->task = (Task)t;
    if (m >= 0 && m < NMODES_ALL) p->mode = (Mode)m;
    p->h = GetPrivateProfileIntW(L"PowerOff", L"h", 23, path);
    p->m = GetPrivateProfileIntW(L"PowerOff", L"m", 0, path);
    p->cd = GetPrivateProfileIntW(L"PowerOff", L"cd", 60, path);
    p->idle = GetPrivateProfileIntW(L"PowerOff", L"idle", 15, path);
    p->days = GetPrivateProfileIntW(L"PowerOff", L"days", 0x7F, path) & 0x7F;
    p->remind_min = GetPrivateProfileIntW(L"PowerOff", L"remind", 5, path);
    p->force = GetPrivateProfileIntW(L"PowerOff", L"force", 0, path) != 0;
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

/* ------------------------------------------------------------------ Task Scheduler hand-off
 * Closing the window with an active task registers it as the "PowerOff" scheduled task,
 * so Windows fires it (even after this process exits or the PC reboots) and the tray
 * process only displays state. The task starts remind_min early and runs
 * "--fire-now --warn-secs N", which shows the reminder and fires when it times out.
 * Idle mode stays in-process: Task Scheduler's idle trigger can't do custom minutes. */

#define WTS_NAME L"PowerOff"

static void iso_local(long long secs, wchar_t *out, size_t n) {
    int y; unsigned mo, d;
    long long s = secs % 86400;
    civil_from_days(secs / 86400, &y, &mo, &d);
    StringCchPrintfW(out, n, L"%04d-%02u-%02uT%02lld:%02lld:%02lld",
        y, mo, d, s / 3600, s / 60 % 60, s % 60);
}

static void xml_escape(const wchar_t *s, wchar_t *out, size_t n) {
    out[0] = L'\0';
    for (; *s; s++) {
        const wchar_t *e = *s == L'&' ? L"&amp;" : *s == L'<' ? L"&lt;" : *s == L'>' ? L"&gt;" : NULL;
        wchar_t one[2] = { *s, 0 };
        StringCchCatW(out, n, e ? e : one);
    }
}

/* 0 = no hand-off, 1 = recurring, >1 = one-shot fire time (local epoch secs) */
static void handoff_save(long long v) {
    wchar_t path[MAX_PATH], s[32];
    ini_path(path, MAX_PATH);
    StringCchPrintfW(s, 32, L"%lld", v);
    WritePrivateProfileStringW(L"PowerOff", L"handoff", s, path);
}

static long long handoff_load(void) {
    wchar_t path[MAX_PATH], s[32];
    ini_path(path, MAX_PATH);
    GetPrivateProfileStringW(L"PowerOff", L"handoff", L"0", s, 32, path);
    return _wtoi64(s);
}

static bool wts_exists(void) {
    wchar_t err[256];
    return run_schtasks(L"/query /tn \"" WTS_NAME L"\"", err, 256);
}

static void wts_delete(void) {
    wchar_t err[256];
    run_schtasks(L"/delete /tn \"" WTS_NAME L"\" /f", err, 256);
    handoff_save(0);
}

static bool wts_register(const Runtime *r) {
    static const wchar_t *DAY_XML[] = {
        L"<Monday/>", L"<Tuesday/>", L"<Wednesday/>", L"<Thursday/>",
        L"<Friday/>", L"<Saturday/>", L"<Sunday/>",
    };
    const Plan *p = &r->plan;
    long long now = local_now_secs();
    long long warn = p->remind_min > 0 ? p->remind_min * 60LL : 0;
    long long handoff = 1;
    wchar_t trig[640], start[32], end[32], extra[96] = L"";

    if (p->mode == M_DAILY || p->mode == M_WEEKLY) {
        int days = p->mode == M_DAILY ? 0x7F : p->days;
        long long tod = p->h * 3600LL + p->m * 60LL - warn;
        if (tod < 0) { /* reminder crosses midnight: start a day earlier */
            tod += 86400;
            days = (days >> 1) | ((days & 1) << 6);
        }
        iso_local(now / 86400 * 86400 + tod, start, 32);
        if (days == 0x7F) {
            StringCchPrintfW(trig, 640, L"<CalendarTrigger><StartBoundary>%ls</StartBoundary>"
                L"<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay></CalendarTrigger>", start);
        } else {
            wchar_t dl[160] = L"";
            for (int i = 0; i < 7; i++)
                if (days & (1 << i)) StringCchCatW(dl, 160, DAY_XML[i]);
            StringCchPrintfW(trig, 640, L"<CalendarTrigger><StartBoundary>%ls</StartBoundary>"
                L"<ScheduleByWeek><DaysOfWeek>%ls</DaysOfWeek><WeeksInterval>1</WeeksInterval>"
                L"</ScheduleByWeek></CalendarTrigger>", start, dl);
        }
    } else if (p->mode == M_COUNTDOWN || p->mode == M_ONCE) {
        long long at = r->fire_at + r->delay;
        if (warn > at - now - 15) warn = at - now - 15;
        if (warn < 0) warn = 0;
        if (at - warn <= now) return false;
        iso_local(at - warn, start, 32);
        iso_local(at + 3600, end, 32);
        StringCchPrintfW(trig, 640, L"<TimeTrigger><StartBoundary>%ls</StartBoundary>"
            L"<EndBoundary>%ls</EndBoundary></TimeTrigger>", start, end);
        StringCchCopyW(extra, 96, L"<DeleteExpiredTaskAfter>PT0S</DeleteExpiredTaskAfter>");
        handoff = at;
    } else {
        return false;
    }

    wchar_t exe[MAX_PATH], exe_x[MAX_PATH * 2];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    xml_escape(exe, exe_x, MAX_PATH * 2);
    static wchar_t xml[4096];
    StringCchPrintfW(xml, 4096,
        L"\xFEFF<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Description>Created by PowerOff. Delete it in PowerOff with Cancel task.</Description></RegistrationInfo>"
        L"<Triggers>%ls</Triggers>"
        L"<Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>LeastPrivilege</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<ExecutionTimeLimit>PT1H</ExecutionTimeLimit>%ls</Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>%ls</Command>"
        L"<Arguments>--task %ls --fire-now --warn-secs %lld%ls</Arguments></Exec></Actions></Task>",
        trig, extra, exe_x, TASK_CLI[p->task], warn, p->force ? L" --force" : L"");

    wchar_t tmp[MAX_PATH], file[MAX_PATH], args[MAX_PATH + 96], err[256];
    GetTempPathW(MAX_PATH, tmp);
    StringCchPrintfW(file, MAX_PATH, L"%sPowerOff-task.xml", tmp);
    FILE *f = _wfopen(file, L"wb");
    if (!f) return false;
    fwrite(xml, sizeof(wchar_t), wcslen(xml), f);
    fclose(f);
    StringCchPrintfW(args, MAX_PATH + 96, L"/create /tn \"" WTS_NAME L"\" /xml \"%s\" /f", file);
    bool ok = run_schtasks(args, err, 256);
    DeleteFileW(file);
    if (ok) handoff_save(handoff);
    return ok;
}

/* ------------------------------------------------------------------ theme (artifact tokens) */

static COLORREF T_WIN, T_CARD, T_CARDBRD, T_T1, T_T2, T_CTL, T_CTLBRD,
                T_CTLBOT, T_ACCENT, T_ONACC, T_FLY, T_FLYBRD, T_SUBTLE, T_TOGBRD, T_TOGFILL;

static void theme_tokens(void) {
    if (G.dark) {
        T_WIN = RGB(32, 32, 32);      T_CARD = RGB(43, 43, 43);
        T_CARDBRD = RGB(58, 58, 58);  T_T1 = RGB(255, 255, 255);
        T_T2 = RGB(200, 200, 200);    T_CTL = RGB(47, 47, 47);
        T_CTLBRD = RGB(58, 58, 58);   T_CTLBOT = RGB(61, 61, 61);
        T_ACCENT = RGB(96, 205, 255); T_ONACC = RGB(0, 0, 0);
        T_FLY = RGB(44, 44, 44);      T_FLYBRD = RGB(51, 51, 51);
        T_SUBTLE = RGB(56, 56, 56);   T_TOGBRD = RGB(200, 200, 200);
        T_TOGFILL = RGB(26, 26, 26);
        G.accent = T_ACCENT;
    } else {
        T_WIN = RGB(243, 243, 243);   T_CARD = RGB(251, 251, 251);
        T_CARDBRD = RGB(239, 239, 239); T_T1 = RGB(27, 27, 27);
        T_T2 = RGB(90, 90, 90);       T_CTL = RGB(250, 250, 250);
        T_CTLBRD = RGB(239, 239, 239); T_CTLBOT = RGB(214, 214, 214);
        T_ACCENT = RGB(0, 95, 184);   T_ONACC = RGB(255, 255, 255);
        T_FLY = RGB(249, 249, 249);   T_FLYBRD = RGB(239, 239, 239);
        T_SUBTLE = RGB(234, 234, 234); T_TOGBRD = RGB(90, 90, 90);
        T_TOGFILL = RGB(246, 246, 246);
        G.accent = T_ACCENT;
    }
}

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
    DwmSetWindowAttribute(hwnd, 20, &on, sizeof(on));
    HMODULE ux = LoadLibraryW(L"uxtheme.dll"); /* delay-loaded: may not be mapped yet */
    if (ux) {
        SetPrefModeFn set_mode = (SetPrefModeFn)GetProcAddress(ux, (LPCSTR)135);
        if (set_mode) set_mode(G.dark ? 1 : 0);
        AllowDarkFn allow = (AllowDarkFn)GetProcAddress(ux, (LPCSTR)133);
        if (allow) allow(hwnd, on);
        SetWindowTheme(hwnd, G.dark ? L"DarkMode_Explorer" : NULL, NULL);
        RefreshPolicyFn refresh = (RefreshPolicyFn)GetProcAddress(ux, (LPCSTR)104);
        if (refresh) refresh();
    }
    theme_tokens();
    if (G.br_ctl) DeleteObject(G.br_ctl);
    G.br_ctl = CreateSolidBrush(T_CTL);
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

static void fill_rect_c(HDC hdc, const RECT *rc, COLORREF fill) {
    HBRUSH br = CreateSolidBrush(fill);
    FillRect(hdc, rc, br);
    DeleteObject(br);
}

static void text_at(HDC hdc, const wchar_t *s, RECT rc, HFONT font, COLORREF color, UINT flags) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    SelectObject(hdc, font);
    DrawTextW(hdc, s, -1, &rc, flags);
}

static void line_c(HDC hdc, int x0, int y0, int x1, int y1, COLORREF color, int w) {
    HPEN pen = CreatePen(PS_SOLID, w, color);
    HGDIOBJ op = SelectObject(hdc, pen);
    MoveToEx(hdc, x0, y0, NULL);
    LineTo(hdc, x1, y1);
    SelectObject(hdc, op);
    DeleteObject(pen);
}

/* rounded-rect outline only */
static void round_outline(HDC hdc, const RECT *rc, int rad, COLORREF edge, int w) {
    HPEN pen = CreatePen(PS_SOLID, w, edge);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, rc->left, rc->top, rc->right - 1, rc->bottom - 1, rad, rad);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(pen);
}

/* ------------------------------------------------------------------ icons (GDI approximations of the artifact's Fluent icons) */

/* bg = the color to cut the crescent against (row card vs flyout) */
static void icon_bg(HDC hdc, int x, int y, int s, IconId id, COLORREF c, COLORREF bg) {
    HPEN pen = CreatePen(PS_SOLID, s >= 32 ? 3 : 2, c);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HGDIOBJ op = SelectObject(hdc, pen);
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ obrush = SelectObject(hdc, br);
    int cx = x + s / 2, cy = y + s / 2;
    int q = s / 4;             /* quarter */
    int h = s / 2;             /* half    */
    switch (id) {
    case IC_POWER:
        SelectObject(hdc, ob); /* need brush fill for stem */
        Arc(hdc, cx - q, cy - q + 1, cx + q, cy + q + 1, cx + q, cy - q, cx - q, cy - q);
        RECT st = { cx - 1, y + (s >= 32 ? 2 : 1), cx + 1, cy };
        FillRect(hdc, &st, br);
        break;
    case IC_RESTART:
        SelectObject(hdc, ob);
        Arc(hdc, x + 2, y + 2, x + s - 2, y + s - 2, cx + h - 1, cy - 2, cx - 2, cy - h + 1);
        { /* arrowhead at top-right of arc */
            POINT tri[3] = { { cx + h - 2, cy - h + 4 }, { cx + h - 2, cy - h + 11 },
                             { cx + h - 8, cy - h + 6 } };
            HPEN np = CreatePen(PS_SOLID, 1, c);
            SelectObject(hdc, np);
            SelectObject(hdc, br);
            Polygon(hdc, tri, 3);
            DeleteObject(np);
        }
        break;
    case IC_MOON: /* crescent: filled circle minus offset circle in bg color */
        SelectObject(hdc, br);
        Ellipse(hdc, x + 2, y + 2, x + s - 2, y + s - 2);
        {
            HBRUSH cut = CreateSolidBrush(bg);
            HGDIOBJ oc = SelectObject(hdc, cut);
            HPEN np = CreatePen(PS_SOLID, 1, bg);
            HGDIOBJ opc = SelectObject(hdc, np);
            Ellipse(hdc, x + s / 3, y - 1, x + s + 1, y + s - 1);
            SelectObject(hdc, oc);
            SelectObject(hdc, opc);
            DeleteObject(cut);
            DeleteObject(np);
        }
        break;
    case IC_SIGNOUT:
        SelectObject(hdc, ob);
        Rectangle(hdc, x + 2, y + 2, x + q + 4, y + s - 2);
        line_c(hdc, cx - 2, cy, x + s - 2, cy, c, 2);
        line_c(hdc, x + s - 7, cy - 4, x + s - 2, cy, c, 2);
        line_c(hdc, x + s - 7, cy + 4, x + s - 2, cy, c, 2);
        break;
    case IC_LOCK:
        SelectObject(hdc, ob);
        Arc(hdc, cx - q + 1, y + 1, cx + q - 1, y + h, cx - q + 1, y + h / 2, cx + q - 1, y + h / 2);
        SelectObject(hdc, br);
        Rectangle(hdc, x + 3, cy - 2, x + s - 3, y + s - 2);
        break;
    case IC_MONITOR:
        SelectObject(hdc, ob);
        Rectangle(hdc, x + 2, y + 3, x + s - 2, cy + 2);
        line_c(hdc, cx, cy + 2, cx, y + s - 4, c, 2);
        line_c(hdc, cx - 4, y + s - 4, cx + 4, y + s - 4, c, 2);
        break;
    case IC_ALERT: /* bell */
        SelectObject(hdc, br);
        Pie(hdc, x + 3, y + 2, x + s - 3, y + h + 2, x + 3, y + 2, x + s - 3, y + 2);
        RECT body = { x + 4, cy - 1, x + s - 4, cy + 4 };
        FillRect(hdc, &body, br);
        Ellipse(hdc, cx - 3, cy + 4, cx + 3, cy + 10);
        break;
    case IC_APPS: /* two squares + diamond */
        SelectObject(hdc, br);
        Rectangle(hdc, x + 2, y + 2, x + q + 4, y + q + 4);
        Rectangle(hdc, x + s - q - 4, y + s - q - 4, x + s - 2, y + s - 2);
        { POINT dm[4] = { {cx + 2, cy - 5}, {cx + 7, cy}, {cx + 2, cy + 5}, {cx - 3, cy} };
          Polygon(hdc, dm, 4); }
        break;
    case IC_ROCKET:
        SelectObject(hdc, ob);
        Ellipse(hdc, cx - 4, y + 2, cx + 4, y + s - 6);
        line_c(hdc, cx, y + s - 6, cx, y + s - 1, c, 2);
        line_c(hdc, cx - 4, cy + 3, cx, cy, c, 2);
        line_c(hdc, cx + 4, cy + 3, cx, cy, c, 2);
        break;
    case IC_CALCLK:
        SelectObject(hdc, ob);
        Rectangle(hdc, x + 2, y + 3, x + s - 2, y + s - 5);
        line_c(hdc, x + 2, y + 3 + 4, x + s - 2, y + 3 + 4, c, 2);
        Ellipse(hdc, x + s - 9, y + s - 9, x + s - 1, y + s - 1);
        line_c(hdc, x + s - 5, y + s - 7, x + s - 5, y + s - 5, c, 1);
        break;
    case IC_CLOCK:
        SelectObject(hdc, ob);
        Ellipse(hdc, x + 2, y + 2, x + s - 2, y + s - 2);
        line_c(hdc, cx, cy, cx, cy - 4, c, 2);
        line_c(hdc, cx, cy, cx + 3, cy + 2, c, 2);
        break;
    case IC_BATT:
        SelectObject(hdc, ob);
        Rectangle(hdc, x + 2, y + 6, x + s - 4, y + s - 6);
        RECT tip = { x + s - 3, cy - 2, x + s - 1, cy + 2 };
        FillRect(hdc, &tip, br);
        RECT lvl = { x + 4, y + 8, x + s - 8, y + s - 8 };
        FillRect(hdc, &lvl, br);
        break;
    case IC_INFO:
        SelectObject(hdc, ob);
        Ellipse(hdc, x + 2, y + 2, x + s - 2, y + s - 2);
        RECT dot = { cx - 1, y + 5, cx + 1, y + 8 };
        FillRect(hdc, &dot, br);
        RECT bar = { cx - 1, y + 9, cx + 1, y + s - 6 };
        FillRect(hdc, &bar, br);
        break;
    }
    SelectObject(hdc, obrush);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(pen);
    DeleteObject(br);
}

/* down chevron (combo), inside rc */
static void chevron_down(HDC hdc, const RECT *rc, COLORREF c) {
    int cx = (rc->left + rc->right) / 2, cy = (rc->top + rc->bottom) / 2;
    line_c(hdc, cx - 3, cy - 1, cx, cy + 2, c, 2);
    line_c(hdc, cx, cy + 2, cx + 3, cy - 1, c, 2);
}

/* right chevron (related cards) */
static void chevron_right(HDC hdc, int cx, int cy, COLORREF c) {
    line_c(hdc, cx - 2, cy - 4, cx + 2, cy, c, 2);
    line_c(hdc, cx + 2, cy, cx - 2, cy + 4, c, 2);
}

/* external-link arrow (related power card) */
static void ext_arrow(HDC hdc, int cx, int cy, COLORREF c) {
    line_c(hdc, cx - 5, cy + 5, cx + 4, cy - 4, c, 2);
    line_c(hdc, cx - 1, cy - 4, cx + 4, cy - 4, c, 2);
    line_c(hdc, cx + 4, cy - 4, cx + 4, cy + 1, c, 2);
    line_c(hdc, cx - 5, cy + 1, cx - 5, cy + 5, c, 1);
    line_c(hdc, cx - 5, cy + 5, cx - 1, cy + 5, c, 1);
}

/* ------------------------------------------------------------------ flyout (custom WinUI-style dropdown) */

#define FLY_CLASS L"PowerOffFly"

typedef struct {
    const wchar_t *label;
    IconId ic;
    COLORREF color;
} FlyItem;

static struct {
    HWND wnd, owner;
    int combo_id;
    FlyItem items[16];
    int count, sel, hover, pick;
} FLY;

static void fly_close(void) {
    if (FLY.wnd) {
        HWND w = FLY.wnd;
        FLY.wnd = NULL;
        ReleaseCapture();
        DestroyWindow(w);
    }
}

static void fly_open(HWND owner, int combo_id, const FlyItem *items, int count, int sel,
        const RECT *btn_rc) {
    fly_close();
    FLY.owner = owner;
    FLY.combo_id = combo_id;
    FLY.count = count;
    FLY.sel = sel;
    FLY.hover = -1;
    FLY.pick = -1;
    for (int i = 0; i < count && i < 16; i++) FLY.items[i] = items[i];
    int w = 208, ih = 36;
    int h = count * ih + 4;
    POINT pt = { btn_rc->left, btn_rc->bottom + 2 };
    ClientToScreen(owner, &pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    if (pt.y + h > mi.rcWork.bottom) pt.y = mi.rcWork.bottom - h;
    if (pt.x + w > mi.rcWork.right) pt.x = mi.rcWork.right - w;
    FLY.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, FLY_CLASS, NULL,
        WS_POPUP, pt.x, pt.y, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!FLY.wnd) return;
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, 8, 8);
    SetWindowRgn(FLY.wnd, rgn, FALSE);
    ShowWindow(FLY.wnd, SW_SHOWNOACTIVATE);
    SetCapture(FLY.wnd);
}

static LRESULT CALLBACK fly_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        fill_round(hdc, &rc, 8, T_FLY, T_FLYBRD);
        for (int i = 0; i < FLY.count; i++) {
            RECT ir = { 2, 2 + i * 36, rc.right - 2, 2 + (i + 1) * 36 };
            if (i == FLY.sel || i == FLY.hover)
                fill_round(hdc, &ir, 6, T_SUBTLE, T_SUBTLE);
            if (FLY.items[i].ic != (IconId)-1) {
                icon_bg(hdc, ir.left + 11, (ir.top + ir.bottom) / 2 - 10, 20,
                    FLY.items[i].ic, FLY.items[i].color, T_FLY);
            }
            RECT tr = { ir.left + (FLY.items[i].ic != (IconId)-1 ? 40 : 11),
                        ir.top, ir.right - 12, ir.bottom };
            text_at(hdc, FLY.items[i].label, tr, G.f_row, T_T1,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            if (i == FLY.sel) { /* check mark */
                int cy = (ir.top + ir.bottom) / 2;
                line_c(hdc, ir.right - 26, cy, ir.right - 22, cy + 4, T_T1, 2);
                line_c(hdc, ir.right - 22, cy + 4, ir.right - 15, cy - 5, T_T1, 2);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc;
        GetClientRect(hwnd, &rc);
        int idx = (pt.y - 2) / 36;
        if (pt.x < 0 || pt.x > rc.right || idx < 0 || idx >= FLY.count) idx = -1;
        if (idx != FLY.hover) {
            FLY.hover = idx;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc;
        GetClientRect(hwnd, &rc);
        int idx = (pt.y - 2) / 36;
        if (idx >= 0 && idx < FLY.count && pt.x >= 0 && pt.x <= rc.right) {
            FLY.pick = idx;
            int cid = FLY.combo_id;
            HWND owner = FLY.owner;
            fly_close();
            PostMessageW(owner, WM_COMMAND, MAKEWPARAM(cid, 0), (LPARAM)idx);
        } else {
            fly_close();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (FLY.wnd == hwnd) fly_close();
        return 0;
    case WM_KILLFOCUS:
        fly_close();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) fly_close();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ children helpers */

static HWND mk_child(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style,
        int id, HFONT font) {
    HWND c = CreateWindowExW(0, cls, text, style, 0, 0, 10, 10,
        parent, (HMENU)(INT_PTR)id, NULL, NULL);
    if (c && font) SendMessageW(c, WM_SETFONT, (WPARAM)font, 1);
    return c;
}

static void move(HWND h, int x, int y, int w, int hh) { MoveWindow(h, x, y, w, hh, FALSE); }

static void get_text(HWND h, wchar_t *out, size_t n) {
    out[0] = L'\0';
    GetWindowTextW(h, out, (int)n);
}

static void set_text(HWND h, const wchar_t *s) { SetWindowTextW(h, s); }

/* ------------------------------------------------------------------ layout (artifact geometry) */

#define PAGE_X 40        /* left/right page padding */
#define ROW_H 68        /* standard row height */
#define GAP 4           /* inter-card gap */
#define HDR_GAP 28      /* space above section headers */

static int g_cy; /* layout cursor, client coords */

static RECT card_row(HWND hwnd, int w) {
    RECT rc = { PAGE_X, g_cy, PAGE_X + w, g_cy + ROW_H };
    g_cy += ROW_H + GAP;
    return rc;
}

static void layout(HWND hwnd) {
    RECT crc;
    GetClientRect(hwnd, &crc);
    int w = crc.right; /* client width */
    int content = w - PAGE_X * 2;

    g_cy = 4;
    /* h1 "Schedule" */
    g_cy += 36 + 24;                       /* h1 box + margin */

    G.rc_hero = (RECT){ PAGE_X, g_cy, PAGE_X + content, g_cy + 96 };
    g_cy += 96 + GAP;

    /* "Task" header */
    G.rc_task_hdr = (RECT){ PAGE_X, g_cy + (HDR_GAP - 20), PAGE_X + content, g_cy + HDR_GAP };
    g_cy += HDR_GAP + 8;

    G.rc_action = card_row(hwnd, content);
    G.rc_sched = card_row(hwnd, content);
    G.rc_time = card_row(hwnd, content);

    bool weekly = G.rt.plan.mode == M_WEEKLY;
    if (weekly) {
        G.rc_days = (RECT){ PAGE_X, g_cy, PAGE_X + content, g_cy + 68 };
        g_cy += 68 + GAP;
    } else {
        SetRectEmpty(&G.rc_days);
    }

    G.rc_opts_hdr = (RECT){ PAGE_X, g_cy + (HDR_GAP - 20), PAGE_X + content, g_cy + HDR_GAP };
    g_cy += HDR_GAP + 8;
    G.rc_remind = card_row(hwnd, content);
    G.rc_force = card_row(hwnd, content);
    G.rc_auto = card_row(hwnd, content);
    G.rc_tray = card_row(hwnd, content);

    G.rc_rel_hdr = (RECT){ PAGE_X, g_cy + (HDR_GAP - 20), PAGE_X + content, g_cy + HDR_GAP };
    g_cy += HDR_GAP + 8;
    int half = (content - GAP) / 2;
    G.rc_rel1 = (RECT){ PAGE_X, g_cy, PAGE_X + half, g_cy + 68 };
    G.rc_rel2 = (RECT){ PAGE_X + half + GAP, g_cy, PAGE_X + content, g_cy + 68 };
    g_cy += 68 + 40;                       /* bottom page padding */

    G.client_h = g_cy;

    /* --- position children --- */
    Mode m = G.rt.plan.mode;

    /* combos: right slot of their rows (200 wide, vertically centered) */
    move(G.combo_action, G.rc_action.right - 16 - 200, (G.rc_action.top + G.rc_action.bottom - 32) / 2, 200, 32);
    move(G.combo_mode, G.rc_sched.right - 16 - 200, (G.rc_sched.top + G.rc_sched.bottom - 32) / 2, 200, 32);

    /* Time row right slot: 242 wide */
    RECT slot = { G.rc_time.right - 16 - 242, (G.rc_time.top + G.rc_time.bottom - 32) / 2,
                  G.rc_time.right - 16, 0 };
    slot.bottom = slot.top + 32;
    G.rc_time_ctl = slot;
    bool show_seg = (m == M_DAILY || m == M_WEEKLY);
    ShowWindow(G.combo_cd, m == M_COUNTDOWN ? SW_SHOW : SW_HIDE);
    ShowWindow(G.combo_idle, m == M_IDLE ? SW_SHOW : SW_HIDE);
    ShowWindow(G.eh, show_seg ? SW_SHOW : SW_HIDE);
    ShowWindow(G.em, show_seg ? SW_SHOW : SW_HIDE);
    ShowWindow(G.apm, show_seg ? SW_SHOW : SW_HIDE);
    move(G.combo_cd, slot.left, slot.top, 242, 32);
    move(G.combo_idle, slot.left, slot.top, 242, 32);
    /* segmented picker: h | m | AM/PM thirds */
    move(G.eh, slot.left + 10, slot.top + 3, 61, 26);
    move(G.em, slot.left + 91, slot.top + 3, 61, 26);
    move(G.apm, slot.left + 162, slot.top, 80, 32);

    /* day buttons */
    if (weekly) {
        for (int i = 0; i < 7; i++) {
            int bx = G.rc_days.right - 16 - (7 * 44 + 6 * 4) + i * 48;
            move(G.day[i], bx, (G.rc_days.top + G.rc_days.bottom - 32) / 2, 44, 32);
            ShowWindow(G.day[i], SW_SHOW);
        }
    } else {
        for (int i = 0; i < 7; i++) ShowWindow(G.day[i], SW_HIDE);
    }

    /* toggle pills */
    RECT tr = { G.rc_remind.right - 16 - 40, (G.rc_remind.top + G.rc_remind.bottom - 20) / 2, 0, 0 };
    tr.bottom = tr.top + 20;
    move(G.tg_remind, G.rc_remind.right - 16 - 40, tr.top, 40, 20);
    move(G.tg_force, G.rc_force.right - 16 - 40,
        (G.rc_force.top + G.rc_force.bottom - 20) / 2, 40, 20);
    move(G.tg_auto, G.rc_auto.right - 16 - 40,
        (G.rc_auto.top + G.rc_auto.bottom - 20) / 2, 40, 20);
    move(G.tg_tray, G.rc_tray.right - 16 - 40,
        (G.rc_tray.top + G.rc_tray.bottom - 20) / 2, 40, 20);

    /* hero start button */
    move(G.btn_start, G.rc_hero.right - 20 - 120, (G.rc_hero.top + G.rc_hero.bottom - 32) / 2, 120, 32);

    /* related cards */
    move(G.rel_power, G.rc_rel1.left, G.rc_rel1.top, G.rc_rel1.right - G.rc_rel1.left, 68);
    move(G.rel_about, G.rc_rel2.left, G.rc_rel2.top, G.rc_rel2.right - G.rc_rel2.left, 68);

    /* resize window to content when needed */
    RECT wr;
    GetWindowRect(hwnd, &wr);
    int need_h = G.client_h;
    int cur_h = wr.bottom - wr.top;
    /* caption 48 at 100% DPI; use actual non-client via AdjustWindowRect */
    RECT adj = { 0, 0, w, need_h };
    AdjustWindowRect(&adj, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE);
    int total_h = adj.bottom - adj.top;
    if (cur_h != total_h)
        SetWindowPos(hwnd, NULL, 0, 0, wr.right - wr.left, total_h,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

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

static void set_tray_tip(void) {
    if (!G.tray) return;
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = G.tray;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wchar_t d[96] = L"";
    if (G.rt.active) rt_describe(&G.rt, d, 96);
    StringCchPrintfW(nid.szTip, 128, L"PowerOff%ls%ls%ls", d[0] ? L"\n" : L"", d,
        G.delegated ? L"\nScheduled in Windows Task Scheduler" : L"");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void show_tray_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Show window");
    AppendMenuW(menu, MF_STRING, IDM_START, G.rt.active ? L"Cancel task" : L"Start task");
    AppendMenuW(menu, MF_STRING, IDM_NOW, L"Sleep now");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, G.delegated ? L"Exit (task stays scheduled)" : L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

static DWORD WINAPI reminder_thread(LPVOID param) {
    wchar_t *msg = (wchar_t *)param;
    int r = MessageBoxW(NULL, msg, L"PowerOff \x2014 reminder",
        MB_YESNOCANCEL | MB_ICONWARNING | MB_SYSTEMMODAL);
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

/* ------------------------------------------------------------------ views */

static void hero_strings(wchar_t *title, size_t tn, wchar_t *sub, size_t sn) {
    Runtime *r = &G.rt;
    wchar_t t[32], d[96];
    const wchar_t *act = TASK_NAMES[r->plan.task];
    switch (r->plan.mode) {
    case M_DAILY:
        fmt12(r->plan.h, r->plan.m, t, 32);
        StringCchPrintfW(title, tn, L"%ls at %ls", act, t);
        break;
    case M_WEEKLY:
        fmt12(r->plan.h, r->plan.m, t, 32);
        days_text(r->plan.days, d, 96);
        StringCchPrintfW(title, tn, L"%ls at %ls on %ls", act, t, d);
        break;
    case M_COUNTDOWN:
        dur_label(r->plan.cd, d, 96);
        StringCchPrintfW(title, tn, L"%ls in %ls", act, d);
        break;
    case M_IDLE:
        dur_label(r->plan.idle, d, 96);
        StringCchPrintfW(title, tn, L"%ls after %ls idle", act, d);
        break;
    default:
        rt_describe(r, title, tn);
    }
    if (!r->active) {
        StringCchCopyW(sub, sn, L"Not scheduled");
        return;
    }
    if (r->plan.mode == M_IDLE) {
        dur_label(r->plan.idle, d, 96);
        StringCchPrintfW(sub, sn, L"Scheduled \x00B7 waiting for %ls of inactivity", d);
    } else {
        long long rem = rt_remaining(r);
        long long tgt = local_now_secs() + rem;
        wchar_t dl[64], dur[32];
        day_label(tgt, dl, 64);
        fmt_dur(rem, dur, 32);
        StringCchPrintfW(sub, sn, L"Scheduled for %ls \x00B7 %ls left", dl, dur);
    }
}

/* Time row sub + right control helpers */
static void time_row_sub(wchar_t *sub, size_t n) {
    Plan *p = &G.rt.plan;
    wchar_t d[96];
    if (p->mode == M_WEEKLY) {
        StringCchCopyW(sub, n, L"Runs at this time on the days below");
    } else if (p->mode == M_COUNTDOWN) {
        dur_label(p->cd, d, 96);
        StringCchPrintfW(sub, n, L"Runs %ls from Start", d);
    } else if (p->mode == M_IDLE) {
        dur_label(p->idle, d, 96);
        StringCchPrintfW(sub, n, L"After %ls with no mouse or keyboard", d);
    } else {
        long long tgt = next_at(p->h, p->m, 0x7F);
        day_label(tgt, d, 96);
        StringCchPrintfW(sub, n, L"Next: %ls", d);
    }
}

static void update_seg(void) {
    int h = G.rt.plan.h, m = G.rt.plan.m;
    int h12 = h % 12;
    if (h12 == 0) h12 = 12;
    wchar_t s[16];
    StringCchPrintfW(s, 16, L"%d", h12);
    set_text(G.eh, s);
    StringCchPrintfW(s, 16, L"%02d", m);
    set_text(G.em, s);
    set_text(G.apm, h < 12 ? L"AM" : L"PM");
}

static void update_combo_texts(void) {
    set_text(G.combo_action, TASK_NAMES[G.rt.plan.task]);
    int gi = 0;
    for (int i = 0; i < NGUI_MODES; i++)
        if (GUI_MODES[i] == G.rt.plan.mode) gi = i;
    set_text(G.combo_mode, GUI_MODE_NAMES[gi]);
    wchar_t d[96];
    dur_label(G.rt.plan.cd, d, 96);
    set_text(G.combo_cd, d);
    dur_label(G.rt.plan.idle, d, 96);
    set_text(G.combo_idle, d);
}

static void update_views(void) {
    Runtime *r = &G.rt;
    if (!G.main) return; /* tray-only: nothing to draw */
    set_text(G.btn_start, r->active ? L"Cancel" : L"Start");
    InvalidateRect(G.main, NULL, FALSE);
}

static void tick(void) {
    Runtime *r = &G.rt;
    rt_apply_signals(r);
    if (G.delegated) { /* Windows fires it; just track one-shot expiry */
        if (r->active && rt_remaining(r) <= 0 &&
                (r->plan.mode == M_COUNTDOWN || r->plan.mode == M_ONCE)) {
            r->active = false;
            G.delegated = false;
            handoff_save(0);
            set_tray_tip();
        }
        update_views();
        return;
    }
    if (r->active && rt_remaining(r) <= 0) {
        Task task = r->plan.task;
        Mode mode = r->plan.mode;
        if (!do_task(task)) {
            wchar_t m[256];
            StringCchPrintfW(m, 256,
                L"Failed to execute '%ls'. (Sleep/Hibernate may be disabled on this PC.)",
                TASK_NAMES[task]);
            MessageBoxW(G.main, m, L"PowerOff", MB_OK | MB_ICONERROR);
        }
        switch (mode) {
        case M_DAILY:
        case M_WEEKLY:
            r->reminded = false;
            r->delay = 0;
            break;
        case M_IDLE:
            if (task == T_SLEEP || task == T_HIBERNATE || task == T_LOCK || task == T_DISPLAY)
                r->reminded = false;
            else
                r->active = false;
            break;
        case M_INTERVAL: {
            long long mins = wcstoll(r->plan.p1, NULL, 10);
            if (mins < 1) mins = 60;
            r->fire_at = local_now_secs() + mins * 60;
            r->span = mins * 60;
            r->reminded = false;
            break;
        }
        default: /* countdown / once: one-shot */
            r->active = false;
            break;
        }
        save_config(&r->plan);
    }
    /* reminder */
    if (r->active && r->plan.remind_min > 0 && !r->reminded) {
        long long rem = rt_remaining(r);
        if (rem <= (long long)r->plan.remind_min * 60 && rem > 5) {
            r->reminded = true;
            spawn_reminder(TASK_NAMES[r->plan.task], rem);
        }
    }
    update_views();
}

static void save_current(void) {
    Plan *p = &G.rt.plan;
    p->remind_min = G.remind_on ? 5 : 0;
    p->force = G.force_on;
    save_config(p);
}

static void toggle_start(void) {
    Runtime *r = &G.rt;
    if (r->active) {
        r->active = false;
        if (G.delegated) { wts_delete(); G.delegated = false; }
        set_tray_tip();
        update_views();
        return;
    }
    save_current();
    wchar_t desc[256];
    const wchar_t *err = rt_start(r, desc, 256);
    if (!err) save_config(&r->plan);
    else MessageBoxW(G.main, err, L"PowerOff", MB_OK | MB_ICONWARNING);
    update_views();
}

/* ------------------------------------------------------------------ row painting */

static void paint_card(HDC hdc, const RECT *rc) {
    fill_round(hdc, rc, 4, T_CARD, T_CARDBRD);
}

static void paint_row(HDC hdc, const RECT *rc, IconId ic, COLORREF col,
        const wchar_t *title, const wchar_t *sub) {
    paint_card(hdc, rc);
    icon_bg(hdc, rc->left + 18, (rc->top + rc->bottom) / 2 - 10, 20, ic, col, T_CARD);
    RECT tr = { rc->left + 18 + 20 + 18, rc->top + 12, rc->right - 16 - 260, rc->top + 34 };
    text_at(hdc, title, tr, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sr = { tr.left, rc->top + 36, tr.right, rc->bottom - 12 };
    text_at(hdc, sub, sr, G.f_sub, T_T2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_toggle(HDC hdc, HWND btn, bool on) {
    RECT rc;
    GetClientRect(btn, &rc);
    RECT pr = { 0, 0, 40, 20 };
    if (on) {
        fill_round(hdc, &pr, 10, T_ACCENT, T_ACCENT);
        HBRUSH br = CreateSolidBrush(T_ONACC);
        HPEN pen = CreatePen(PS_SOLID, 1, T_ONACC);
        HGDIOBJ ob = SelectObject(hdc, br);
        HGDIOBJ op = SelectObject(hdc, pen);
        Ellipse(hdc, 27, 4, 39, 16);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(br);
        DeleteObject(pen);
    } else {
        fill_round(hdc, &pr, 10, T_TOGFILL, T_TOGBRD);
        HBRUSH br = CreateSolidBrush(T_TOGBRD);
        HPEN pen = CreatePen(PS_SOLID, 1, T_TOGBRD);
        HGDIOBJ ob = SelectObject(hdc, br);
        HGDIOBJ op = SelectObject(hdc, pen);
        Ellipse(hdc, 4, 4, 16, 16);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(br);
        DeleteObject(pen);
    }
}

static void paint_combo_btn(HDC hdc, HWND btn) {
    RECT rc;
    GetClientRect(btn, &rc);
    fill_round(hdc, &rc, 4, T_CTL, T_CTLBRD);
    line_c(hdc, rc.left, rc.bottom - 2, rc.right - 1, rc.bottom - 2, T_CTLBOT, 1);
    wchar_t txt[96];
    GetWindowTextW(btn, txt, 96);
    RECT tr = { rc.left + 11, rc.top, rc.right - 26, rc.bottom };
    text_at(hdc, txt, tr, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT cr = { rc.right - 24, rc.top, rc.right, rc.bottom };
    chevron_down(hdc, &cr, T_T2);
}

static void paint_day_btn(HDC hdc, HWND btn, bool on) {
    RECT rc;
    GetClientRect(btn, &rc);
    if (on) {
        fill_round(hdc, &rc, 4, T_ACCENT, T_CTLBOT);
        wchar_t d[8];
        GetWindowTextW(btn, d, 8);
        text_at(hdc, d, rc, G.f_row, T_ONACC, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    } else {
        fill_round(hdc, &rc, 4, T_CTL, T_CTLBRD);
        line_c(hdc, rc.left, rc.bottom - 2, rc.right - 1, rc.bottom - 2, T_CTLBOT, 1);
        wchar_t d[8];
        GetWindowTextW(btn, d, 8);
        text_at(hdc, d, rc, G.f_row, T_T1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
}

static void paint_start_btn(HDC hdc, HWND btn, bool armed) {
    RECT rc;
    GetClientRect(btn, &rc);
    wchar_t t[32];
    GetWindowTextW(btn, t, 32);
    if (armed) {
        fill_round(hdc, &rc, 4, T_CARD, T_CTLBRD);
        text_at(hdc, t, rc, G.f_row, T_T1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    } else {
        fill_round(hdc, &rc, 4, T_ACCENT, T_CTLBOT);
        text_at(hdc, t, rc, G.f_row, T_ONACC, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
}

static void paint_rel_card(HDC hdc, HWND btn, IconId ic, COLORREF col,
        const wchar_t *title, const wchar_t *sub, bool ext) {
    RECT rc;
    GetClientRect(btn, &rc);
    paint_card(hdc, &rc);
    icon_bg(hdc, rc.left + 18, (rc.top + rc.bottom) / 2 - 10, 20, ic, col, T_CARD);
    RECT tr = { rc.left + 56, rc.top + 12, rc.right - 30, rc.top + 34 };
    text_at(hdc, title, tr, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sr = { rc.left + 56, rc.top + 36, rc.right - 30, rc.bottom - 12 };
    text_at(hdc, sub, sr, G.f_sub, T_T2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    int cx = rc.right - 18, cy = (rc.top + rc.bottom) / 2;
    if (ext) ext_arrow(hdc, cx, cy, T_T1);
    else chevron_right(hdc, cx, cy, T_T1);
}

/* ------------------------------------------------------------------ main window proc */

static void ensure_tray(void);

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        G.main = hwnd;
        G.f_h1 = make_font(L"Segoe UI Variable Display", L"Segoe UI", -28, FW_SEMIBOLD);
        G.f_hero = make_font(L"Segoe UI Variable Text", L"Segoe UI", -20, FW_SEMIBOLD);
        G.f_row = make_font(L"Segoe UI Variable Text", L"Segoe UI", -14, FW_NORMAL);
        G.f_sub = make_font(L"Segoe UI Variable Text", L"Segoe UI", -12, FW_NORMAL);
        G.f_seg = make_font(L"Segoe UI Variable Text", L"Segoe UI", -14, FW_NORMAL);
        DWORD obtn = WS_CHILD | WS_VISIBLE | BS_OWNERDRAW;
        DWORD edit_seg = WS_CHILD | WS_VISIBLE | ES_CENTER; /* borderless */

        G.combo_action = mk_child(hwnd, L"BUTTON", TASK_NAMES[0], obtn, ID_COMBO_ACTION, G.f_row);
        G.combo_mode = mk_child(hwnd, L"BUTTON", GUI_MODE_NAMES[0], obtn, ID_COMBO_MODE, G.f_row);
        G.combo_cd = mk_child(hwnd, L"BUTTON", L"1 hour", obtn, ID_COMBO_CD, G.f_row);
        G.combo_idle = mk_child(hwnd, L"BUTTON", L"15 minutes", obtn, ID_COMBO_IDLE, G.f_row);
        G.eh = mk_child(hwnd, L"EDIT", L"11", edit_seg, ID_EH, G.f_seg);
        G.em = mk_child(hwnd, L"EDIT", L"30", edit_seg, ID_EM, G.f_seg);
        G.apm = mk_child(hwnd, L"BUTTON", L"PM", obtn, ID_APM, G.f_row);
        for (int i = 0; i < 7; i++)
            G.day[i] = mk_child(hwnd, L"BUTTON", DAYS[i], obtn, ID_DAY0 + i, G.f_row);
        G.tg_remind = mk_child(hwnd, L"BUTTON", L"", obtn, ID_TG_REMIND, G.f_row);
        G.tg_force = mk_child(hwnd, L"BUTTON", L"", obtn, ID_TG_FORCE, G.f_row);
        G.tg_auto = mk_child(hwnd, L"BUTTON", L"", obtn, ID_TG_AUTO, G.f_row);
        G.tg_tray = mk_child(hwnd, L"BUTTON", L"", obtn, ID_TG_TRAY, G.f_row);
        G.btn_start = mk_child(hwnd, L"BUTTON", L"Start", obtn, ID_START, G.f_row);
        G.rel_power = mk_child(hwnd, L"BUTTON", L"", obtn, ID_REL_POWER, G.f_row);
        G.rel_about = mk_child(hwnd, L"BUTTON", L"", obtn, ID_REL_ABOUT, G.f_row);

        G.ready = true;
        update_seg();
        update_combo_texts();
        layout(hwnd);
        update_views();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT crc;
        GetClientRect(hwnd, &crc);
        fill_rect_c(hdc, &crc, T_WIN);
        if (G.ready) {
            Plan *p = &G.rt.plan;
            /* h1 */
            RECT h1 = { PAGE_X, 4, crc.right - PAGE_X, 4 + 36 };
            text_at(hdc, L"Schedule", h1, G.f_h1, T_T1, DT_LEFT | DT_SINGLELINE);
            /* hero */
            paint_card(hdc, &G.rc_hero);
            icon_bg(hdc, G.rc_hero.left + 24, (G.rc_hero.top + G.rc_hero.bottom) / 2 - 16, 32,
                TASK_ICON[p->task], TASK_COLOR[p->task], T_CARD);
            wchar_t ht[128], hs[128];
            hero_strings(ht, 128, hs, 128);
            RECT htr = { G.rc_hero.left + 24 + 32 + 20, G.rc_hero.top + 20,
                         G.rc_hero.right - 20 - 120 - 16, G.rc_hero.top + 48 };
            text_at(hdc, ht, htr, G.f_hero, T_T1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT hsr = { htr.left, G.rc_hero.top + 52, htr.right, G.rc_hero.top + 74 };
            text_at(hdc, hs, hsr, G.f_row, T_T2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            /* section headers */
            text_at(hdc, L"Task", G.rc_task_hdr, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE);
            text_at(hdc, L"Options", G.rc_opts_hdr, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE);
            text_at(hdc, L"Related settings", G.rc_rel_hdr, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE);
            /* task rows */
            wchar_t sub[128];
            StringCchCopyW(sub, 128, L"What happens when the time comes");
            paint_row(hdc, &G.rc_action, TASK_ICON[p->task], TASK_COLOR[p->task],
                L"Action", sub);
            paint_row(hdc, &G.rc_sched, IC_CALCLK, C_CALCLK, L"Schedule",
                L"Choose what triggers the action");
            time_row_sub(sub, 128);
            paint_row(hdc, &G.rc_time, IC_CLOCK, C_CLOCK, L"Time", sub);
            /* segmented picker chrome (behind the eh/em/apm children) */
            if (p->mode == M_DAILY || p->mode == M_WEEKLY) {
                RECT sg = G.rc_time_ctl;
                fill_round(hdc, &sg, 4, T_CTL, T_CTLBRD);
                line_c(hdc, sg.left + 81, sg.top + 1, sg.left + 81, sg.bottom - 2, T_CTLBRD, 1);
                line_c(hdc, sg.left + 162, sg.top + 1, sg.left + 162, sg.bottom - 2, T_CTLBRD, 1);
                line_c(hdc, sg.left, sg.bottom - 2, sg.right - 1, sg.bottom - 2, T_CTLBOT, 1);
            }
            /* days row */
            if (p->mode == M_WEEKLY) {
                paint_card(hdc, &G.rc_days);
                wchar_t d[96];
                days_text(p->days, d, 96);
                RECT dr = { G.rc_days.left + 18, G.rc_days.top + 12,
                            G.rc_days.right - 16 - (7 * 44 + 6 * 4) - 18, G.rc_days.top + 34 };
                text_at(hdc, L"Repeat on", dr, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE);
                RECT dsr = { dr.left, G.rc_days.top + 36, dr.right, G.rc_days.bottom - 12 };
                StringCchPrintfW(sub, 128, L"Repeats %ls", d);
                text_at(hdc, sub, dsr, G.f_sub, T_T2, DT_LEFT | DT_SINGLELINE);
            }
            /* options rows (On/Off labels left of the pill children) */
            static const struct { RECT *rc; IconId ic; COLORREF col; const wchar_t *t, *s; bool *on; } OPTS[] = {
                { &G.rc_remind, IC_ALERT, C_ALERT, L"Remind me before",
                  L"Show a notification 5 minutes before the action", &G.remind_on },
                { &G.rc_force, IC_APPS, C_APPS, L"Force apps to close",
                  L"Apps with unsaved work won't stop the action", &G.force_on },
                { &G.rc_auto, IC_ROCKET, C_ROCKET, L"Start with Windows",
                  L"Run in the system tray when you sign in", &G.auto_on },
                { &G.rc_tray, IC_MONITOR, C_INFO, L"Tray icon",
                  L"Off: closing exits PowerOff; Windows still runs the task", &G.tray_on },
            };
            for (int i = 0; i < 4; i++) {
                paint_row(hdc, OPTS[i].rc, OPTS[i].ic, OPTS[i].col, OPTS[i].t, OPTS[i].s);
                RECT lr = { OPTS[i].rc->right - 16 - 40 - 12 - 34,
                            (OPTS[i].rc->top + OPTS[i].rc->bottom) / 2 - 10,
                            OPTS[i].rc->right - 16 - 40 - 12,
                            (OPTS[i].rc->top + OPTS[i].rc->bottom) / 2 + 10 };
                text_at(hdc, *OPTS[i].on ? L"On" : L"Off", lr, G.f_row, T_T1,
                    DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            }
            /* related cards are owner-drawn buttons (see WM_DRAWITEM) */
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, T_CTL);
        SetTextColor(hdc, T_T1);
        if (!G.br_ctl) G.br_ctl = CreateSolidBrush(T_CTL);
        return (LRESULT)G.br_ctl;
    }
    case WM_DRAWITEM: {
        if (!G.ready) return 0;
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        int id = (int)di->CtlID;
        bool pressed = (di->itemState & ODS_SELECTED) != 0;
        if (id == ID_COMBO_ACTION || id == ID_COMBO_MODE || id == ID_COMBO_CD || id == ID_COMBO_IDLE)
            paint_combo_btn(di->hDC, di->hwndItem);
        else if (id == ID_APM) {
            RECT rc = di->rcItem;
            wchar_t t[8];
            GetWindowTextW(di->hwndItem, t, 8);
            text_at(di->hDC, t, rc, G.f_row, T_T1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        } else if (id >= ID_DAY0 && id < ID_DAY0 + 7) {
            paint_day_btn(di->hDC, di->hwndItem, (G.rt.plan.days >> (id - ID_DAY0)) & 1);
        } else if (id == ID_TG_REMIND)
            paint_toggle(di->hDC, di->hwndItem, G.remind_on);
        else if (id == ID_TG_FORCE)
            paint_toggle(di->hDC, di->hwndItem, G.force_on);
        else if (id == ID_TG_AUTO)
            paint_toggle(di->hDC, di->hwndItem, G.auto_on);
        else if (id == ID_TG_TRAY)
            paint_toggle(di->hDC, di->hwndItem, G.tray_on);
        else if (id == ID_START)
            paint_start_btn(di->hDC, di->hwndItem, G.rt.active);
        else if (id == ID_REL_POWER)
            paint_rel_card(di->hDC, di->hwndItem, IC_BATT, C_BATT,
                L"Power & battery", L"Sleep, screen timeout, power mode", true);
        else if (id == ID_REL_ABOUT)
            paint_rel_card(di->hDC, di->hwndItem, IC_INFO, C_INFO,
                L"About PowerOff", L"Version 1.0.0", false);
        (void)pressed;
        return 0;
    }
    case WM_COMMAND: {
        if (!G.ready) return 0;
        int id = (int)(wp & 0xFFFF);
        int notif = (int)((wp >> 16) & 0xFFFF);
        if (notif == 0 && id == ID_COMBO_ACTION && FLY.pick >= 0) {
            G.rt.plan.task = (Task)FLY.pick;
            FLY.pick = -1;
            update_combo_texts();
            save_current();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (notif == 0 && id == ID_COMBO_MODE && FLY.pick >= 0) {
            G.rt.plan.mode = GUI_MODES[FLY.pick];
            FLY.pick = -1;
            update_combo_texts();
            layout(hwnd);       /* day row may appear/vanish; window resizes */
            save_current();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (notif == 0 && id == ID_COMBO_CD && FLY.pick >= 0) {
            G.rt.plan.cd = CDS[FLY.pick];
            FLY.pick = -1;
            update_combo_texts();
            save_current();
            return 0;
        }
        if (notif == 0 && id == ID_COMBO_IDLE && FLY.pick >= 0) {
            G.rt.plan.idle = IDLES[FLY.pick];
            FLY.pick = -1;
            update_combo_texts();
            save_current();
            return 0;
        }
        if (notif == BN_CLICKED) {
            switch (id) {
            case ID_COMBO_ACTION: case ID_COMBO_MODE: case ID_COMBO_CD: case ID_COMBO_IDLE: {
                RECT rc;
                GetWindowRect((HWND)lp, &rc); /* screen coords, convert below */
                POINT pt = { rc.left, rc.top };
                ScreenToClient(hwnd, &pt);
                RECT brc = { pt.x, pt.y, pt.x + (rc.right - rc.left), pt.y + (rc.bottom - rc.top) };
                if (id == ID_COMBO_ACTION) {
                    FlyItem it[NTASKS];
                    for (int i = 0; i < NTASKS; i++) {
                        it[i].label = TASK_NAMES[i];
                        it[i].ic = TASK_ICON[i];
                        it[i].color = TASK_COLOR[i];
                    }
                    fly_open(hwnd, id, it, NTASKS, (int)G.rt.plan.task, &brc);
                } else if (id == ID_COMBO_MODE) {
                    FlyItem it[NGUI_MODES];
                    for (int i = 0; i < NGUI_MODES; i++) {
                        it[i].label = GUI_MODE_NAMES[i];
                        it[i].ic = (IconId)-1;
                        it[i].color = 0;
                    }
                    int sel = 0;
                    for (int i = 0; i < NGUI_MODES; i++)
                        if (GUI_MODES[i] == G.rt.plan.mode) sel = i;
                    fly_open(hwnd, id, it, NGUI_MODES, sel, &brc);
                } else if (id == ID_COMBO_CD) {
                    FlyItem it[NCDS];
                    static wchar_t lbl[NCDS][32];
                    for (int i = 0; i < NCDS; i++) {
                        dur_label(CDS[i], lbl[i], 32);
                        it[i].label = lbl[i];
                        it[i].ic = (IconId)-1;
                        it[i].color = 0;
                    }
                    int sel = 0;
                    for (int i = 0; i < NCDS; i++) if (CDS[i] == G.rt.plan.cd) sel = i;
                    fly_open(hwnd, id, it, NCDS, sel, &brc);
                } else {
                    FlyItem it[NIDLES];
                    static wchar_t lbl[NIDLES][32];
                    for (int i = 0; i < NIDLES; i++) {
                        dur_label(IDLES[i], lbl[i], 32);
                        it[i].label = lbl[i];
                        it[i].ic = (IconId)-1;
                        it[i].color = 0;
                    }
                    int sel = 0;
                    for (int i = 0; i < NIDLES; i++) if (IDLES[i] == G.rt.plan.idle) sel = i;
                    fly_open(hwnd, id, it, NIDLES, sel, &brc);
                }
                return 0;
            }
            case ID_APM: { /* toggle AM/PM: shift 12h */
                G.rt.plan.h = (G.rt.plan.h + 12) % 24;
                update_seg();
                save_current();
                return 0;
            }
            case ID_START:
                toggle_start();
                return 0;
            case ID_TG_REMIND:
                G.remind_on = !G.remind_on;
                save_current();
                InvalidateRect(hwnd, &G.rc_remind, FALSE);
                return 0;
            case ID_TG_FORCE:
                G.force_on = !G.force_on;
                save_current();
                InvalidateRect(hwnd, &G.rc_force, FALSE);
                return 0;
            case ID_TG_AUTO:
                G.auto_on = !G.auto_on;
                set_startup(G.auto_on);
                save_current();
                InvalidateRect(hwnd, &G.rc_auto, FALSE);
                return 0;
            case ID_TG_TRAY: {
                wchar_t path[MAX_PATH];
                G.tray_on = !G.tray_on;
                ini_path(path, MAX_PATH);
                WritePrivateProfileStringW(L"PowerOff", L"tray", G.tray_on ? L"1" : L"0", path);
                if (!G.tray_on && G.tray) DestroyWindow(G.tray);
                InvalidateRect(hwnd, &G.rc_tray, FALSE);
                return 0;
            }
            case ID_REL_POWER:
                ShellExecuteW(NULL, L"open", L"ms-settings:powersleep", NULL, NULL, SW_SHOW);
                return 0;
            case ID_REL_ABOUT:
                MessageBoxW(hwnd,
                    L"PowerOff 1.0 (C edition)\n"
                    L"Tiny native Win32 auto-shutdown \x2014 no framework, ~46 KB.\n\n"
                    L"Task Scheduler entry 'PowerOff daily' can be managed with\n"
                    L"PowerOff.exe --install-daily / --uninstall.",
                    L"About PowerOff", MB_OK | MB_ICONINFORMATION);
                return 0;
            default:
                if (id >= ID_DAY0 && id < ID_DAY0 + 7) {
                    int i = id - ID_DAY0;
                    G.rt.plan.days ^= 1 << i;
                    InvalidateRect(hwnd, &G.rc_days, FALSE);
                    save_current();
                    return 0;
                }
                break;
            }
        }
        if (notif == EN_CHANGE && (id == ID_EH || id == ID_EM)) {
            /* parse 12h edit + AM/PM into 24h */
            wchar_t a[16], b[16], apm[8];
            get_text(G.eh, a, 16);
            get_text(G.em, b, 16);
            get_text(G.apm, apm, 8);
            int h12 = _wtoi(a), m = _wtoi(b);
            bool pm = (apm[0] == L'P');
            if (h12 >= 1 && h12 <= 12 && m >= 0 && m <= 59) {
                int h = h12 % 12;
                if (pm) h += 12;
                G.rt.plan.h = h;
                G.rt.plan.m = m;
            }
        }
        return 0;
    }
    case WM_SETTINGCHANGE:
        read_theme();
        apply_dark_mode(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_CLOSE: {
        /* The window is always destroyed, not hidden: reopening rebuilds it, and the
         * process left behind is just the tray icon -- or nothing at all. */
        if (G.rt.active) {
            if (wts_register(&G.rt)) G.delegated = true;
            else if (G.delegated) { wts_delete(); G.delegated = false; } /* fall back to in-process */
        }
        bool in_process = G.rt.active && !G.delegated; /* idle mode, or hand-off failed */
        if (G.tray_on && !in_process) {
            /* Restart as a fresh "--tray" process: the UI DLLs and their heaps loaded by
             * the window can't be unloaded, a new process never maps them. Windows go
             * first so the new copy's single-instance check doesn't find this one. */
            wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16];
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            StringCchPrintfW(cmd, MAX_PATH + 16, L"\"%s\" --tray", exe);
            if (G.tray) DestroyWindow(G.tray);
            DestroyWindow(hwnd); /* no tray window left: posts WM_QUIT */
            if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            return 0;
        }
        if (G.tray_on || in_process) ensure_tray();
        DestroyWindow(hwnd); /* quits the app if no tray icon is left */
        set_tray_tip();
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        return 0;
    }
    case WM_DESTROY: {
        HFONT *fonts[] = { &G.f_h1, &G.f_hero, &G.f_row, &G.f_sub, &G.f_seg };
        fly_close();
        G.ready = false;
        G.main = NULL;
        for (int i = 0; i < 5; i++) { DeleteObject(*fonts[i]); *fonts[i] = NULL; }
        if (G.br_ctl) { DeleteObject(G.br_ctl); G.br_ctl = NULL; }
        if (!G.tray) PostQuitMessage(0);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ run GUI */

/* The main window is built on demand and destroyed on close. The comctl32/uxtheme/
 * dwmapi/shell32/powrprof imports are delay-loaded (build.bat), so a tray-only process
 * maps only what one hidden window and one notify icon need. */
static void show_main(void) {
    if (G.main) {
        ShowWindow(G.main, SW_SHOWNORMAL);
        SetForegroundWindow(G.main);
        return;
    }
    static bool cc_ready;
    if (!cc_ready) {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&icc);
        cc_ready = true;
    }
    read_theme();
    theme_tokens();
    /* estimate size: layout needs a window; create then resize in WM_CREATE */
    RECT adj = { 0, 0, 640, 700 };
    AdjustWindowRect(&adj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND hwnd = CreateWindowExW(0, L"PowerOffWnd", L"PowerOff",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, adj.right - adj.left, adj.bottom - adj.top,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return;
    apply_dark_mode(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

static UINT g_taskbar_created;

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if ((lp & 0xFFFF) == WM_LBUTTONUP) show_main();
        else if ((lp & 0xFFFF) == WM_RBUTTONUP) show_tray_menu(hwnd);
        return 0;
    case WM_COMMAND:
        switch (wp & 0xFFFF) {
        case IDM_SHOW: show_main(); return 0;
        case IDM_START: toggle_start(); set_tray_tip(); return 0;
        case IDM_NOW: do_task(T_SLEEP); return 0;
        case IDM_EXIT:
            if (G.main) DestroyWindow(G.main);
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        del_tray(hwnd);
        G.tray = NULL;
        if (!G.main) PostQuitMessage(0);
        return 0;
    default:
        if (msg == g_taskbar_created && msg) { add_tray(hwnd); set_tray_tip(); return 0; } /* Explorer restarted */
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ensure_tray(void) {
    if (G.tray) return;
    G.tray = CreateWindowExW(0, L"PowerOffTray", L"", WS_POPUP, 0, 0, 0, 0,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (G.tray) { add_tray(G.tray); set_tray_tip(); }
}

static void CALLBACK timer_proc(HWND h, UINT m, UINT_PTR id, DWORD t) {
    (void)h; (void)m; (void)id; (void)t;
    if (G.rt.active) tick();
}

static void run_gui(bool tray) {
    { /* single instance: hand the request to a running copy */
        HWND other = FindWindowW(L"PowerOffWnd", NULL);
        if (other) {
            if (!tray) { ShowWindow(other, SW_SHOWNORMAL); SetForegroundWindow(other); }
            return;
        }
        other = FindWindowW(L"PowerOffTray", NULL);
        if (other) {
            if (!tray) { AllowSetForegroundWindow(ASFW_ANY); PostMessageW(other, WM_COMMAND, IDM_SHOW, 0); }
            return;
        }
    }
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"PowerOffWnd";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = fly_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = FLY_CLASS;
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = hi;
    wc.lpszClassName = L"PowerOffTray";
    RegisterClassW(&wc);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    memset(&G.rt, 0, sizeof(G.rt));
    load_config(&G.rt.plan);
    G.remind_on = G.rt.plan.remind_min > 0;
    G.force_on = G.rt.plan.force;
    G.auto_on = get_startup();
    {
        wchar_t path[MAX_PATH];
        ini_path(path, MAX_PATH);
        G.tray_on = GetPrivateProfileIntW(L"PowerOff", L"tray", 1, path) != 0;
    }
    { /* pick up a task handed to Task Scheduler by an earlier run */
        long long h = handoff_load();
        wchar_t desc[256];
        if (h && wts_exists() && (h == 1 || h > local_now_secs()) &&
                !rt_start(&G.rt, desc, 256)) {
            if (h > 1) { G.rt.fire_at = h; G.rt.span = h - local_now_secs(); }
            G.delegated = true;
        } else if (h) {
            handoff_save(0);
        }
    }
    SetTimer(NULL, 0, 1000, timer_proc);

    if (!tray) show_main();
    else if (G.tray_on) ensure_tray();
    else return; /* tray icon turned off: nothing to show, Task Scheduler has the job */
    if (!G.main && !G.tray) return;
    if (tray) SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* ------------------------------------------------------------------ headless (CLI) */

static void run_headless(Plan *plan) {
    Runtime r;
    memset(&r, 0, sizeof(r));
    r.plan = *plan;
    wchar_t desc[256];
    const wchar_t *err = rt_start(&r, desc, 256);
    if (err) {
        fwprintf(stderr, L"PowerOff: %ls\n", err);
        exit(2);
    }
    wprintf(L"PowerOff: %ls\n", desc);
    for (;;) {
        rt_apply_signals(&r);
        if (!r.active) {
            wprintf(L"PowerOff: cancelled.\n");
            break;
        }
        long long rem = rt_remaining(&r);
        if (rem <= 0) {
            if (do_task_ex(r.plan.task, r.plan.force)) {
                wprintf(L"PowerOff: executed '%ls'.\n", TASK_NAMES[r.plan.task]);
            } else {
                fwprintf(stderr, L"PowerOff: failed to execute '%ls'.\n",
                    TASK_NAMES[r.plan.task]);
                exit(3);
            }
            if (r.plan.mode == M_INTERVAL || r.plan.mode == M_DAILY || r.plan.mode == M_WEEKLY) {
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
                    (r.plan.task == T_SLEEP || r.plan.task == T_HIBERNATE ||
                     r.plan.task == T_LOCK || r.plan.task == T_DISPLAY)) {
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
            wprintf(L"PowerOff: '%ls' in %ls \x2026\n", TASK_NAMES[r.plan.task], dur);
            r.reminded = true;
        }
        Sleep(1000);
    }
}

static DWORD WINAPI warn_box_thread(LPVOID param) {
    return (DWORD)MessageBoxW(NULL, (const wchar_t *)param, L"PowerOff \x2014 reminder",
        MB_OKCANCEL | MB_ICONWARNING | MB_SYSTEMMODAL | MB_SETFOREGROUND);
}

/* Shows the reminder; fires when it times out (nobody at the PC), on OK, never on Cancel. */
static void warn_then_fire(Task task, unsigned warn_secs, bool force) {
    if (warn_secs > 0) {
        wchar_t msg[256], dur[32];
        fmt_dur(warn_secs, dur, 32);
        StringCchPrintfW(msg, 256,
            L"%ls in %ls.\n\nOK = do it now   \x2022   Cancel = skip this time", TASK_NAMES[task], dur);
        HANDLE th = CreateThread(NULL, 0, warn_box_thread, msg, 0, NULL);
        if (th) {
            DWORD code = IDOK;
            if (WaitForSingleObject(th, warn_secs * 1000) == WAIT_OBJECT_0)
                GetExitCodeThread(th, &code);
            CloseHandle(th);
            if (code == IDCANCEL) {
                wprintf(L"PowerOff: skipped by user.\n");
                return;
            }
        }
    }
    if (do_task_ex(task, force)) {
        wprintf(L"PowerOff: executed '%ls'.\n", TASK_NAMES[task]);
    } else {
        fwprintf(stderr, L"PowerOff: failed to execute '%ls'.\n", TASK_NAMES[task]);
        exit(3);
    }
    ExitProcess(0); /* closes a still-open reminder box */
}

/* ------------------------------------------------------------------ CLI */

static void print_help(void) {
    wprintf(
        L"PowerOff 1.0 \x2014 tiny auto shutdown (native Win32, C)\n"
        L"\n"
        L"GUI (no args, defaults to Sleep / at 11:00 PM):\n"
        L"  PowerOff.exe [--tray] [--dark|--light]\n"
        L"\n"
        L"Headless scheduler (runs in console until the task fires):\n"
        L"  PowerOff.exe --task sleep --daily 23:00\n"
        L"  PowerOff.exe --task shutdown --once \"2026-10-05 23:00\"\n"
        L"  PowerOff.exe --task sleep --in 3600 | --countdown 1:00:00\n"
        L"  PowerOff.exe --task lock --idle 15 | --task sleep --every 60\n"
        L"  PowerOff.exe --task sleep --weekly 23:00 Mon,Tue,Wed,Thu,Fri\n"
        L"\n"
        L"Fire immediately (for Task Scheduler / scripts):\n"
        L"  PowerOff.exe --task sleep --fire-now [--warn-secs 60]\n"
        L"\n"
        L"Daily 11 PM sleep that survives reboot (recommended):\n"
        L"  PowerOff.exe --install-daily 23:00 --task sleep\n"
        L"  PowerOff.exe --uninstall\n"
        L"\n"
        L"Tasks: shutdown restart sleep hibernate logoff lock display\n");
}

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
    wchar_t clean[32];
    size_t j = 0;
    for (size_t i = 0; s[i] && j < 31; i++) {
        wchar_t c = s[i];
        if (c >= L'A' && c <= L'Z') c += 32;
        if (c == L'_' || c == L'-') continue;
        clean[j++] = c;
    }
    clean[j] = L'\0';
    for (int i = 0; i < NTASKS; i++) {
        if (!wcscmp(clean, TASK_CLI[i])) { *out = (Task)i; return true; }
    }
    /* aliases */
    if (!wcscmp(clean, L"reboot")) { *out = T_RESTART; return true; }
    if (!wcscmp(clean, L"logout")) { *out = T_LOGOFF; return true; }
    if (!wcscmp(clean, L"signout")) { *out = T_LOGOFF; return true; }
    if (!wcscmp(clean, L"poweroff")) { *out = T_SHUTDOWN; return true; }
    if (!wcscmp(clean, L"monitoroff")) { *out = T_DISPLAY; return true; }
    if (!wcscmp(clean, L"suspend")) { *out = T_SLEEP; return true; }
    return false;
}

static int days_from_list(const wchar_t *s) {
    /* Mon,Tue,Wed,Thu,Fri,Sat,Sun — also 'weekdays'/'weekends'/'daily' */
    if (!wcscmp(s, L"daily") || !wcscmp(s, L"everyday")) return 0x7F;
    if (!wcscmp(s, L"weekdays")) return 0x1F;
    if (!wcscmp(s, L"weekends")) return 0x60;
    int mask = 0;
    const wchar_t *p = s;
    while (*p) {
        while (*p == L' ' || *p == L',') p++;
        wchar_t tok[8];
        size_t k = 0;
        while (*p && *p != L',' && *p != L' ' && k < 7) tok[k++] = *p++;
        tok[k] = L'\0';
        if (!k) break;
        for (int i = 0; i < 7; i++)
            if (!_wcsicmp(tok, DAYS[i])) { mask |= 1 << i; break; }
    }
    return mask;
}

/* Built as a windowed program (no console flash on launch or from Task Scheduler).
 * CLI use borrows the console of the shell that started it. */
static void attach_parent_console(int argc, wchar_t **argv) {
    static const wchar_t *GUI_FLAGS[] = { L"--tray", L"-tray", L"--dark", L"--light" };
    bool cli = false;
    for (int i = 1; i < argc && !cli; i++) {
        cli = true;
        for (int k = 0; k < 4; k++)
            if (!wcscmp(argv[i], GUI_FLAGS[k])) cli = false;
    }
    if (!cli) return;
    /* redirected output (pipe/file) is already wired up by the CRT; leave it alone */
    bool out_ok = _fileno(stdout) >= 0, err_ok = _fileno(stderr) >= 0;
    if ((out_ok && err_ok) || !AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE *f;
    if (!out_ok) _wfreopen_s(&f, L"CONOUT$", L"w", stdout);
    if (!err_ok) _wfreopen_s(&f, L"CONOUT$", L"w", stderr);
    if (!out_ok) wprintf(L"\n"); /* the shell has already printed its prompt */
}

int wmain(int argc, wchar_t **argv) {
    attach_parent_console(argc, argv);
    /* UTF-16 for a real console, UTF-8 for pipes and files */
    _setmode(_fileno(stdout), _isatty(_fileno(stdout)) ? _O_U16TEXT : _O_U8TEXT);
    _setmode(_fileno(stderr), _isatty(_fileno(stderr)) ? _O_U16TEXT : _O_U8TEXT);

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
        fwprintf(stderr, L"PowerOff: %ls\n", err);
        return 1;
    }
    {
        const wchar_t *st = arg_val(argc, argv, L"--install-daily");
        if (st) {
            Task task = T_SLEEP;
            const wchar_t *tn = arg_val(argc, argv, L"--task");
            if (tn && !task_from_arg(tn, &task)) {
                fwprintf(stderr, L"PowerOff: unknown task '%ls'.\n", tn);
                return 2;
            }
            unsigned h, m;
            if (!parse_hhmm(st, &h, &m)) {
                fwprintf(stderr, L"PowerOff: use HH:MM, e.g. 23:00\n");
                return 2;
            }
            wchar_t exe[MAX_PATH], tr[1024], args[1536], err[256], hhmm[16];
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            StringCchPrintfW(tr, 1024, L"\"%s\" --task %ls --fire-now --warn-secs 60",
                exe, TASK_CLI[task]);
            StringCchPrintfW(hhmm, 16, L"%02u:%02u", h, m);
            StringCchPrintfW(args, 1536,
                L"/create /tn \"PowerOff daily\" /sc daily /st %ls /f /tr \"%ls\"", hhmm, tr);
            if (run_schtasks(args, err, 256)) {
                wprintf(L"PowerOff: Scheduled '%ls' daily at %ls (Task Scheduler: 'PowerOff daily').\n",
                    TASK_NAMES[task], hhmm);
                return 0;
            }
            fwprintf(stderr, L"PowerOff: %ls\n", err);
            return 1;
        }
    }

    const wchar_t *task_raw = arg_val(argc, argv, L"--task");
    Task task_opt = T_SLEEP;
    bool have_task = false;
    if (task_raw) {
        if (!task_from_arg(task_raw, &task_opt)) {
            fwprintf(stderr, L"PowerOff: unknown task '%ls'. Tasks: shutdown restart sleep hibernate logoff lock display\n", task_raw);
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
        warn_then_fire(task_opt, w ? (unsigned)wcstoul(w, NULL, 10) : 0,
            has_flag(argc, argv, L"--force"));
        return 0;
    }

    Mode mode = M_DAILY;
    const wchar_t *raw = NULL;
    const wchar_t *v;
    if ((v = arg_val(argc, argv, L"--daily")) != NULL) { mode = M_DAILY; raw = v; }
    else if ((v = arg_val(argc, argv, L"--weekly")) != NULL) { mode = M_WEEKLY; raw = v; }
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
        plan.days = 0x7F;
        plan.remind_min = 1;
        plan.force = has_flag(argc, argv, L"--force");
        switch (mode) {
        case M_DAILY: {
            unsigned h, m;
            if (!parse_hhmm(raw, &h, &m)) {
                fwprintf(stderr, L"PowerOff: --daily needs HH:MM\n");
                return 2;
            }
            plan.h = (int)h; plan.m = (int)m;
            StringCchPrintfW(plan.p1, 64, L"%u", h);
            StringCchPrintfW(plan.p2, 64, L"%u", m);
            break;
        }
        case M_WEEKLY: {
            /* "23:00 Mon,Tue,..." */
            const wchar_t *sp = wcschr(raw, L' ');
            unsigned h, m;
            wchar_t hhmm[16];
            if (sp) {
                StringCchCopyNW(hhmm, 16, raw, (size_t)(sp - raw));
                sp++;
            } else {
                StringCchCopyW(hhmm, 16, raw);
                sp = L"daily";
            }
            if (!parse_hhmm(hhmm, &h, &m)) {
                fwprintf(stderr, L"PowerOff: --weekly needs \"HH:MM Mon,Tue,...\"\n");
                return 2;
            }
            plan.h = (int)h; plan.m = (int)m;
            plan.days = days_from_list(sp);
            if (!plan.days) {
                fwprintf(stderr, L"PowerOff: no valid days in '%ls'\n", sp);
                return 2;
            }
            break;
        }
        case M_ONCE: {
            const wchar_t *sp = wcschr(raw, L' ');
            if (!sp) {
                fwprintf(stderr, L"PowerOff: --once needs \"YYYY-MM-DD HH:MM\"\n");
                return 2;
            }
            StringCchCopyNW(plan.p1, 64, raw, (size_t)(sp - raw));
            StringCchCopyW(plan.p2, 64, sp + 1);
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
                const wchar_t *p2 = raw;
                bool alld = raw[0] != L'\0';
                for (; *p2; p2++)
                    if (*p2 < L'0' || *p2 > L'9') { alld = false; break; }
                if (!alld || s <= 0) {
                    fwprintf(stderr, L"PowerOff: bad countdown '%ls'\n", raw);
                    return 2;
                }
            }
            if (h < 0 || m < 0 || (s <= 0 && h == 0 && m == 0)) {
                fwprintf(stderr, L"PowerOff: bad countdown '%ls'\n", raw);
                return 2;
            }
            plan.cd = (int)(h * 3600 + m * 60 + s) / 60;
            if (plan.cd < 1) plan.cd = 1;
            plan.remind_min = 0;
            break;
        }
        case M_IDLE:
        case M_INTERVAL:
            StringCchCopyW(plan.p1, 64, raw);
            plan.idle = _wtoi(raw);
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
    read_theme();
    if (has_flag(argc, argv, L"--dark")) G.dark = true;
    if (has_flag(argc, argv, L"--light")) G.dark = false;
    run_gui(has_flag(argc, argv, L"--tray") || has_flag(argc, argv, L"-tray"));
    return 0;
}
