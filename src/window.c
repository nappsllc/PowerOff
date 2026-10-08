/* PowerOff: title bar, main window, GUI startup. Part of the unity build: included by poweroff.c, in order. */

/* ------------------------------------------------------------------ main window proc */

static void ensure_tray(void);

/* ------------------------------------------------------------------ title bar
 * The design's 48 px title bar: Windows has no taller standard caption, so the
 * client area extends over it (WM_NCCALCSIZE) and it is drawn here. The strip
 * still hit-tests as HTCAPTION, so dragging, Snap and the system menu work. */

static RECT cap_btn_rect(int i) { /* i = 1 min, 2 max, 3 close; 46x32 at top right */
    RECT cr;
    GetClientRect(G.main, &cr);
    RECT r = { cr.right - S(46) * (4 - i), 0, cr.right - S(46) * (3 - i), S(32) };
    return r;
}

static int cap_btn_at(POINT pt) {
    for (int i = 1; i <= 3; i++) {
        RECT r = cap_btn_rect(i);
        if (PtInRect(&r, pt)) return i;
    }
    return 0;
}

static void invalidate_title(void) {
    RECT cr;
    GetClientRect(G.main, &cr);
    RECT t = { 0, 0, cr.right, S(TITLE_H) };
    InvalidateRect(G.main, &t, FALSE);
}

static void paint_title(HDC hdc) {
    static HICON icon;
    static int icon_px;
    if (icon_px != S(16)) { /* reload at the current DPI */
        if (icon) DestroyIcon(icon);
        icon_px = S(16);
        icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, icon_px, icon_px, 0);
    }
    if (icon) DrawIconEx(hdc, S(16), S(16), icon, icon_px, icon_px, 0, NULL, DI_NORMAL);
    RECT cr;
    GetClientRect(G.main, &cr);
    COLORREF fg = G.inactive ? T_T2 : T_T1;
    RECT tr = { S(16 + 16 + 12), 0, cr.right - S(46 * 3 + 8), S(TITLE_H) };
    text_at(hdc, APP_NAME, tr, G.f_sub, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    static const wchar_t glyph[4] = { 0, 0xE921, 0xE922, 0xE8BB }; /* ChromeMinimize/Maximize/Close */
    for (int i = 1; i <= 3; i++) {
        RECT r = cap_btn_rect(i);
        bool enabled = i != 2; /* fixed-size window: maximize disabled */
        bool hot = enabled && G.cap_hover == i, down = hot && G.cap_down == i;
        COLORREF c = enabled ? fg : mix_c(T_WIN, T_T1, 0.36f);
        if (hot && i == 3) {
            fill_rect_c(hdc, &r, down ? RGB(200, 64, 49) : RGB(196, 43, 28));
            c = RGB(255, 255, 255);
        } else if (hot) {
            fill_rect_c(hdc, &r, down ? mix_c(T_WIN, T_T1, 0.03f) : T_SUBTLE);
        }
        glyph_at(hdc, glyph[i], r, 10, c);
    }
}

static void make_ui_fonts(void) { /* at the current DPI */
    G.f_hero = make_font(L"Segoe UI Variable Text", L"Segoe UI", -20, FW_SEMIBOLD);
    G.f_row = make_font(L"Segoe UI Variable Text", L"Segoe UI", -14, FW_NORMAL);
    G.f_sub = make_font(L"Segoe UI Variable Text", L"Segoe UI", -12, FW_NORMAL);
    G.f_hdr = make_font(L"Segoe UI Variable Text", L"Segoe UI", -14, FW_SEMIBOLD); /* section headers */
}

static void free_ui_fonts(void) {
    HFONT *fonts[] = { &G.f_hero, &G.f_row, &G.f_sub, &G.f_hdr };
    for (int i = 0; i < 4; i++) { if (*fonts[i]) DeleteObject(*fonts[i]); *fonts[i] = NULL; }
}

/* ------------------------------------------------------------------ accessibility state
 * Names, roles and descriptions for the owner-drawn controls, and their current values
 * and checked states. Cheap (a few COM calls), so it simply runs after every change. */

static void acc_time_btn(HWND btn, int h, int m) {
    wchar_t v[16];
    fmt12(h, m, v, 16);
    acc_value(btn, v);
}

