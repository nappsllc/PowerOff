/* PowerOff: Windows Task Scheduler integration. Part of the unity build: included by poweroff.c, in order. */

/* ------------------------------------------------------------------ Task Scheduler
 * The "PowerOff" scheduled task IS the schedule: Start registers it, edits update it,
 * Cancel deletes it, and the window reads it back (ts_load_plan) on start and whenever
 * it is activated, so edits made in Task Scheduler show up here. Windows runs it even
 * when PowerOff isn't running; it starts remind_min early with
 * "--task X --fire-now --warn-secs N [--force]" (the reminder card, then the action).
 * Idle mode can't be a task (Task Scheduler's idle trigger has no custom minutes) and
 * stays in-process. Talks to the Task Scheduler COM API (ITaskService) directly. */

#define WTS_NAME L"PowerOff"
#define WTS_WAKE L"PowerOff wake"
#define WTS_DAILY L"PowerOff daily" /* --install-daily */
#define WTS_STARTUP L"PowerOff startup" /* Start with Windows, Store build */

static void iso_local(long long secs, wchar_t *out, size_t n) {
    int y; unsigned mo, d;
    long long s = secs % 86400;
    civil_from_days(secs / 86400, &y, &mo, &d);
    StringCchPrintfW(out, n, L"%04d-%02u-%02uT%02lld:%02lld:%02lld",
        y, mo, d, s / 3600, s / 60 % 60, s % 60);
}

