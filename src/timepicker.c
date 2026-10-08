/* PowerOff: time picker flyout. Part of the unity build: included by poweroff.c, in order. */

/* ------------------------------------------------------------------ time picker
 * WinUI TimePicker flyout as in the design: the 242x32 segmented button opens a
 * 242-wide popup with hour / minute / AM-PM columns of 40 px rows, the selection in
 * an accent band, wheel or click to change, then accept or cancel. Keyboard: Left /
 * Right pick a column, Up / Down change it, Enter accepts, Esc cancels. Geometry is in
 * design pixels (TP_*), scaled with S(). */

#define TP_CLASS L"PowerOffTimePick"
#define TP_W 242
#define TP_ROW 40
#define TP_COLS (7 * TP_ROW)        /* 280 */
#define TP_H (TP_COLS + 41)         /* + divider + footer */
static const int TP_X[4] = { 0, 81, 162, TP_W };

static struct {
    HWND wnd, owner;     /* owner: the button that opened it (keeps keyboard focus) */
    int target;          /* 0 = Time row, 1 = Wake time row */
    int h, m;            /* draft, 24h */
    int hcol, hrow;      /* hovered cell, -1 = none */
    int hbtn;            /* hovered footer button: 1 accept, 2 cancel */
    int kcol;            /* keyboard column, -1 until an arrow key is used */
} TP = { NULL, NULL, 0, 0, 0, -1, -1, 0, -1 };

static void paint_time_btn(HDC hdc, HWND btn, int h, int m) {
    RECT all, rc = ctl_body(btn);
    GetClientRect(btn, &all);
    fill_rect_c(hdc, &all, T_CARD);
    focus_ring(hdc, &rc, 4, T_CARD);
    fill_round(hdc, &rc, 4, btn == G.hover_btn ? T_CTLHOV : T_CTL, T_CTLBRD);
    int x0 = rc.left, top = rc.top, bot = rc.bottom;
    line_c(hdc, x0 + S(81), top + SB(1), x0 + S(81), bot - SB(1) - 1, T_CTLBRD, 1);
    line_c(hdc, x0 + S(162), top + SB(1), x0 + S(162), bot - SB(1) - 1, T_CTLBRD, 1);
    line_c(hdc, x0 + S(3), bot - SB(1) - 1, rc.right - S(4), bot - SB(1) - 1, T_CTLBOT, 1);
    wchar_t a[8], b[8];
    StringCchPrintfW(a, 8, L"%d", h % 12 ? h % 12 : 12);
    StringCchPrintfW(b, 8, L"%02d", m);
    RECT s1 = { x0, top, x0 + S(81), bot }, s2 = { x0 + S(81), top, x0 + S(162), bot },
         s3 = { x0 + S(162), top, rc.right, bot };
    UINT f = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
    text_at(hdc, a, s1, G.f_row, T_T1, f);
    text_at(hdc, b, s2, G.f_row, T_T1, f);
    text_at(hdc, h < 12 ? L"AM" : L"PM", s3, G.f_row, T_T1, f);
}

static void tp_label(int col, int off, wchar_t *out, size_t n) {
    out[0] = L'\0';
    if (col == 0) {
        int v = ((TP.h % 12 + off) % 12 + 12) % 12;
        StringCchPrintfW(out, n, L"%d", v ? v : 12);
    } else if (col == 1) {
        StringCchPrintfW(out, n, L"%02d", ((TP.m + off) % 60 + 60) % 60);
    } else { /* AM/PM: only the other value, on its side */
        bool pm = TP.h >= 12;
        if (off == 0) StringCchCopyW(out, n, pm ? L"PM" : L"AM");
        else if (off == 1 && !pm) StringCchCopyW(out, n, L"PM");
        else if (off == -1 && pm) StringCchCopyW(out, n, L"AM");
    }
}

static void tp_shift(int col, int d) {
    if (col == 0) TP.h = ((TP.h % 12 + d) % 12 + 12) % 12 + (TP.h >= 12 ? 12 : 0);
    else if (col == 1) TP.m = ((TP.m + d) % 60 + 60) % 60;
    else if ((d > 0 && TP.h < 12) || (d < 0 && TP.h >= 12)) TP.h = (TP.h + 12) % 24;
    InvalidateRect(TP.wnd, NULL, FALSE);
}

