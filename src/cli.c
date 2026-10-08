/* PowerOff: headless scheduler and command line. Part of the unity build: included by poweroff.c, in order. */

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

static int g_rm_result;

static void snooze_mark(long long until) { /* read by the window (refresh_from_task) */
    wchar_t path[MAX_PATH], v[24];
    ini_path(path, MAX_PATH);
    StringCchPrintfW(v, 24, L"%lld", until);
    WritePrivateProfileStringW(L"PowerOff", L"snooze_until", v, path);
}
static void rm_done_cli(int result) { g_rm_result = result; PostQuitMessage(0); }

/* Run by the Task Scheduler task warn_secs before the time: the reminder card counts
 * down; no answer or "now" runs the action, Stop skips it, Snooze waits 10 minutes and
 * asks again 1 minute before. */
static void warn_then_fire(Task task, unsigned warn_secs, bool force) {
    while (warn_secs > 0) {
        g_rm_result = RM_PENDING;
        reminder_show(task, warn_secs, rm_done_cli);
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_rm_result == RM_STOP) {
            wprintf(L"PowerOff: stopped by user.\n");
            snooze_mark(0);
            return;
        }
        if (g_rm_result != RM_SNOOZE) break; /* now, or the countdown ran out */
        wprintf(L"PowerOff: snoozed for 10 minutes.\n");
        snooze_mark(local_now_secs() + RM_SNOOZE_SECS); /* the window shows the new time */
        warn_secs = REMIND_MIN * 60;
        Sleep((RM_SNOOZE_SECS - warn_secs) * 1000);
    }
    snooze_mark(0);
    if (do_task_ex(task, force)) {
        wprintf(L"PowerOff: executed '%ls'.\n", TASK_NAMES[task]);
    } else {
        fwprintf(stderr, L"PowerOff: failed to execute '%ls'.\n", TASK_NAMES[task]);
        exit(3);
    }
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
        L"Turn the display on after a timer wake (used by the wake task):\n"
        L"  PowerOff.exe --wake\n"
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
        ts_delete(WTS_DAILY);
        wprintf(L"PowerOff: Removed '" WTS_DAILY L"' scheduled task.\n");
        return 0;
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
            /* the 60 s reminder card starts a minute early, so the action is at HH:MM */
            wchar_t start[32], trig[256], args[96];
            long long tod = h * 3600LL + m * 60LL - 60;
            if (tod < 0) tod += 86400;
            iso_local(local_now_secs() / 86400 * 86400 + tod, start, 32);
            StringCchPrintfW(trig, 256, L"<CalendarTrigger><StartBoundary>%ls</StartBoundary>"
                L"<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay></CalendarTrigger>", start);
            StringCchPrintfW(args, 96, L"--task %ls --fire-now --warn-secs 60", TASK_CLI[task]);
            if (wts_create(WTS_DAILY, trig, L"", args)) {
                wprintf(L"PowerOff: Scheduled '%ls' daily at %02u:%02u (Task Scheduler: '" WTS_DAILY L"').\n",
                    TASK_NAMES[task], h, m);
                return 0;
            }
            fwprintf(stderr, L"PowerOff: couldn't create the scheduled task.\n");
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
    if (has_flag(argc, argv, L"--wake")) { /* run by the "PowerOff wake" task */
        /* Timer wakes are "unattended": the display stays off and Windows sleeps again
         * after ~2 minutes. Synthetic input marks the wake as attended and lights the screen. */
        SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        INPUT in[2];
        memset(in, 0, sizeof(in));
        in[0].type = in[1].type = INPUT_MOUSE;
        in[0].mi.dwFlags = in[1].mi.dwFlags = MOUSEEVENTF_MOVE;
        in[0].mi.dx = 1;
        in[1].mi.dx = -1;
        SendInput(2, in, sizeof(INPUT));
        return 0;
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