static void acc_sync_all(void) {
    if (!G.main || !g_acc) return;
    static const wchar_t *FULLDAYS[] = { L"Monday", L"Tuesday", L"Wednesday", L"Thursday",
        L"Friday", L"Saturday", L"Sunday" };
    wchar_t v[128];
    acc_control(G.combo_action, L"Action", ROLE_SYSTEM_COMBOBOX, L"What happens when the time comes");
    acc_value(G.combo_action, TASK_NAMES[G.rt.plan.task]);
    acc_control(G.combo_mode, L"Schedule", ROLE_SYSTEM_COMBOBOX, L"Choose what triggers the action");
    get_text(G.combo_mode, v, 128); acc_value(G.combo_mode, v);
    acc_control(G.combo_cd, L"Countdown", ROLE_SYSTEM_COMBOBOX, NULL);
    get_text(G.combo_cd, v, 128); acc_value(G.combo_cd, v);
    acc_control(G.combo_idle, L"Idle time", ROLE_SYSTEM_COMBOBOX, NULL);
    get_text(G.combo_idle, v, 128); acc_value(G.combo_idle, v);
    acc_control(G.time_btn, L"Time", ROLE_SYSTEM_COMBOBOX, L"Opens the time picker");
    acc_time_btn(G.time_btn, G.rt.plan.h, G.rt.plan.m);
    acc_control(G.wake_btn, L"Wake time", ROLE_SYSTEM_COMBOBOX, L"Not from shut down: the PC must be asleep");
    acc_time_btn(G.wake_btn, G.wake_h, G.wake_m);
    for (int i = 0; i < 7; i++) {
        acc_control(G.day[i], FULLDAYS[i], ROLE_SYSTEM_CHECKBUTTON, L"Repeat on");
        acc_checked(G.day[i], (G.rt.plan.days >> i) & 1);
    }
    static const struct { HWND *h; const wchar_t *n, *d; bool *on; } T[] = {
        { &G.tg_remind, L"Remind me 1 minute before", L"1 minute countdown before the action, to stop or snooze", &G.remind_on },
        { &G.tg_force, L"Force apps to close", L"Apps with unsaved work won't stop the action", &G.force_on },
        { &G.tg_auto, L"Start with Windows", L"Run in the system tray when you sign in", &G.auto_on },
        { &G.tg_tray, L"Tray icon", L"Off: close exits; Windows runs the task", &G.tray_on },
        { &G.tg_wake, L"Wake up", L"Wake the PC from sleep every day", &G.wake_on },
    };
    for (int i = 0; i < 5; i++) {
        acc_control(*T[i].h, T[i].n, ROLE_SYSTEM_CHECKBUTTON, T[i].d);
        acc_checked(*T[i].h, *T[i].on);
    }
    wchar_t ht[128], hs[128];
    hero_strings(ht, 128, hs, 128);
    StringCchPrintfW(v, 128, L"%ls. %ls", ht, hs);
    acc_desc(G.btn_start, v); /* name is the button text: Start / Cancel */
    acc_control(G.rel_power, LINK_POWER, ROLE_SYSTEM_LINK, L"Opens Windows Settings: sleep, screen timeout, power mode");
    acc_control(G.rel_about, LINK_ABOUT, ROLE_SYSTEM_PUSHBUTTON, APP_NAME L", version " APP_VERSION);
}

static void acc_sync(HWND h) { (void)h; acc_sync_all(); } /* focus moved: refresh states */

