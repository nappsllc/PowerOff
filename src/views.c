/* PowerOff: view strings and row painting. Part of the unity build: included by poweroff.c, in order. */

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

static void wake_save(void) {
    wchar_t path[MAX_PATH], v[16];
    ini_path(path, MAX_PATH);
    WritePrivateProfileStringW(L"PowerOff", L"wake", G.wake_on ? L"1" : L"0", path);
    StringCchPrintfW(v, 16, L"%d", G.wake_h);
    WritePrivateProfileStringW(L"PowerOff", L"wake_h", v, path);
    StringCchPrintfW(v, 16, L"%d", G.wake_m);
    WritePrivateProfileStringW(L"PowerOff", L"wake_m", v, path);
}

/* re-register the wake task after the time changed */
static void wake_flush(void) {
    if (!G.wake_dirty) return;
    G.wake_dirty = false;
    wake_save();
    if (G.wake_on && !wts_wake_register(G.wake_h, G.wake_m))
        MessageBoxW(G.main, L"Couldn't create the wake task in Task Scheduler.", L"PowerOff", MB_OK | MB_ICONWARNING);
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
    /* Called every second while counting down: repaint only what can change. */
    const wchar_t *bt = r->active ? L"Cancel" : L"Start";
    wchar_t cur[16];
    GetWindowTextW(G.btn_start, cur, 16);
    if (wcscmp(cur, bt)) {                          /* only when the task starts or stops */
        set_text(G.btn_start, bt);
        InvalidateRect(G.btn_start, NULL, FALSE);  /* armed/unarmed style */
    }
    InvalidateRect(G.main, &G.rc_hero, FALSE);      /* countdown */
    static wchar_t last_sub[128];
    wchar_t sub[128];
    time_row_sub(sub, 128);
    if (wcscmp(sub, last_sub)) {                    /* "Next: ..." */
        StringCchCopyW(last_sub, 128, sub);
        InvalidateRect(G.main, &G.rc_time, FALSE);
    }
}

/* Re-read the "PowerOff" task: it is the schedule (a task edited or deleted in Task
 * Scheduler, a run that happened, a snooze from the reminder card all show up here). */
static void ui_after_plan_change(void); /* window.c */

static void refresh_from_task(void) {
    if (G.rt.active && !G.delegated) return; /* in-process (idle) schedule: ours */
    Runtime r = G.rt;
    if (ts_load_plan(&r)) {
        G.rt = r;
        G.delegated = true;
        G.remind_on = r.plan.remind_min > 0;
        G.force_on = r.plan.force;
        wchar_t path[MAX_PATH];
        ini_path(path, MAX_PATH);
        G.snooze_until = GetPrivateProfileIntW(L"PowerOff", L"snooze_until", 0, path);
        if (G.snooze_until <= local_now_secs()) G.snooze_until = 0;
    } else if (G.delegated) { /* gone: deleted outside, or a countdown that ran */
        G.rt.active = false;
        G.delegated = false;
        G.snooze_until = 0;
    }
    set_tray_tip();
    ui_after_plan_change();
}

static void spawn_reminder(Task task, long long secs); /* reminder.c */

static void tick(void) {
    Runtime *r = &G.rt;
    rt_apply_signals(r);
    if (G.delegated) { /* Windows runs it: just follow the task */
        static int since;
        if (rt_remaining(r) <= 0 || ++since >= 60) { since = 0; refresh_from_task(); }
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
            spawn_reminder(r->plan.task, rem);
        }
    }
    update_views();
}

/* Start, or an edit while scheduled: (re)compute the timing and put it in the
 * "PowerOff" task; idle mode, or a refused registration, runs in-process instead. */
