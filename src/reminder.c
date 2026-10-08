/* PowerOff: reminder card. Part of the unity build: included by poweroff.c, in order. */

/* ------------------------------------------------------------------ reminder
 * Shown 1 minute before the action: a topmost card at the bottom right of the work
 * area (like a notification) with a live countdown, a draining progress bar and three
 * buttons: "<Action> now", "Snooze 10 min", "Stop". When the countdown ends with no
 * answer, the action runs. Used by the window (in-process countdown) and by the
 * "--fire-now --warn-secs N" process Task Scheduler starts for a handed-off task.
 * Keyboard: Tab / arrows move, Enter / Space press, Esc stops. */

#define RM_CLASS L"PowerOffReminder"
#define RM_W 440
#define RM_H 188
#define RM_FOOT 116         /* footer band top */
#define RM_SNOOZE_SECS 600  /* "Snooze 10 min" */

enum { RM_PENDING, RM_NOW, RM_SNOOZE, RM_STOP, RM_TIMEOUT };

static struct {
    HWND wnd;
    Task task;
    ULONGLONG deadline, span;  /* GetTickCount64 ms */
    int hover, focus;          /* 1 now, 2 snooze, 3 stop */
    bool keys;                 /* keyboard used: show the focus ring */
    HFONT f_title, f_text;
    void (*done)(int result);
} RM;

static const wchar_t *RM_WHAT[] = { /* index-aligned with Task */
    L"PowerOff will shut down the PC when the countdown ends.",
    L"PowerOff will restart the PC when the countdown ends.",
    L"PowerOff will put the PC to sleep when the countdown ends.",
    L"PowerOff will hibernate the PC when the countdown ends.",
    L"PowerOff will sign you out when the countdown ends.",
    L"PowerOff will lock the PC when the countdown ends.",
    L"PowerOff will turn off the display when the countdown ends.",
};

static RECT rm_btn(int i) { /* primary 152 wide, the others 112, 8 apart, 24 margins */
    static const int X0[4] = { 0, 24, 184, 304 }, X1[4] = { 0, 176, 296, 416 };
    RECT r = { S(X0[i]), S(136), S(X1[i]), S(168) };
    return r;
}

static int rm_hit(int x, int y) {
    POINT pt = { x, y };
    for (int i = 1; i <= 3; i++) {
        RECT r = rm_btn(i);
        if (PtInRect(&r, pt)) return i;
    }
    return 0;
}

static void rm_finish(int result) {
    if (!RM.wnd) return;
    HWND w = RM.wnd;
    void (*done)(int) = RM.done;
    RM.wnd = NULL;
    KillTimer(w, 1);
    DestroyWindow(w);
    if (RM.f_title) { DeleteObject(RM.f_title); RM.f_title = NULL; }
    if (RM.f_text) { DeleteObject(RM.f_text); RM.f_text = NULL; }
    if (done) done(result);
}