static void acc_time_btn(HWND btn, int h, int m); /* accessibility value, window.c */

static void tp_close(void) {
    if (!TP.wnd) return;
    HWND w = TP.wnd;
    TP.wnd = NULL; /* first: ReleaseCapture sends WM_CAPTURECHANGED back to us */
    ReleaseCapture();
    DestroyWindow(w);
    acc_sync_all(); /* drop the announced draft time if it wasn't accepted */
}

static void tp_accept(void) {
    int target = TP.target;
    if (target == 0) {
        G.rt.plan.h = TP.h;
        G.rt.plan.m = TP.m;
        save_current();
    } else {
        G.wake_h = TP.h;
        G.wake_m = TP.m;
        G.wake_dirty = true;
    }
    tp_close();
    if (target == 1) wake_flush(); /* re-registers the wake task */
    HWND btn = target == 0 ? G.time_btn : G.wake_btn;
    acc_time_btn(btn, target == 0 ? G.rt.plan.h : G.wake_h, target == 0 ? G.rt.plan.m : G.wake_m);
    InvalidateRect(G.main, NULL, FALSE); /* time-row subtitle */
    InvalidateRect(btn, NULL, FALSE);
}

static void tp_hit(int x, int y, int *col, int *row, int *btn) {
    *col = *row = -1;
    *btn = 0;
    if (x < 0 || x >= S(TP_W) || y < 0 || y >= S(TP_H)) return;
    if (y < S(TP_COLS)) {
        for (int c = 0; c < 3; c++)
            if (x >= S(TP_X[c]) && x < S(TP_X[c + 1])) *col = c;
        *row = y / S(TP_ROW);
    } else if (y > S(TP_COLS + 4) && y < S(TP_H - 4)) {
        *btn = x < S(TP_W) / 2 ? 1 : 2;
    }
}

/* keyboard, forwarded by the owner button (the popup never takes focus) */
static bool tp_key(WPARAM vk) {
    if (!TP.wnd) return false;
    switch (vk) {
    case VK_ESCAPE: tp_close(); return true;
    case VK_RETURN: case VK_SPACE: tp_accept(); return true;
    case VK_LEFT: TP.kcol = TP.kcol <= 0 ? 2 : TP.kcol - 1; break;
    case VK_RIGHT: case VK_TAB: TP.kcol = TP.kcol < 0 || TP.kcol >= 2 ? 0 : TP.kcol + 1; break;
    case VK_UP: if (TP.kcol < 0) TP.kcol = 0; tp_shift(TP.kcol, -1); break;
    case VK_DOWN: if (TP.kcol < 0) TP.kcol = 0; tp_shift(TP.kcol, 1); break;
    default: return false;
    }
    acc_time_btn(TP.owner, TP.h, TP.m); /* announce the draft time */
    InvalidateRect(TP.wnd, NULL, FALSE);
    return true;
}