static const wchar_t *schedule(void) {
    static Mode last_mode;
    static int last_cd;
    Runtime *r = &G.rt;
    bool running = r->active;
    long long fire_at = r->fire_at, span = r->span;
    wchar_t desc[256];
    const wchar_t *err = rt_start(r, desc, 256);
    if (err) return err;
    /* an edit that isn't about the countdown itself (action, reminder, force) keeps its end */
    if (running && r->plan.mode == M_COUNTDOWN && last_mode == M_COUNTDOWN && last_cd == r->plan.cd &&
            fire_at > local_now_secs()) {
        r->fire_at = fire_at;
        r->span = span;
    }
    last_mode = r->plan.mode;
    last_cd = r->plan.cd;
    G.snooze_until = 0;
    if (wts_schedulable(r->plan.mode) && wts_register(r)) {
        G.delegated = true;
    } else {
        if (G.delegated) wts_delete();
        G.delegated = false;
    }
    set_tray_tip();
    return NULL;
}

static void save_current(void) {
    Plan *p = &G.rt.plan;
    p->remind_min = G.remind_on ? REMIND_MIN : 0;
    p->force = G.force_on;
    save_config(p);
    if (G.rt.active) schedule(); /* an edit while scheduled updates the task */
}

static void toggle_start(void) {
    Runtime *r = &G.rt;
    if (r->active) {
        r->active = false;
        if (G.delegated) { wts_delete(); G.delegated = false; }
        G.snooze_until = 0;
        set_tray_tip();
        update_views();
        return;
    }
    save_current();
    const wchar_t *err = schedule();
    if (err) MessageBoxW(G.main, err, L"PowerOff", MB_OK | MB_ICONWARNING);
    update_views();
}

/* ------------------------------------------------------------------ row painting */

static void paint_card(HDC hdc, const RECT *rc) {
    fill_round(hdc, rc, 4, rc == G.hover_row ? T_CARDHOV : T_CARD, T_CARDBRD);
}

/* WinUI keyboard focus visual: 2 px outline in the text color, 1 px outside the
 * control with a corner radius 2 px larger. Drawn only when focus came from the
 * keyboard (Windows hides it after mouse use; see ODS_NOFOCUSRECT). */
static bool g_focus_cues; /* set per WM_DRAWITEM */

static void focus_ring(HDC hdc, const RECT *rc, int rad, COLORREF under) {
    if (!g_focus_cues) return;
    float b = SF(2.0f), g = SF(1.0f);
    aa_rrect(hdc, (float)rc->left - g - b, (float)rc->top - g - b,
        (float)(rc->right - rc->left) + 2 * (g + b), (float)(rc->bottom - rc->top) + 2 * (g + b),
        SF((float)rad + 3), under, T_T1, b);
}