static long long parse_iso(const wchar_t *s) { /* "YYYY-MM-DDTHH:MM:SS" (local) -> epoch secs, 0 = bad */
    int y, mo, d, h, mi, se = 0;
    if (swscanf_s(s, L"%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 5) return 0;
    return days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400LL + h * 3600LL + mi * 60LL + se;
}

static void xml_escape(const wchar_t *s, wchar_t *out, size_t n) {
    out[0] = L'\0';
    for (; *s; s++) {
        const wchar_t *e = *s == L'&' ? L"&amp;" : *s == L'<' ? L"&lt;" : *s == L'>' ? L"&gt;" : NULL;
        wchar_t one[2] = { *s, 0 };
        StringCchCatW(out, n, e ? e : one);
    }
}

/* text of the first <tag>...</tag>, false if absent */
static bool xml_get(const wchar_t *xml, const wchar_t *tag, wchar_t *out, size_t n) {
    wchar_t open[48];
    StringCchPrintfW(open, 48, L"<%ls>", tag);
    const wchar_t *a = wcsstr(xml, open);
    if (!a) return false;
    a += wcslen(open);
    const wchar_t *b = wcsstr(a, L"</");
    if (!b) return false;
    size_t len = (size_t)(b - a) < n - 1 ? (size_t)(b - a) : n - 1;
    wmemcpy(out, a, len);
    out[len] = L'\0';
    return true;
}

/* ------------------------------------------------------------------ COM plumbing */

static ITaskFolder *ts_root(void) { /* "\" folder, connected once per process */
    static ITaskFolder *root;
    if (root) return root;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ITaskService *svc = NULL;
    if (FAILED(CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskService, (void **)&svc)))
        return NULL;
    VARIANT e;
    VariantInit(&e);
    if (SUCCEEDED(ITaskService_Connect(svc, e, e, e, e))) {
        BSTR p = SysAllocString(L"\\");
        ITaskService_GetFolder(svc, p, &root);
        SysFreeString(p);
    }
    ITaskService_Release(svc);
    return root;
}

static bool ts_register(const wchar_t *name, const wchar_t *xml) {
    ITaskFolder *f = ts_root();
    if (!f) return false;
    BSTR n = SysAllocString(name), x = SysAllocString(xml);
    VARIANT e;
    VariantInit(&e);
    IRegisteredTask *t = NULL;
    HRESULT hr = ITaskFolder_RegisterTask(f, n, x, TASK_CREATE_OR_UPDATE, e, e,
        TASK_LOGON_INTERACTIVE_TOKEN, e, &t);
    if (t) IRegisteredTask_Release(t);
    SysFreeString(n);
    SysFreeString(x);
    return SUCCEEDED(hr);
}

static void ts_delete(const wchar_t *name) {
    ITaskFolder *f = ts_root();
    if (!f) return;
    BSTR n = SysAllocString(name);
    ITaskFolder_DeleteTask(f, n, 0);
    SysFreeString(n);
}

typedef struct {
    bool enabled;
    TASK_STATE state;     /* TASK_STATE_RUNNING while the reminder / action runs */
    long long next_run;   /* local epoch secs, 0 = none */
    wchar_t xml[8192];
} TaskInfo;

static bool ts_read(const wchar_t *name, TaskInfo *ti) {
    ITaskFolder *f = ts_root();
    if (!f) return false;
    BSTR n = SysAllocString(name);
    IRegisteredTask *t = NULL;
    HRESULT hr = ITaskFolder_GetTask(f, n, &t);
    SysFreeString(n);
    if (FAILED(hr) || !t) return false;
    VARIANT_BOOL en = VARIANT_FALSE;
    IRegisteredTask_get_Enabled(t, &en);
    ti->enabled = en != VARIANT_FALSE;
    ti->state = TASK_STATE_UNKNOWN;
    IRegisteredTask_get_State(t, &ti->state);
    DATE nr = 0;
    SYSTEMTIME st;
    ti->next_run = 0;
    if (SUCCEEDED(IRegisteredTask_get_NextRunTime(t, &nr)) && nr > 1 && VariantTimeToSystemTime(nr, &st))
        ti->next_run = days_from_civil(st.wYear, st.wMonth, st.wDay) * 86400LL +
                       st.wHour * 3600LL + st.wMinute * 60LL + st.wSecond;
    BSTR x = NULL;
    ti->xml[0] = L'\0';
    if (SUCCEEDED(IRegisteredTask_get_Xml(t, &x)) && x) {
        StringCchCopyW(ti->xml, 8192, x);
        SysFreeString(x);
    }
    IRegisteredTask_Release(t);
    return true;
}

/* Registers (or replaces) a task that runs this exe with `args`. Task XML, unlike
 * plain schtasks flags, can run on battery, expire one-shot tasks and wake the PC.
 * RegistrationInfo/Date records when it was scheduled (a countdown's start). */
static bool wts_create(const wchar_t *name, const wchar_t *trig, const wchar_t *extra,
                       const wchar_t *args) {
    wchar_t exe[MAX_PATH], exe_x[MAX_PATH * 2], now[32];
    app_exe(exe, MAX_PATH);
    xml_escape(exe, exe_x, MAX_PATH * 2);
    iso_local(local_now_secs(), now, 32);
    static wchar_t xml[4096];
    StringCchPrintfW(xml, 4096,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Date>%ls</Date><Author>PowerOff</Author>"
        L"<Description>Created by PowerOff. Change or cancel it in PowerOff.</Description></RegistrationInfo>"
        L"<Triggers>%ls</Triggers>"
        L"<Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>LeastPrivilege</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"%ls%ls</Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>%ls</Command>"
        L"<Arguments>%ls</Arguments></Exec></Actions></Task>",
        now, trig, wcsstr(extra, L"ExecutionTimeLimit") ? L"" : L"<ExecutionTimeLimit>PT1H</ExecutionTimeLimit>",
        extra, exe_x, args);
    return ts_register(name, xml);
}

/* ------------------------------------------------------------------ the wake task
 * Daily wake from sleep/hibernate: WakeToRun makes Windows arm the RTC. It runs "--wake",
 * which turns the display on (an unattended timer wake leaves it off). */

static bool wts_wake_register(int h, int m) {
    wchar_t start[32], trig[256];
    iso_local(local_now_secs() / 86400 * 86400 + h * 3600LL + m * 60LL, start, 32);
    StringCchPrintfW(trig, 256, L"<CalendarTrigger><StartBoundary>%ls</StartBoundary>"
        L"<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay></CalendarTrigger>", start);
    return wts_create(WTS_WAKE, trig, L"<WakeToRun>true</WakeToRun>", L"--wake");
}

static void wts_wake_delete(void) { ts_delete(WTS_WAKE); }

/* ------------------------------------------------------------------ the scheduled action */

static const wchar_t *DAY_XML[] = {
    L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday", L"Sunday",
};

/* Schedulable as a task: everything but idle (and the CLI-only interval). */
static bool wts_schedulable(Mode m) {
    return m == M_DAILY || m == M_WEEKLY || m == M_COUNTDOWN || m == M_ONCE;
}

/* Plan -> "PowerOff" task (created or replaced). */
static bool wts_register(const Runtime *r) {
    const Plan *p = &r->plan;
    long long now = local_now_secs();
    long long warn = p->remind_min > 0 ? p->remind_min * 60LL : 0;
    wchar_t trig[640], start[32], end[32], extra[96] = L"", at_arg[16] = L"";

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
            wchar_t dl[200] = L"";
            for (int i = 0; i < 7; i++) {
                if (!(days & (1 << i))) continue;
                StringCchCatW(dl, 200, L"<");
                StringCchCatW(dl, 200, DAY_XML[i]);
                StringCchCatW(dl, 200, L"/>");
            }
            StringCchPrintfW(trig, 640, L"<CalendarTrigger><StartBoundary>%ls</StartBoundary>"
                L"<ScheduleByWeek><DaysOfWeek>%ls</DaysOfWeek><WeeksInterval>1</WeeksInterval>"
                L"</ScheduleByWeek></CalendarTrigger>", start, dl);
        }
        /* The next time is closer than the reminder lead (just picked, a minute ahead):
         * the calendar trigger's start for it has passed, so Windows would wait a day.
         * A one-off trigger starts the reminder now; --at counts it down to the time. */
        int pd = p->mode == M_DAILY ? 0x7F : p->days;
        long long next_at = 0;
        for (int d = 0; d < 2 && !next_at; d++) {
            long long at = now / 86400 * 86400 + d * 86400LL + p->h * 3600LL + p->m * 60LL;
            int wd = (int)((at / 86400 + 3) % 7); /* 1970-01-01 was a Thursday; Monday = 0 */
            if (at > now && ((pd >> wd) & 1)) next_at = at;
        }
        if (warn && next_at && next_at - warn <= now + 2) {
            wchar_t s2[32], e2[32], t2[200];
            iso_local(now + 3, s2, 32);
            iso_local(next_at + 600, e2, 32);
            StringCchPrintfW(t2, 200, L"<TimeTrigger><StartBoundary>%ls</StartBoundary>"
                L"<EndBoundary>%ls</EndBoundary></TimeTrigger>", s2, e2);
            StringCchCatW(trig, 640, t2);
        }
        StringCchPrintfW(at_arg, 16, L" --at %02d:%02d", p->h, p->m);
    } else if (p->mode == M_COUNTDOWN || p->mode == M_ONCE) {
        long long at = r->fire_at + r->delay;
        if (warn > at - now - 15) warn = 0; /* no room for a reminder: just run at the time */
        if (at - warn <= now) return false;
        iso_local(at - warn, start, 32);
        iso_local(at + 3600, end, 32);
        StringCchPrintfW(trig, 640, L"<TimeTrigger><StartBoundary>%ls</StartBoundary>"
            L"<EndBoundary>%ls</EndBoundary></TimeTrigger>", start, end);
        StringCchCopyW(extra, 96, L"<DeleteExpiredTaskAfter>PT0S</DeleteExpiredTaskAfter>");
    } else {
        return false;
    }

    wchar_t args[112];
    StringCchPrintfW(args, 112, L"--task %ls --fire-now --warn-secs %lld%ls%ls",
        TASK_CLI[p->task], warn, at_arg, p->force ? L" --force" : L"");
    return wts_create(WTS_NAME, trig, extra, args);
}

