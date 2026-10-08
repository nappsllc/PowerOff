/* PowerOff: dropdown flyout. Part of the unity build: included by poweroff.c, in order. */

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
} FLY = { .pick = -1 }; /* -1: no pending pick (0 would read as "first item picked") */

static void acc_sync_all(void); /* window.c */

static void fly_close(void) {
    if (FLY.wnd) {
        HWND w = FLY.wnd;
        FLY.wnd = NULL;
        ReleaseCapture();
        DestroyWindow(w);
        acc_sync_all(); /* drop the announced arrow-key value if nothing was picked */
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
    /* design: 208 wide (or the control's width + 8), 2 px padding, 36 px items, opened
     * over the control so the selected item sits on it: left -4, top -(sel*36 + 6) */
    int ih = S(36), w = btn_rc->right - btn_rc->left + S(8);
    if (w < S(208)) w = S(208);
    int h = count * ih + S(4);
    POINT pt = { btn_rc->left - S(4), btn_rc->top - (sel * ih + S(6)) };
    ClientToScreen(owner, &pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    if (pt.y + h > mi.rcWork.bottom) pt.y = mi.rcWork.bottom - h;
    if (pt.y < mi.rcWork.top) pt.y = mi.rcWork.top;
    if (pt.x + w > mi.rcWork.right) pt.x = mi.rcWork.right - w;
    FLY.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, FLY_CLASS, NULL,
        WS_POPUP, pt.x, pt.y, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!FLY.wnd) return;
    /* let DWM round it (anti-aliased, with the system border); a window region
     * would clip hard and stair-step. Windows 10 ignores this: square corners. */
    DWORD corner = 2; /* DWMWCP_ROUND */
    DwmSetWindowAttribute(FLY.wnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    ShowWindow(FLY.wnd, SW_SHOWNOACTIVATE);
    SetCapture(FLY.wnd);
}

static void fly_pick(int idx) { /* report like a click: WM_COMMAND with the index in lp */
    FLY.pick = idx;
    int cid = FLY.combo_id;
    HWND owner = FLY.owner;
    fly_close();
    PostMessageW(owner, WM_COMMAND, MAKEWPARAM(cid, 0), (LPARAM)idx);
}

/* keyboard, forwarded by the combo button (the popup never takes focus) */
static bool fly_key(WPARAM vk) {
    if (!FLY.wnd) return false;
    int cur = FLY.hover >= 0 ? FLY.hover : FLY.sel;
    switch (vk) {
    case VK_ESCAPE: fly_close(); return true;
    case VK_RETURN: case VK_SPACE: fly_pick(cur); return true;
    case VK_UP: FLY.hover = cur > 0 ? cur - 1 : 0; break;
    case VK_DOWN: FLY.hover = cur < FLY.count - 1 ? cur + 1 : FLY.count - 1; break;
    case VK_HOME: FLY.hover = 0; break;
    case VK_END: FLY.hover = FLY.count - 1; break;
    default: return false;
    }
    InvalidateRect(FLY.wnd, NULL, FALSE);
    acc_value(GetDlgItem(FLY.owner, FLY.combo_id), FLY.items[FLY.hover].label); /* announce */
    return true;
}

static LRESULT CALLBACK fly_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC hdc = CreateCompatibleDC(wdc); /* buffered: aa_rrect blends with what's below */
        HBITMAP bm = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(hdc, bm);
        fill_rect_c(hdc, &rc, T_FLY); /* DWM rounds the window and draws its border */
        for (int i = 0; i < FLY.count; i++) {
            RECT ir = { S(4), S(2) + i * S(36), rc.right - S(4), S(2) + (i + 1) * S(36) };
            if (i == FLY.sel || i == FLY.hover)
                fill_round(hdc, &ir, 4, T_SUBTLE, T_SUBTLE);
            if (i == FLY.sel) /* WinUI selection pill */
                aa_rrect(hdc, (float)ir.left, (float)ir.top + SF(10), SF(3), SF(16), SF(1.5f), T_ACCENT, T_ACCENT, 0);
            RECT tr = { ir.left + S(12), ir.top, ir.right - S(12), ir.bottom };
            text_at(hdc, FLY.items[i].label, tr, G.f_row, T_T1,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, ob);
        DeleteObject(bm);
        DeleteDC(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc;
        GetClientRect(hwnd, &rc);
        int idx = (pt.y - S(2)) / S(36);
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
        int idx = (pt.y - S(2)) / S(36);
        if (idx >= 0 && idx < FLY.count && pt.x >= 0 && pt.x <= rc.right) {
            fly_pick(idx);
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
        fly_key(wp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