static void paint_row(HDC hdc, const RECT *rc, IconId ic,
        const wchar_t *title, const wchar_t *sub, int ctl_w) { /* ctl_w: right control + gap, design px */
    paint_card(hdc, rc);
    int cy = (rc->top + rc->bottom) / 2;
    icon_bg(hdc, rc->left + S(18), cy - S(10), S(20), ic, T_T1, T_CARD);
    if (!sub) { /* v3 Options row: one 14/20 line, 16 px after the icon */
        RECT tr = { rc->left + S(18 + 20 + 16), cy - S(10), rc->right - S(16 + ctl_w), cy + S(10) };
        text_at(hdc, title, tr, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        return;
    }
    /* design: 14/20 title directly over a 12/16 subtitle, block centered in the row */
    RECT tr = { rc->left + S(18 + 20 + 18), cy - S(18), rc->right - S(16 + ctl_w), cy + S(2) };
    text_at(hdc, title, tr, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sr = { tr.left, cy + S(2), tr.right, cy + S(18) };
    text_at(hdc, sub, sr, G.f_sub, T_T2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static RECT ctl_body(HWND btn) { /* the drawn control inside its padded window */
    RECT rc;
    GetClientRect(btn, &rc);
    InflateRect(&rc, -S(CTL_PAD), -S(CTL_PAD));
    return rc;
}

static void paint_toggle(HDC hdc, HWND btn, bool on) {
    RECT all, rc = ctl_body(btn);
    GetClientRect(btn, &all);
    /* toggles sit on an Options row card, which lightens on hover */
    COLORREF under = G.hover_row && row_toggle(G.hover_row) == btn ? T_CARDHOV : T_CARD;
    fill_rect_c(hdc, &all, under);
    focus_ring(hdc, &rc, 10, under);
    /* WinUI ToggleSwitch: 40x20 capsule (radius 10), 12 px knob inset 4 px */
    float x = (float)rc.left, y = (float)rc.top;
    if (on) {
        aa_rrect(hdc, x, y, SF(40), SF(20), SF(10), T_ACCENT, T_ACCENT, 0);
        aa_rrect(hdc, x + SF(24), y + SF(4), SF(12), SF(12), SF(6), T_ONACC, T_ONACC, 0);
    } else {
        aa_rrect(hdc, x, y, SF(40), SF(20), SF(10), T_TOGFILL, T_TOGBRD, (float)SB(1));
        aa_rrect(hdc, x + SF(4), y + SF(4), SF(12), SF(12), SF(6), T_TOGBRD, T_TOGBRD, 0);
    }
}

static void paint_combo_btn(HDC hdc, HWND btn) {
    RECT all, rc = ctl_body(btn);
    GetClientRect(btn, &all);
    fill_rect_c(hdc, &all, T_CARD);
    focus_ring(hdc, &rc, 4, T_CARD);
    fill_round(hdc, &rc, 4, btn == G.hover_btn ? T_CTLHOV : T_CTL, T_CTLBRD);
    line_c(hdc, rc.left + S(3), rc.bottom - SB(1) - 1, rc.right - S(4), rc.bottom - SB(1) - 1, T_CTLBOT, 1);
    wchar_t txt[96];
    GetWindowTextW(btn, txt, 96);
    RECT tr = { rc.left + S(11), rc.top, rc.right - S(26), rc.bottom };
    text_at(hdc, txt, tr, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT cr = { rc.right - S(24), rc.top, rc.right, rc.bottom };
    chevron_down(hdc, &cr, T_T2);
}

static void paint_day_btn(HDC hdc, HWND btn, bool on) {
    RECT all, rc = ctl_body(btn);
    GetClientRect(btn, &all);
    fill_rect_c(hdc, &all, T_CARD);
    focus_ring(hdc, &rc, 4, T_CARD);
    bool hot = btn == G.hover_btn;
    wchar_t d[8];
    GetWindowTextW(btn, d, 8);
    if (on) {
        fill_round(hdc, &rc, 4, hot ? T_ACCHOV : T_ACCENT, T_CTLBOT);
        text_at(hdc, d, rc, G.f_row, T_ONACC, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    } else {
        fill_round(hdc, &rc, 4, hot ? T_CTLHOV : T_CTL, T_CTLBRD);
        line_c(hdc, rc.left + S(3), rc.bottom - SB(1) - 1, rc.right - S(4), rc.bottom - SB(1) - 1, T_CTLBOT, 1);
        text_at(hdc, d, rc, G.f_row, T_T1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
}

static void paint_start_btn(HDC hdc, HWND btn, bool armed) {
    RECT all, rc = ctl_body(btn);
    GetClientRect(btn, &all);
    fill_rect_c(hdc, &all, T_CARD);
    focus_ring(hdc, &rc, 4, T_CARD);
    wchar_t t[32];
    GetWindowTextW(btn, t, 32);
    bool hot = btn == G.hover_btn;
    if (armed) {
        fill_round(hdc, &rc, 4, hot ? T_CTLHOV : T_CARD, T_CTLBRD);
        text_at(hdc, t, rc, G.f_hdr, T_T1, DT_CENTER | DT_SINGLELINE | DT_VCENTER); /* semibold, as the design reads */
    } else {
        fill_round(hdc, &rc, 4, hot ? T_ACCHOV : T_ACCENT, T_CTLBOT);
        text_at(hdc, t, rc, G.f_hdr, T_ONACC, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
}

/* footer text link on the page: accent text, lighter accent on hover */
static void paint_link(HDC hdc, HWND btn, const wchar_t *text) {
    RECT all, rc = ctl_body(btn);
    GetClientRect(btn, &all);
    fill_rect_c(hdc, &all, T_WIN);
    focus_ring(hdc, &rc, 2, T_WIN);
    text_at(hdc, text, rc, G.f_row, btn == G.hover_btn ? T_ACCHOV : T_ACCENT,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOCLIP);
}
