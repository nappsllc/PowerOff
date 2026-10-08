/* PowerOff: control helpers, hover tracking, layout. Part of the unity build: included by poweroff.c, in order. */

/* ------------------------------------------------------------------ children helpers */

/* ------------------------------------------------------------------ hover
 * Design hover fills: Options rows and related cards lighten (--cardHover),
 * controls use --ctlHover, accent buttons --accentHover. Windows only tells the
 * control under the mouse, so owner-drawn buttons are subclassed to report it, and
 * a row stays hot while the mouse is over its toggle. */

static const RECT *row_at(POINT pt) {
    const RECT *rows[] = { &G.rc_remind, &G.rc_force, &G.rc_auto, &G.rc_tray, &G.rc_wake };
    for (int i = 0; i < 5; i++)
        if (PtInRect(rows[i], pt)) return rows[i];
    return NULL;
}

static HWND row_toggle(const RECT *r) {
    return r == &G.rc_remind ? G.tg_remind : r == &G.rc_force ? G.tg_force :
           r == &G.rc_auto ? G.tg_auto : r == &G.rc_tray ? G.tg_tray :
           r == &G.rc_wake ? G.tg_wake : NULL;
}

static void set_hover_row(const RECT *r) {
    if (r == G.hover_row || !G.main) return;
    const RECT *old = G.hover_row;
    G.hover_row = r;
    if (old) { InvalidateRect(G.main, old, FALSE); InvalidateRect(row_toggle(old), NULL, FALSE); }
    if (r) { InvalidateRect(G.main, r, FALSE); InvalidateRect(row_toggle(r), NULL, FALSE); }
}

static void hover_from_cursor(void) { /* after a leave: maybe now over a row's toggle */
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(G.main, &pt);
    set_hover_row(row_at(pt));
}

static WNDPROC g_button_proc;
static bool popup_open(void);      /* dropdown or time picker showing (timepicker.c) */
static bool popup_key(WPARAM vk);  /* hand it a key; true if used */
static void acc_sync(HWND h);      /* refresh screen-reader state (window.c) */

static LRESULT CALLBACK button_hover_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEMOVE) {
        if (G.hover_btn != h) {
            HWND old = G.hover_btn;
            G.hover_btn = h;
            if (old) InvalidateRect(old, NULL, FALSE);
            InvalidateRect(h, NULL, FALSE);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
        }
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        MapWindowPoints(h, G.main, &pt, 1);
        set_hover_row(row_at(pt));
    } else if (m == WM_MOUSELEAVE) {
        if (G.hover_btn == h) { G.hover_btn = NULL; InvalidateRect(h, NULL, FALSE); }
        hover_from_cursor();
    } else if (m == WM_GETOBJECT && (LONG)l == OBJID_CLIENT) {
        /* answer MSAA natively, so UI Automation bridges to it (and to our annotations)
         * instead of falling back to a bare "pane" */
        IAccessible *a = NULL;
        if (SUCCEEDED(CreateStdAccessibleObject(h, OBJID_CLIENT, &IID_IAccessible, (void **)&a))) {
            LRESULT r = LresultFromObject(&IID_IAccessible, w, (IUnknown *)a);
            a->lpVtbl->Release(a);
            return r;
        }
    } else if (m == WM_SETFOCUS || m == WM_KILLFOCUS) {
        LRESULT r = CallWindowProcW(g_button_proc, h, m, w, l);
        acc_sync(h); /* the focused state is part of the annotated state */
        return r;
    } else if (m == WM_GETDLGCODE && popup_open()) {
        return DLGC_WANTALLKEYS; /* a popup is open: its owner button takes every key */
    } else if (m == WM_KEYDOWN && popup_open()) {
        if (popup_key(w)) return 0;
    } else if (m == WM_CHAR && popup_open()) {
        return 0; /* swallow the beep for keys handled above */
    }
    return CallWindowProcW(g_button_proc, h, m, w, l);
}

/* Controls are a superclass of BUTTON: the button's own code (clicks, Space, focus,
 * owner draw) under our class name. UI Automation has a built-in handler for the
 * "Button" class that ignores the accessibility annotations and calls owner-drawn
 * buttons "Pane"; for an unknown class it reads MSAA, annotations included. */