static LRESULT CALLBACK rm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        int W = S(RM_W), H = S(RM_H);
        ULONGLONG now = GetTickCount64();
        LONGLONG left = (LONGLONG)(RM.deadline - now);
        if (left < 0) left = 0;
        int secs = (int)((left + 999) / 1000);
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd, &ps);
        HDC hdc = CreateCompatibleDC(wdc); /* buffered: aa_rrect reads the DC back */
        HBITMAP bm = CreateCompatibleBitmap(wdc, W, H);
        HGDIOBJ ob = SelectObject(hdc, bm);
        RECT all = { 0, 0, W, H }, foot = { 0, S(RM_FOOT), W, H };
        fill_rect_c(hdc, &all, T_CARD);
        fill_rect_c(hdc, &foot, T_WIN);
        line_c(hdc, 0, S(RM_FOOT), W, S(RM_FOOT), T_CARDBRD, 1);
        icon_bg(hdc, S(24), S(24), S(32), TASK_ICON[RM.task], TASK_COLOR[RM.task], T_CARD);
        wchar_t t[96];
        StringCchPrintfW(t, 96, L"%ls in %d:%02d", TASK_NAMES[RM.task], secs / 60, secs % 60);
        RECT tr = { S(68), S(24), W - S(24), S(56) };
        text_at(hdc, t, tr, RM.f_title, T_T1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT dr = { S(24), S(64), W - S(24), S(84) };
        text_at(hdc, RM_WHAT[RM.task], dr, RM.f_text, T_T2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        /* WinUI ProgressBar: 1 px track, 3 px rounded fill, draining */
        float px = SF(24), pw = (float)(W - S(48)), py = SF(96);
        line_c(hdc, S(24), S(97), W - S(24), S(97), T_CTLBRD, 1);
        float frac = RM.span ? (float)left / (float)RM.span : 0;
        if (frac > 0) aa_rrect(hdc, px, py, pw * frac, SF(3), SF(1.5f), T_ACCENT, T_ACCENT, 0);
        static const wchar_t *LBL[4] = { 0, NULL, L"Snooze 10 min", L"Stop" };
        for (int i = 1; i <= 3; i++) {
            RECT r = rm_btn(i);
            wchar_t now_lbl[48];
            const wchar_t *lbl = LBL[i];
            if (i == 1) { StringCchPrintfW(now_lbl, 48, L"%ls now", TASK_NAMES[RM.task]); lbl = now_lbl; }
            g_focus_cues = RM.keys && RM.focus == i;
            focus_ring(hdc, &r, 4, T_WIN);
            if (i == 1) {
                fill_round(hdc, &r, 4, RM.hover == 1 ? T_ACCHOV : T_ACCENT, T_ACCENT);
                text_at(hdc, lbl, r, RM.f_text, T_ONACC, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else {
                fill_round(hdc, &r, 4, RM.hover == i ? T_CTLHOV : T_CTL, T_CTLBRD);
                text_at(hdc, lbl, r, RM.f_text, T_T1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        BitBlt(wdc, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, ob);
        DeleteObject(bm);
        DeleteDC(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (GetTickCount64() >= RM.deadline) rm_finish(RM_TIMEOUT);
        else InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        int h = rm_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != RM.hover) { RM.hover = h; InvalidateRect(hwnd, NULL, FALSE); }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (RM.hover) { RM.hover = 0; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        int b = rm_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (b) rm_finish(b == 1 ? RM_NOW : b == 2 ? RM_SNOOZE : RM_STOP);
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: rm_finish(RM_STOP); return 0;
        case VK_RETURN: case VK_SPACE:
            rm_finish(RM.focus == 1 ? RM_NOW : RM.focus == 2 ? RM_SNOOZE : RM_STOP);
            return 0;
        case VK_TAB: case VK_RIGHT: case VK_DOWN:
            RM.focus = RM.focus % 3 + 1;
            break;
        case VK_LEFT: case VK_UP:
            RM.focus = RM.focus == 1 ? 3 : RM.focus - 1;
            break;
        default:
            return 0;
        }
        RM.keys = true;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_CLOSE:
        rm_finish(RM_STOP);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Show the reminder for `task`, `secs` before it runs; `done` gets the RM_* result. */
static void reminder_show(Task task, unsigned secs, void (*done)(int result));

/* in-app countdown: the answer becomes a signal for tick() */
static void rm_done_app(int result) {
    if (result == RM_NOW) InterlockedExchange(&g_fire_now, 1);
    else if (result == RM_SNOOZE) InterlockedExchange(&g_delay_secs, RM_SNOOZE_SECS);
    else if (result == RM_STOP) InterlockedExchange(&g_cancel_req, 1);
    /* RM_TIMEOUT: nothing, tick() runs the action at the time */
}

static void spawn_reminder(Task task, long long secs) {
    reminder_show(task, (unsigned)secs, rm_done_app);
}

static void reminder_show(Task task, unsigned secs, void (*done)(int result)) {
    static bool registered;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = rm_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = RM_CLASS;
        RegisterClassW(&wc);
        registered = true;
    }
    if (RM.wnd) rm_finish(RM_PENDING); /* replace an older one */
    if (!G.main) { /* no window to take the DPI and theme from (tray or --fire-now) */
        g_dpi = GetDpiForSystem();
        read_theme();
        theme_tokens();
    }
    acc_init();
    RM.task = task;
    RM.span = (ULONGLONG)secs * 1000;
    RM.deadline = GetTickCount64() + RM.span;
    RM.hover = 0;
    RM.focus = 2; /* Snooze: the safe default for an accidental Enter */
    RM.keys = false;
    RM.done = done;
    RM.f_title = make_font(L"Segoe UI Variable Text", L"Segoe UI", -20, FW_SEMIBOLD);
    RM.f_text = make_font(L"Segoe UI Variable Text", L"Segoe UI", -14, FW_NORMAL);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(G.main, MONITOR_DEFAULTTOPRIMARY), &mi);
    int W = S(RM_W), H = S(RM_H);
    RM.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, RM_CLASS, L"PowerOff reminder", WS_POPUP,
        mi.rcWork.right - W - S(12), mi.rcWork.bottom - H - S(12), W, H, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!RM.wnd) return;
    DWORD corner = 2; /* DWMWCP_ROUND: rounded, shadowed, system border */
    DwmSetWindowAttribute(RM.wnd, 33, &corner, sizeof(corner));
    wchar_t name[96], desc[200];
    StringCchPrintfW(name, 96, L"%ls in %u seconds", TASK_NAMES[task], secs);
    StringCchPrintfW(desc, 200, L"%ls Buttons: %ls now, Snooze 10 minutes, Stop. Escape stops.",
        RM_WHAT[task], TASK_NAMES[task]);
    acc_control(RM.wnd, name, ROLE_SYSTEM_ALERT, desc);
    SetTimer(RM.wnd, 1, 250, NULL);
    ShowWindow(RM.wnd, SW_SHOW);
    SetForegroundWindow(RM.wnd); /* may be refused for a background process; it is topmost anyway */
    MessageBeep(MB_ICONWARNING);
    NotifyWinEvent(EVENT_SYSTEM_ALERT, RM.wnd, OBJID_CLIENT, CHILDID_SELF); /* screen readers speak it */
}
