/* PowerOff: Plan/runtime model, time math, power actions, scheduler, settings. Part of the unity build: included by poweroff.c, in order. */

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

#define APP_NAME L"PowerNapps PowerOff"   /* shown name (v3 design) */
#define LINK_POWER L"Power settings"
#define LINK_ABOUT L"About"
#define REMIND_MIN 1 /* the reminder comes 1 minute before, with a countdown */

typedef struct {
    Task task;
    Mode mode;
    int h, m;          /* daily/weekly target time, 24h */
    int cd;            /* countdown minutes (preset) */
    int idle;          /* idle minutes (preset) */
    int days;          /* weekly: bitmask Mon=1..Sun=64 */
    int remind_min;    /* reminder lead in minutes, 0 = off */
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
    IC_ALERT, IC_APPS, IC_ROCKET, IC_CALCLK, IC_CLOCK, IC_SUN, IC_HIBER,
} IconId;

static IconId TASK_ICON[] = {
    IC_POWER, IC_RESTART, IC_MOON, IC_HIBER, IC_SIGNOUT, IC_LOCK, IC_MONITOR,
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

/* ------------------------------------------------------------------ globals */

typedef struct {
    HWND main;
    /* interactive children */
    HWND combo_action, combo_mode, combo_cd, combo_idle;
    HWND time_btn;                    /* Time row: opens the time picker flyout */
    HWND day[7];
    HWND tray;                        /* tray icon owner, exists only while the icon is shown */
    HWND tg_remind, tg_force, tg_auto, tg_tray, tg_wake;
    HWND wake_btn;                    /* Wake time row: same picker */
    HWND btn_start, rel_power, rel_about;
    /* fonts */
    HFONT f_hero, f_row, f_sub, f_hdr;
    /* cached brushes for WM_CTLCOLOR* */
    HBRUSH br_ctl;
    /* layout rects (client coords) */
    RECT rc_sched_hdr, rc_hero, rc_task_hdr, rc_action, rc_sched, rc_time, rc_days;
    RECT rc_opts_hdr, rc_remind, rc_force, rc_auto, rc_tray, rc_wake, rc_wake_time;
    RECT rc_rel1, rc_rel2; /* footer links */
    int client_h;
    /* state */
    bool ready, dark;
    DWORD accent;
    Runtime rt;
    bool remind_on, force_on, auto_on;
    bool tray_on;     /* keep a tray icon after the window closes; off = process exits */
    int cap_hover, cap_down; /* custom title bar buttons: 1 min, 2 max, 3 close */
    HWND hover_btn;          /* owner-drawn control under the mouse */
    const RECT *hover_row;   /* Options row under the mouse (incl. over its toggle) */
    bool inactive;           /* window not active: dimmed caption */
    HWND last_focus;         /* focused control while inactive, restored on activation */
    bool kbd_nav;            /* last input was a key: show the focus ring (a click hides it) */
    bool wake_on, wake_dirty; /* daily wake task; dirty = time edited, not yet re-registered */
    int wake_h, wake_m;
    bool delegated;   /* the active schedule is the "PowerOff" task: Windows runs it, we display it */
    long long snooze_until; /* reminder card snoozed the task's run to this time (local secs) */
} Globals;
static Globals G;

enum {
    ID_COMBO_ACTION = 200, ID_COMBO_MODE, ID_COMBO_CD, ID_COMBO_IDLE,
    ID_TIME = 210, ID_WAKE_TIME,
    ID_DAY0 = 220, /* ..226 */
    ID_TG_REMIND = 230, ID_TG_FORCE, ID_TG_AUTO, ID_TG_TRAY, ID_TG_WAKE,
    ID_START = 240, ID_REL_POWER = 250, ID_REL_ABOUT,
    IDM_SHOW = 1001, IDM_START, IDM_NOW, IDM_EXIT,
};
#define WM_TRAY (WM_APP + 1)

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
    unsigned y_ = yoe + (unsigned)(era * 400); /* era >= 0 for any date after year 0 */
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

/* Shut down = EWX_POWEROFF (what Start > Shut down does): EWX_SHUTDOWN alone only halts
 * at "safe to turn off", which leaves a VM (and some PCs) running. Planned-shutdown
 * reason codes, so the event log says why. */
#define PO_REASON (SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED)

static bool do_task_ex(Task t, bool force) {
    DWORD extra = force ? EWX_FORCE : EWX_FORCEIFHUNG;
    switch (t) {
    case T_LOCK: return LockWorkStation() != 0;
    case T_SLEEP: return SetSuspendState(FALSE, TRUE, FALSE) != 0;
    case T_HIBERNATE: return SetSuspendState(TRUE, TRUE, FALSE) != 0;
    case T_DISPLAY:
        SendMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
        return true;
    case T_LOGOFF: enable_shutdown_privilege(); return ExitWindowsEx(EWX_LOGOFF | extra, PO_REASON) != 0;
    case T_SHUTDOWN: enable_shutdown_privilege(); return ExitWindowsEx(EWX_POWEROFF | extra, PO_REASON) != 0;
    case T_RESTART: enable_shutdown_privilege(); return ExitWindowsEx(EWX_REBOOT | extra, PO_REASON) != 0;
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
    /* a countdown no longer than the reminder lead would remind the moment it starts */
    if ((r->plan.mode == M_COUNTDOWN || r->plan.mode == M_ONCE || r->plan.mode == M_INTERVAL) &&
            r->span <= r->plan.remind_min * 60LL)
        r->reminded = true;
    rt_describe(r, desc, n);
    return NULL;
}

static long long rt_remaining(Runtime *r) {
    long long now = local_now_secs();
    if (G.snooze_until > now) return G.snooze_until - now; /* snoozed from the reminder card */
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
        r->reminded = false; /* snoozed: remind again before the new time */
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
    p->remind_min = REMIND_MIN; p->force = false;
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
            p->remind_min = GetPrivateProfileIntW(L"PowerOff", L"remind", REMIND_MIN, path) ? REMIND_MIN : 0;
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
    p->remind_min = GetPrivateProfileIntW(L"PowerOff", L"remind", REMIND_MIN, path) ? REMIND_MIN : 0;
    p->force = GetPrivateProfileIntW(L"PowerOff", L"force", 0, path) != 0;
}

/* Store (MSIX) build: running from a package changes how Windows can start us */
static bool packaged(void) {
    static int p = -1;
    if (p < 0) {
        UINT32 n = 0;
        p = GetCurrentPackageFullName(&n, NULL) != APPMODEL_ERROR_NO_PACKAGE;
    }
    return p != 0;
}

/* The path Windows should launch: in a package, its app execution alias (Task Scheduler
 * can't start an exe inside WindowsApps directly); otherwise this exe. */
static void app_exe(wchar_t *exe, DWORD n) {
    if (!packaged() || !ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\poweroff.exe", exe, n))
        GetModuleFileNameW(NULL, exe, n);
}