static void wts_delete(void) { ts_delete(WTS_NAME); }

/* "PowerOff" task -> runtime. Fills r->plan (action, force, reminder, time, days or
 * countdown) and the timing; false if there is no task or it has nothing left to run. */
static bool ts_load_plan(Runtime *r) {
    static TaskInfo ti; /* 16 KB of XML: off the stack */
    if (!ts_read(WTS_NAME, &ti) || !ti.enabled) return false;
    const wchar_t *x = ti.xml;
    wchar_t args[256] = L"", sb[40] = L"", dt[40] = L"";
    xml_get(x, L"Arguments", args, 256);
    xml_get(x, L"StartBoundary", sb, 40);
    xml_get(x, L"Date", dt, 40);
    long long start = parse_iso(sb), now = local_now_secs();
    if (!start) return false;

    Plan *p = &r->plan;
    const wchar_t *a = wcsstr(args, L"--task ");
    if (a) {
        wchar_t name[16] = L"";
        swscanf_s(a + 7, L"%15s", name, 16);
        for (int i = 0; i < NTASKS; i++)
            if (!_wcsicmp(name, TASK_CLI[i])) p->task = (Task)i;
    }
    long long warn = 0;
    if ((a = wcsstr(args, L"--warn-secs ")) != NULL) warn = _wtoi64(a + 12);
    p->force = wcsstr(args, L"--force") != NULL;
    if (warn > 0) p->remind_min = REMIND_MIN; /* a 0 lead can also mean "no room": keep the setting */

    r->delay = 0;
    r->reminded = true; /* the task's own --fire-now process shows the reminder */
    if (wcsstr(x, L"<TimeTrigger>") && !wcsstr(x, L"<CalendarTrigger>")) { /* one-shot countdown */
        long long at = start + warn, created = parse_iso(dt);
        if (at <= now - 5 && !ti.next_run && ti.state != TASK_STATE_RUNNING) return false; /* already ran */
        p->mode = M_COUNTDOWN;
        r->fire_at = at;
        r->span = created && created < at ? at - created : at - now;
        p->cd = (int)((r->span + 30) / 60);
    } else if (wcsstr(x, L"<CalendarTrigger>")) {
        long long tod = start % 86400 + warn;
        int days = 0x7F;
        const wchar_t *w = wcsstr(x, L"<ScheduleByWeek>");
        if (w) {
            days = 0;
            for (int i = 0; i < 7; i++) {
                wchar_t t[24];
                StringCchPrintfW(t, 24, L"<%ls", DAY_XML[i]);
                if (wcsstr(w, t)) days |= 1 << i;
            }
        }
        if (tod >= 86400) { /* the reminder started the day before: action day is the next */
            tod -= 86400;
            days = ((days << 1) | (days >> 6)) & 0x7F;
        }
        p->h = (int)(tod / 3600);
        p->m = (int)(tod / 60 % 60);
        p->days = days;
        p->mode = days == 0x7F && !w ? M_DAILY : M_WEEKLY;
        r->fire_at = 0;
        r->span = p->mode == M_DAILY ? 86400 : 7 * 86400;
    } else {
        return false; /* a trigger PowerOff doesn't make (edited by hand): not ours to show */
    }
    r->active = true;
    return true;
}

/* ------------------------------------------------------------------ Start with Windows
 * The HKCU Run value; in the Store build a package's registry writes are private, so a
 * Run value would never run: a logon task starts the tray instead. */

static void set_startup(bool on) {
    if (packaged()) {
        if (!on) { ts_delete(WTS_STARTUP); return; }
        wchar_t dom[128] = L"", usr[128] = L"", who[260], who_x[520], trig[640];
        GetEnvironmentVariableW(L"USERDOMAIN", dom, 128);
        GetEnvironmentVariableW(L"USERNAME", usr, 128);
        StringCchPrintfW(who, 260, L"%ls\\%ls", dom, usr);
        xml_escape(who, who_x, 520);
        StringCchPrintfW(trig, 640, L"<LogonTrigger><UserId>%ls</UserId></LogonTrigger>", who_x);
        wts_create(WTS_STARTUP, trig, L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>", L"--tray");
        return;
    }
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
    if (packaged()) {
        TaskInfo ti;
        return ts_read(WTS_STARTUP, &ti);
    }
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