#define CTL_CLASS L"PowerOffCtl"

static void ensure_ctl_class(void) {
    static bool done;
    if (done) return;
    WNDCLASSEXW wc = { sizeof(wc) };
    GetClassInfoExW(NULL, L"BUTTON", &wc);
    g_button_proc = wc.lpfnWndProc;
    wc.lpfnWndProc = button_hover_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = CTL_CLASS;
    RegisterClassExW(&wc);
    done = true;
}

static HWND mk_child(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style,
        int id, HFONT font) {
    if (!wcscmp(cls, L"BUTTON")) { ensure_ctl_class(); cls = CTL_CLASS; }
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

/* design geometry, 96-DPI pixels; layout() scales with S() */
#define PAGE_X 40        /* left/right page padding */
#define ROW_H 68        /* standard row height */
#define OPT_H 48        /* compact single-line Options row (v3) */
#define GAP 4           /* inter-card gap */
#define HDR_GAP 28      /* space above section headers */
#define TITLE_H 48      /* design: custom 48 px title bar drawn in the client area */
#define PAGE_W 640      /* client width */

static int g_cy; /* layout cursor, client coords (device px) */

static RECT card_row_h(int w, int h) {
    RECT rc = { S(PAGE_X), g_cy, S(PAGE_X) + w, g_cy + S(h) };
    g_cy += S(h) + S(GAP);
    return rc;
}
static RECT card_row(HWND hwnd, int w) { (void)hwnd; return card_row_h(w, ROW_H); }
static RECT opt_row(int w) { return card_row_h(w, OPT_H); }

static int text_w(HWND hwnd, HFONT f, const wchar_t *t) {
    HDC dc = GetDC(hwnd);
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz = { 0 };
    GetTextExtentPoint32W(dc, t, lstrlenW(t), &sz);
    SelectObject(dc, o);
    ReleaseDC(hwnd, dc);
    return sz.cx;
}

/* Wake up only matters for actions a timer can wake the PC from */
static bool wake_applies(void) {
    return G.rt.plan.task == T_SLEEP || G.rt.plan.task == T_HIBERNATE;
}

/* a section header 28 px below the cursor's gap, then 8 px to its cards */
static RECT header_row(int content) {
    RECT r = { S(PAGE_X), g_cy + S(HDR_GAP - 20), S(PAGE_X) + content, g_cy + S(HDR_GAP) };
    g_cy += S(HDR_GAP + 8);
    return r;
}

/* Controls are child windows, so their keyboard focus ring has to fit inside them:
 * each window is CTL_PAD px larger on every side than the control it draws. */
#define CTL_PAD 3

static void move_ctl(HWND h, int x, int y, int w, int hh) { /* device px, unpadded */
    int p = S(CTL_PAD);
    move(h, x - p, y - p, w + 2 * p, hh + 2 * p);
}

/* right-aligned control slot of a row: w x h design px, 16 px from the right edge */
static void place_right(HWND h, const RECT *row, int w, int hh) {
    move_ctl(h, row->right - S(16) - S(w), (row->top + row->bottom - S(hh)) / 2, S(w), S(hh));
}

/* resize the window so its client area is w x h device px (frame added as it is) */
static void size_client(HWND hwnd, int w, int h) {
    RECT wr, cr;
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    int ww = w + (wr.right - wr.left - cr.right), wh = h + (wr.bottom - wr.top - cr.bottom);
    if (ww != wr.right - wr.left || wh != wr.bottom - wr.top)
        SetWindowPos(hwnd, NULL, 0, 0, ww, wh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void layout(HWND hwnd) {
    RECT crc;
    GetClientRect(hwnd, &crc);
    if (crc.right != S(PAGE_W)) { /* width follows the DPI */
        size_client(hwnd, S(PAGE_W), crc.bottom);
        GetClientRect(hwnd, &crc);
    }
    int w = crc.right; /* client width */
    int px = S(PAGE_X), content = w - px * 2;

    g_cy = S(TITLE_H + 8);
    G.rc_sched_hdr = (RECT){ px, g_cy, px + content, g_cy + S(20) };
    g_cy += S(20 + 8);

    G.rc_hero = (RECT){ px, g_cy, px + content, g_cy + S(96) };
    g_cy += S(96 + GAP);

    G.rc_task_hdr = header_row(content);
    G.rc_action = card_row(hwnd, content);
    G.rc_sched = card_row(hwnd, content);
    G.rc_time = card_row(hwnd, content);

    bool weekly = G.rt.plan.mode == M_WEEKLY;
    if (weekly) G.rc_days = card_row(hwnd, content);
    else SetRectEmpty(&G.rc_days);

    G.rc_opts_hdr = header_row(content);
    G.rc_remind = opt_row(content);
    G.rc_force = opt_row(content);
    G.rc_auto = opt_row(content);
    G.rc_tray = opt_row(content);
    if (wake_applies()) G.rc_wake = opt_row(content);
    else SetRectEmpty(&G.rc_wake);
    if (wake_applies() && G.wake_on) G.rc_wake_time = opt_row(content);
    else SetRectEmpty(&G.rc_wake_time);

    /* footer links, 20 px below the last card: Power settings left, About right */
    g_cy += S(20 - GAP);
    int lw1 = text_w(hwnd, G.f_row, LINK_POWER), lw2 = text_w(hwnd, G.f_row, LINK_ABOUT);
    G.rc_rel1 = (RECT){ px, g_cy, px + lw1, g_cy + S(20) };
    G.rc_rel2 = (RECT){ px + content - lw2, g_cy, px + content, g_cy + S(20) };
    g_cy += S(20 + 28);                    /* bottom page padding */

    G.client_h = g_cy;

    /* --- position children --- */
    Mode m = G.rt.plan.mode;
    place_right(G.combo_action, &G.rc_action, 200, 32);
    place_right(G.combo_mode, &G.rc_sched, 200, 32);

    /* Time row: one 242-wide slot for the countdown / idle combos or the time picker */
    bool show_seg = (m == M_DAILY || m == M_WEEKLY);
    ShowWindow(G.combo_cd, m == M_COUNTDOWN ? SW_SHOW : SW_HIDE);
    ShowWindow(G.combo_idle, m == M_IDLE ? SW_SHOW : SW_HIDE);
    ShowWindow(G.time_btn, show_seg ? SW_SHOW : SW_HIDE);
    place_right(G.combo_cd, &G.rc_time, 242, 32);
    place_right(G.combo_idle, &G.rc_time, 242, 32);
    place_right(G.time_btn, &G.rc_time, 242, 32);

    /* day buttons: 7 x 44, 4 apart, right-aligned */
    for (int i = 0; i < 7; i++) {
        if (weekly) {
            int bx = G.rc_days.right - S(16) - S(7 * 44 + 6 * 4) + i * S(48);
            move_ctl(G.day[i], bx, (G.rc_days.top + G.rc_days.bottom - S(32)) / 2, S(44), S(32));
        }
        ShowWindow(G.day[i], weekly ? SW_SHOW : SW_HIDE);
    }

    /* toggle pills */
    place_right(G.tg_remind, &G.rc_remind, 40, 20);
    place_right(G.tg_force, &G.rc_force, 40, 20);
    place_right(G.tg_auto, &G.rc_auto, 40, 20);
    place_right(G.tg_tray, &G.rc_tray, 40, 20);
    ShowWindow(G.tg_wake, wake_applies() ? SW_SHOW : SW_HIDE);
    place_right(G.tg_wake, &G.rc_wake, 40, 20);
    ShowWindow(G.wake_btn, wake_applies() && G.wake_on ? SW_SHOW : SW_HIDE);
    place_right(G.wake_btn, &G.rc_wake_time, 242, 32);

    /* hero start button: 120 x 32, 20 px from the right */
    move_ctl(G.btn_start, G.rc_hero.right - S(20 + 120), (G.rc_hero.top + G.rc_hero.bottom - S(32)) / 2,
        S(120), S(32));

    /* footer links: padded like the other controls, so the focus ring fits around the text */
    move_ctl(G.rel_power, G.rc_rel1.left, G.rc_rel1.top, G.rc_rel1.right - G.rc_rel1.left, S(20));
    move_ctl(G.rel_about, G.rc_rel2.left, G.rc_rel2.top, G.rc_rel2.right - G.rc_rel2.left, S(20));

    size_client(hwnd, w, G.client_h); /* height follows the content */
}