/* the plan changed under us (read back from the task): redraw everything */
static void ui_after_plan_change(void) {
    if (!G.main || !G.ready) return;
    update_combo_texts();
    layout(G.main);
    update_views();
    RedrawWindow(G.main, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
    acc_sync_all();
}

/* WM_COMMAND: button clicks and flyout picks */
static LRESULT command(HWND hwnd, WPARAM wp, LPARAM lp) {
    int id = (int)(wp & 0xFFFF);
    int notif = (int)((wp >> 16) & 0xFFFF);
    /* a flyout pick arrives as WM_COMMAND with the item index in lp; a real click
     * carries the button hwnd */
    bool fly_pick = notif == 0 && FLY.pick >= 0 && !IsWindow((HWND)lp);
    if (fly_pick && id == ID_COMBO_ACTION) {
        G.rt.plan.task = (Task)FLY.pick;
        FLY.pick = -1;
        update_combo_texts();
        save_current();
        layout(hwnd); /* Wake up rows appear only for Sleep / Hibernate */
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    if (fly_pick && id == ID_COMBO_MODE) {
        G.rt.plan.mode = GUI_MODES[FLY.pick];
        FLY.pick = -1;
        update_combo_texts();
        layout(hwnd);       /* day row may appear/vanish; window resizes */
        save_current();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    if (fly_pick && id == ID_COMBO_CD) {
        G.rt.plan.cd = CDS[FLY.pick];
        FLY.pick = -1;
        update_combo_texts();
        save_current();
        return 0;
    }
    if (fly_pick && id == ID_COMBO_IDLE) {
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
            InflateRect(&brc, -S(CTL_PAD), -S(CTL_PAD)); /* the drawn control, not its focus padding */
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
        case ID_TIME:
            tp_open(G.time_btn, 0, G.rt.plan.h, G.rt.plan.m);
            return 0;
        case ID_WAKE_TIME:
            tp_open(G.wake_btn, 1, G.wake_h, G.wake_m);
            return 0;
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
            set_startup(G.auto_on); /* not part of the schedule */
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
        case ID_TG_WAKE:
            G.wake_on = !G.wake_on;
            G.wake_dirty = false;
            wake_save();
            if (G.wake_on) {
                if (!wts_wake_register(G.wake_h, G.wake_m)) {
                    G.wake_on = false;
                    wake_save();
                    MessageBoxW(hwnd, L"Couldn't create the wake task in Task Scheduler.",
                        L"PowerOff", MB_OK | MB_ICONWARNING);
                }
            } else {
                wts_wake_delete();
            }
            layout(hwnd); /* the Wake time row appears or goes */
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case ID_REL_POWER:
            ShellExecuteW(NULL, L"open", L"ms-settings:powersleep", NULL, NULL, SW_SHOW);
            return 0;
        case ID_REL_ABOUT:
            about_open();
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
    return 0;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        G.main = hwnd;
        g_dpi = GetDpiForWindow(hwnd);
        { /* testing aid: render at another scale, e.g. POWEROFF_TEST_DPI=144 for 150% */
            wchar_t t[8];
            if (GetEnvironmentVariableW(L"POWEROFF_TEST_DPI", t, 8)) g_dpi = _wtoi(t) >= 96 ? _wtoi(t) : g_dpi;
        }
        theme_dpi_reset();
        make_ui_fonts();
        acc_init();
        DWORD obtn = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW; /* Tab-navigable */

        /* created in reading order: that is the Tab order */
        G.btn_start = mk_child(hwnd, L"BUTTON", L"Start", obtn, ID_START, G.f_row);
        G.combo_action = mk_child(hwnd, L"BUTTON", TASK_NAMES[0], obtn, ID_COMBO_ACTION, G.f_row);
        G.combo_mode = mk_child(hwnd, L"BUTTON", GUI_MODE_NAMES[0], obtn, ID_COMBO_MODE, G.f_row);
        G.combo_cd = mk_child(hwnd, L"BUTTON", L"1 hour", obtn, ID_COMBO_CD, G.f_row);
        G.combo_idle = mk_child(hwnd, L"BUTTON", L"15 minutes", obtn, ID_COMBO_IDLE, G.f_row);
        G.time_btn = mk_child(hwnd, L"BUTTON", L"", obtn, ID_TIME, G.f_row);
        for (int i = 0; i < 7; i++)
            G.day[i] = mk_child(hwnd, L"BUTTON", DAYS[i], obtn, ID_DAY0 + i, G.f_row);
        G.tg_remind = mk_child(hwnd, L"BUTTON", L"", obtn, ID_TG_REMIND, G.f_row);
        G.tg_force = mk_child(hwnd, L"BUTTON", L"", obtn, ID_TG_FORCE, G.f_row);
        G.tg_auto = mk_child(hwnd, L"BUTTON", L"", obtn, ID_TG_AUTO, G.f_row);
        G.tg_tray = mk_child(hwnd, L"BUTTON", L"", obtn, ID_TG_TRAY, G.f_row);
        G.tg_wake = mk_child(hwnd, L"BUTTON", L"", obtn, ID_TG_WAKE, G.f_row);
        G.wake_btn = mk_child(hwnd, L"BUTTON", L"", obtn, ID_WAKE_TIME, G.f_row);
        G.rel_power = mk_child(hwnd, L"BUTTON", L"", obtn, ID_REL_POWER, G.f_row);
        G.rel_about = mk_child(hwnd, L"BUTTON", L"", obtn, ID_REL_ABOUT, G.f_row);

        G.ready = true;
        update_combo_texts();
        layout(hwnd);
        update_views();
        acc_sync_all();
        return 0;
    }
    case WM_DPICHANGED: { /* moved to a monitor with other scaling: rebuild at the new DPI */
        const RECT *sug = (const RECT *)lp;
        g_dpi = HIWORD(wp);
        theme_dpi_reset();
        free_ui_fonts();
        make_ui_fonts();
        fly_close();
        tp_close();
        SetWindowPos(hwnd, NULL, sug->left, sug->top, sug->right - sug->left, sug->bottom - sug->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        layout(hwnd);
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        /* Drawn off-screen into a bitmap the size of the dirty rect (viewport-shifted
         * so coordinates stay in client space), clipped to it, then blitted once:
         * no flicker, and the anti-aliasing only runs over the dirty pixels. */
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd, &ps);
        RECT dirty = ps.rcPaint;
        int dw = dirty.right - dirty.left, dh = dirty.bottom - dirty.top;
        if (dw <= 0 || dh <= 0) { EndPaint(hwnd, &ps); return 0; }
        HDC hdc = CreateCompatibleDC(wdc);
        HBITMAP buf = CreateCompatibleBitmap(wdc, dw, dh);
        HGDIOBJ obuf = SelectObject(hdc, buf);
        SetViewportOrgEx(hdc, -dirty.left, -dirty.top, NULL);
        IntersectClipRect(hdc, dirty.left, dirty.top, dirty.right, dirty.bottom);
        RECT crc;
        GetClientRect(hwnd, &crc);
        fill_rect_c(hdc, &dirty, T_WIN);
        if (G.ready) {
            Plan *p = &G.rt.plan;
            if (dirty.top < S(TITLE_H)) paint_title(hdc);
            /* hero */
            paint_card(hdc, &G.rc_hero);
            int hcy = (G.rc_hero.top + G.rc_hero.bottom) / 2;
            icon_bg(hdc, G.rc_hero.left + S(24), hcy - S(16), S(32), TASK_ICON[p->task], TASK_COLOR[p->task], T_CARD);
            wchar_t ht[128], hs[128];
            hero_strings(ht, 128, hs, 128);
            /* design: 20/28 title, 2 px gap, 14/20 subtitle, centered */
            RECT htr = { G.rc_hero.left + S(24 + 32 + 20), hcy - S(25),
                         G.rc_hero.right - S(20 + 120 + 16), hcy + S(3) };
            text_at(hdc, ht, htr, G.f_hero, T_T1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT hsr = { htr.left, hcy + S(5), htr.right, hcy + S(25) };
            text_at(hdc, hs, hsr, G.f_row, T_T2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            /* section headers: 14 px semibold, as in the design */
            text_at(hdc, L"Schedule", G.rc_sched_hdr, G.f_hdr, T_T1, DT_LEFT | DT_SINGLELINE);
            text_at(hdc, L"Task", G.rc_task_hdr, G.f_hdr, T_T1, DT_LEFT | DT_SINGLELINE);
            text_at(hdc, L"Options", G.rc_opts_hdr, G.f_hdr, T_T1, DT_LEFT | DT_SINGLELINE);
            /* task rows */
            wchar_t sub[128];
            StringCchCopyW(sub, 128, L"What happens when the time comes");
            paint_row(hdc, &G.rc_action, TASK_ICON[p->task],
                L"Action", sub, 216);
            paint_row(hdc, &G.rc_sched, IC_CALCLK, L"Schedule",
                L"Choose what triggers the action", 216);
            time_row_sub(sub, 128);
            paint_row(hdc, &G.rc_time, IC_CLOCK, L"Time", sub, 258);
            /* days row */
            if (p->mode == M_WEEKLY) {
                paint_card(hdc, &G.rc_days);
                wchar_t d[96];
                days_text(p->days, d, 96);
                int dcy = (G.rc_days.top + G.rc_days.bottom) / 2;
                RECT dr = { G.rc_days.left + S(18), dcy - S(18),
                            G.rc_days.right - S(16 + 7 * 44 + 6 * 4 + 18), dcy + S(2) };
                text_at(hdc, L"Repeat on", dr, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE);
                RECT dsr = { dr.left, dcy + S(2), dr.right, dcy + S(18) };
                StringCchPrintfW(sub, 128, L"Repeats %ls", d);
                text_at(hdc, sub, dsr, G.f_sub, T_T2, DT_LEFT | DT_SINGLELINE);
            }
            /* options rows (On/Off labels left of the pill children) */
            static const struct { RECT *rc; IconId ic; const wchar_t *t; bool *on; } OPTS[] = {
                { &G.rc_remind, IC_ALERT, L"Remind me 1 minute before", &G.remind_on },
                { &G.rc_force, IC_APPS, L"Force apps to close", &G.force_on },
                { &G.rc_auto, IC_ROCKET, L"Start with Windows", &G.auto_on },
                { &G.rc_tray, IC_MONITOR, L"Tray icon", &G.tray_on },
                { &G.rc_wake, IC_SUN, L"Wake up", &G.wake_on },
            };
            for (int i = 0; i < (wake_applies() ? 5 : 4); i++) { /* Wake up is last */
                paint_row(hdc, OPTS[i].rc, OPTS[i].ic, OPTS[i].t, NULL, 104); /* v3: single line */
                int ocy = (OPTS[i].rc->top + OPTS[i].rc->bottom) / 2;
                RECT lr = { OPTS[i].rc->right - S(16 + 40 + 12 + 34), ocy - S(10),
                            OPTS[i].rc->right - S(16 + 40 + 12), ocy + S(10) };
                text_at(hdc, *OPTS[i].on ? L"On" : L"Off", lr, G.f_row, T_T1,
                    DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            }
            if (wake_applies() && G.wake_on) {
                paint_row(hdc, &G.rc_wake_time, IC_CLOCK, L"Wake time", NULL, 258);
            }
            /* footer links are owner-drawn buttons (see WM_DRAWITEM) */
        }
        SetViewportOrgEx(hdc, 0, 0, NULL);
        BitBlt(wdc, dirty.left, dirty.top, dw, dh, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, obuf);
        DeleteObject(buf);
        DeleteDC(hdc);
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
        /* painted into a bitmap and blitted once, so a repaint never shows half-drawn */
        DRAWITEMSTRUCT dis = *(DRAWITEMSTRUCT *)lp, *di = &dis;
        HDC screen = di->hDC;
        int bw = di->rcItem.right - di->rcItem.left, bh = di->rcItem.bottom - di->rcItem.top;
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, bw, bh);
        HGDIOBJ obmp = SelectObject(mem, bmp);
        SetViewportOrgEx(mem, -di->rcItem.left, -di->rcItem.top, NULL);
        di->hDC = mem;
        int id = (int)di->CtlID;
        bool pressed = (di->itemState & ODS_SELECTED) != 0;
        /* focus ring only when focus came from the keyboard */
        g_focus_cues = (di->itemState & ODS_FOCUS) && G.kbd_nav;
        if (id == ID_COMBO_ACTION || id == ID_COMBO_MODE || id == ID_COMBO_CD || id == ID_COMBO_IDLE)
            paint_combo_btn(di->hDC, di->hwndItem);
        else if (id == ID_TIME) {
            paint_time_btn(di->hDC, di->hwndItem, G.rt.plan.h, G.rt.plan.m);
        } else if (id == ID_WAKE_TIME) {
            paint_time_btn(di->hDC, di->hwndItem, G.wake_h, G.wake_m);
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
        else if (id == ID_TG_WAKE)
            paint_toggle(di->hDC, di->hwndItem, G.wake_on);
        else if (id == ID_START)
            paint_start_btn(di->hDC, di->hwndItem, G.rt.active);
        else if (id == ID_REL_POWER)
            paint_link(di->hDC, di->hwndItem, LINK_POWER);
        else if (id == ID_REL_ABOUT)
            paint_link(di->hDC, di->hwndItem, LINK_ABOUT);
        (void)pressed;
        SetViewportOrgEx(mem, 0, 0, NULL);
        BitBlt(screen, di->rcItem.left, di->rcItem.top, bw, bh, mem, 0, 0, SRCCOPY);
        SelectObject(mem, obmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        return 0;
    }
    case WM_COMMAND: {
        if (!G.ready) return 0;
        LRESULT r = command(hwnd, wp, lp);
        acc_sync_all(); /* names, values and checked states follow every change */
        return r;
    }
    case WM_MOUSEWHEEL:
        if (TP.wnd) { SendMessageW(TP.wnd, WM_MOUSEWHEEL, wp, lp); return 0; }
        break;
    case WM_NCCALCSIZE:
        if (wp) { /* drop the standard caption: client starts at the window top */
            NCCALCSIZE_PARAMS *pp = (NCCALCSIZE_PARAMS *)lp;
            LONG top = pp->rgrc[0].top;
            LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
            pp->rgrc[0].top = top;
            return r;
        }
        break;
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        if (hit == HTCLIENT) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            if (pt.y < S(TITLE_H) && !cap_btn_at(pt)) return HTCAPTION;
        }
        return hit;
    }
    case WM_ACTIVATE:
        G.inactive = LOWORD(wp) == WA_INACTIVE;
        invalidate_title();
        if (G.inactive) { /* remember the focused control, as dialogs do */
            HWND f = GetFocus();
            if (f && IsChild(hwnd, f)) G.last_focus = f;
            return 0;
        }
        if (G.ready) refresh_from_task(); /* edited in Task Scheduler meanwhile? */
        if (G.last_focus && IsWindow(G.last_focus) && IsWindowVisible(G.last_focus)) {
            SetFocus(G.last_focus); /* back where the keyboard was, not the window itself */
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = cap_btn_at(pt);
        if (h != G.cap_hover) {
            G.cap_hover = h;
            invalidate_title();
        }
        set_hover_row(row_at(pt));
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (G.cap_hover && !G.cap_down) { G.cap_hover = 0; invalidate_title(); }
        hover_from_cursor(); /* left onto a toggle: its row stays hot */
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int b = cap_btn_at(pt);
        if (b && b != 2) { G.cap_down = b; SetCapture(hwnd); invalidate_title(); }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!G.cap_down) return 0;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int b = G.cap_down;
        G.cap_down = 0;
        ReleaseCapture();
        G.cap_hover = cap_btn_at(pt);
        invalidate_title();
        if (cap_btn_at(pt) == b) {
            if (b == 1) ShowWindow(hwnd, SW_MINIMIZE);
            else if (b == 3) PostMessageW(hwnd, WM_CLOSE, 0, 0);
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
        wake_flush();
        bool in_process = G.rt.active && !G.delegated; /* idle mode (or a refused task) needs us */
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
        fly_close();
        tp_close();
        about_close();
        G.hover_btn = NULL;
        G.hover_row = NULL;
        G.last_focus = NULL;
        G.ready = false;
        G.main = NULL;
        free_ui_fonts();
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
    HWND hwnd = CreateWindowExW(0, L"PowerOffWnd", APP_NAME,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, adj.right - adj.left, adj.bottom - adj.top,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return;
    apply_dark_mode(hwnd);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    layout(hwnd); /* height from the custom frame */
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
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); /* small title-bar size is picked from the same resource */
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
    wc.lpfnWndProc = tp_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = TP_CLASS;
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = about_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = ABOUT_CLASS;
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
        G.wake_on = GetPrivateProfileIntW(L"PowerOff", L"wake", 0, path) != 0;
        G.wake_h = GetPrivateProfileIntW(L"PowerOff", L"wake_h", 7, path) % 24;
        G.wake_m = GetPrivateProfileIntW(L"PowerOff", L"wake_m", 0, path) % 60;
    }
    { /* the "PowerOff" task is the schedule: show what it says */
        Runtime r = G.rt;
        if (ts_load_plan(&r)) {
            G.rt = r;
            G.delegated = true;
            G.remind_on = r.plan.remind_min > 0;
            G.force_on = r.plan.force;
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
        /* dialog-style keyboard navigation for the main window: Tab / Shift+Tab move
         * between controls, Space / Enter press them */
        /* focus ring only while navigating by keyboard: a key shows it, a click hides it
         * (Windows' own cue stays on for good after the first key press) */
        bool key = msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN;
        bool click = msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
                     msg.message == WM_NCLBUTTONDOWN;
        if ((key && !G.kbd_nav) || (click && G.kbd_nav)) {
            G.kbd_nav = key;
            HWND f = GetFocus();
            if (f && G.main && IsChild(G.main, f)) InvalidateRect(f, NULL, FALSE);
        }
        if (G.main && IsDialogMessageW(G.main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