static LRESULT CALLBACK tp_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        int W = S(TP_W), H = S(TP_H), R = S(TP_ROW), band = 3 * R;
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd, &ps);
        HDC hdc = CreateCompatibleDC(wdc); /* buffered: aa_rrect reads the DC back */
        HBITMAP bm = CreateCompatibleBitmap(wdc, W, H);
        HGDIOBJ ob = SelectObject(hdc, bm);
        RECT all = { 0, 0, W, H };
        fill_rect_c(hdc, &all, T_FLY);
        aa_rrect(hdc, SF(4), (float)band, (float)(W - S(8)), (float)R, SF(4), T_ACCENT, T_ACCENT, 0);
        for (int c = 0; c < 3; c++) {
            int cl = S(TP_X[c]), cr_ = S(TP_X[c + 1]);
            if (c == TP.kcol) /* keyboard column */
                aa_rrect(hdc, (float)cl + SF(2), SF(2), (float)(cr_ - cl) - SF(4), (float)(7 * R) - SF(4), SF(4),
                    T_FLY, T_T1, SF(2));
            for (int r = 0; r < 7; r++) {
                int off = r - 3;
                wchar_t t[8];
                tp_label(c, off, t, 8);
                if (!t[0]) continue;
                RECT cell = { cl, r * R, cr_, r * R + R };
                if (off && c == TP.hcol && r == TP.hrow)
                    aa_rrect(hdc, (float)cl + SF(4), (float)cell.top, (float)(cr_ - cl) - SF(8), (float)R, SF(4),
                        T_SUBTLE, T_SUBTLE, 0);
                text_at(hdc, t, cell, G.f_row, off ? T_T1 : T_ONACC, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        for (int c = 1; c < 3; c++) { /* column dividers, not across the band */
            line_c(hdc, S(TP_X[c]), 0, S(TP_X[c]), band, T_FLYBRD, 1);
            line_c(hdc, S(TP_X[c]), band + R, S(TP_X[c]), 7 * R, T_FLYBRD, 1);
        }
        line_c(hdc, 0, S(TP_COLS), W, S(TP_COLS), T_FLYBRD, 1);
        RECT ok = { S(4), S(TP_COLS + 5), W / 2 - S(2), H - S(4) };
        RECT no = { W / 2 + S(2), S(TP_COLS + 5), W - S(4), H - S(4) };
        if (TP.hbtn == 1) fill_round(hdc, &ok, 4, T_SUBTLE, T_SUBTLE);
        if (TP.hbtn == 2) fill_round(hdc, &no, 4, T_SUBTLE, T_SUBTLE);
        glyph_at(hdc, 0xE73E, ok, 14, T_T1); /* CheckMark */
        glyph_at(hdc, 0xE711, no, 14, T_T1); /* Cancel */
        BitBlt(wdc, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, ob);
        DeleteObject(bm);
        DeleteDC(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        int c, r, b;
        tp_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &c, &r, &b);
        if (c != TP.hcol || r != TP.hrow || b != TP.hbtn) {
            TP.hcol = c; TP.hrow = r; TP.hbtn = b;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int c, r, b, x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (x < 0 || x >= S(TP_W) || y < 0 || y >= S(TP_H)) { tp_close(); return 0; } /* click outside */
        tp_hit(x, y, &c, &r, &b);
        if (b == 1) tp_accept();
        else if (b == 2) tp_close();
        else if (c >= 0 && r != 3) {
            wchar_t t[8];
            tp_label(c, r - 3, t, 8);
            if (t[0]) tp_shift(c, r - 3);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        int c, r, b;
        tp_hit(pt.x, pt.y, &c, &r, &b);
        if (c >= 0) tp_shift(c, GET_WHEEL_DELTA_WPARAM(wp) < 0 ? 1 : -1);
        return 0;
    }
    case WM_KEYDOWN:
        tp_key(wp);
        return 0;
    case WM_CAPTURECHANGED:
        if (TP.wnd == hwnd && (HWND)lp != hwnd) tp_close();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool popup_open(void) { return FLY.wnd || TP.wnd; }
static bool popup_key(WPARAM vk) { return FLY.wnd ? fly_key(vk) : tp_key(vk); }

static void tp_open(HWND btn, int target, int h, int m) {
    tp_close();
    fly_close();
    RECT br = ctl_body(btn);
    MapWindowPoints(btn, NULL, (POINT *)&br, 2);
    int W = S(TP_W), H = S(TP_H);
    int x = br.left, y = (br.top + br.bottom) / 2 - (3 * S(TP_ROW) + S(TP_ROW) / 2); /* band on the button */
    HMONITOR mon = MonitorFromWindow(btn, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (y + H > mi.rcWork.bottom) y = mi.rcWork.bottom - H;
    if (x + W > mi.rcWork.right) x = mi.rcWork.right - W;
    TP.owner = btn;
    TP.target = target;
    TP.h = h;
    TP.m = m;
    TP.hcol = TP.hrow = -1;
    TP.hbtn = 0;
    TP.kcol = -1;
    TP.wnd = CreateWindowExW(WS_EX_TOOLWINDOW, TP_CLASS, NULL, WS_POPUP, x, y, W, H,
        G.main, NULL, GetModuleHandleW(NULL), NULL);
    if (!TP.wnd) return;
    DWORD corner = 2; /* DWMWCP_ROUND: rounded, shadowed, system border */
    DwmSetWindowAttribute(TP.wnd, 33, &corner, sizeof(corner));
    ShowWindow(TP.wnd, SW_SHOWNOACTIVATE);
    SetCapture(TP.wnd);
}
