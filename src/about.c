/* PowerOff: About dialog. Part of the unity build: included by poweroff.c, in order. */

/* ------------------------------------------------------------------ about
 * ContentDialog-style box: icon + name, version, description, "Contact us", a link to
 * more apps, copyright, and a footer band with "Check for updates" and an accent "OK".
 * Modal to the window. Keyboard: Tab / arrows move between the controls, Enter / Space
 * press, Esc closes. Geometry in design pixels, scaled with S(). */

#define ABOUT_CLASS L"PowerOffAbout"
#define APP_VERSION L"1.0.0"
#define RELEASES_URL L"https://github.com/nappsllc/PowerOff/releases/latest"
#define CONTACT_URL L"mailto:hi@powernapps.net?subject=PowerNapps%20PowerOff"
#define SITE_URL L"https://powernapps.net"
#define AB_DESC L"A tiny app that shuts down, restarts or sleeps your PC on a schedule. " \
    L"Windows Task Scheduler does the work, so it uses no memory once closed."
#define AB_LINK L"More cool apps at powernapps.net"
#define AB_W 400
#define AB_H 396
#define AB_FOOT 316  /* footer band top */
enum { AB_CONTACT = 1, AB_SITE, AB_UPDATES, AB_OK, AB_N = AB_OK };
static const wchar_t *AB_NAMES[] = { NULL, L"Contact us button", L"More cool apps at powernapps.net link",
    L"Check for updates button", L"OK button" };
static struct { HWND wnd; int hover, focus; } AB;

static RECT ab_btn(int i) {
    RECT r = { 0 };
    switch (i) {
    case AB_CONTACT: r = (RECT){ S(24), S(200), S(144), S(232) }; break;
    case AB_SITE: { /* the link is as wide as its text */
        HDC dc = GetDC(AB.wnd);
        HGDIOBJ o = SelectObject(dc, G.f_row);
        SIZE sz = { 0 };
        GetTextExtentPoint32W(dc, AB_LINK, lstrlenW(AB_LINK), &sz);
        SelectObject(dc, o);
        ReleaseDC(AB.wnd, dc);
        r = (RECT){ S(24), S(248), S(24) + sz.cx, S(268) };
        break;
    }
    case AB_UPDATES: r = (RECT){ S(24), S(340), S(192), S(372) }; break;
    case AB_OK: r = (RECT){ S(208), S(340), S(376), S(372) }; break;
    }
    return r;
}

static void about_close(void) {
    if (!AB.wnd) return;
    HWND w = AB.wnd;
    AB.wnd = NULL;
    EnableWindow(G.main, TRUE); /* before destroying, so activation returns to the window */
    DestroyWindow(w);
}

static int about_hit(int x, int y) {
    POINT pt = { x, y };
    for (int i = 1; i <= AB_N; i++) {
        RECT r = ab_btn(i);
        if (PtInRect(&r, pt)) return i;
    }
    return 0;
}

static void about_press(int i) {
    static const wchar_t *URL[] = { NULL, CONTACT_URL, SITE_URL, RELEASES_URL };
    if (i == AB_OK) about_close();
    else if (i) ShellExecuteW(NULL, L"open", URL[i], NULL, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK about_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        int W = S(AB_W), H = S(AB_H), ic = S(32);
        HICON icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, ic, ic, 0);
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd, &ps);
        HDC hdc = CreateCompatibleDC(wdc);
        HBITMAP bm = CreateCompatibleBitmap(wdc, W, H);
        HGDIOBJ ob = SelectObject(hdc, bm);
        RECT all = { 0, 0, W, H }, foot = { 0, S(AB_FOOT), W, H };
        fill_rect_c(hdc, &all, T_CARD);
        fill_rect_c(hdc, &foot, T_WIN);
        line_c(hdc, 0, S(AB_FOOT), W, S(AB_FOOT), T_CARDBRD, 1);
        if (icon) { DrawIconEx(hdc, S(24), S(24), icon, ic, ic, 0, NULL, DI_NORMAL); DestroyIcon(icon); }
        RECT tr = { S(68), S(24), W - S(24), S(56) };
        text_at(hdc, APP_NAME, tr, G.f_hero, T_T1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT ver = { S(24), S(72), W - S(24), S(92) }, desc = { S(24), S(100), W - S(24), S(160) },
             ask = { S(24), S(172), W - S(24), S(192) }, copy = { S(24), S(276), W - S(24), S(296) };
        text_at(hdc, L"Version " APP_VERSION, ver, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE);
        text_at(hdc, AB_DESC, desc, G.f_row, T_T2, DT_LEFT | DT_WORDBREAK);
        text_at(hdc, L"Questions, concerns or a gig to offer?", ask, G.f_row, T_T1, DT_LEFT | DT_SINGLELINE);
        text_at(hdc, L"\x00A9 2026 PowerNapps (by Denis Platonov), Los Angeles", copy, G.f_row, T_T2, DT_LEFT | DT_SINGLELINE);
        RECT con = ab_btn(AB_CONTACT), site = ab_btn(AB_SITE), upd = ab_btn(AB_UPDATES), ok = ab_btn(AB_OK);
        g_focus_cues = true; /* keyboard-only dialog: the focused control always shows it */
        RECT f = ab_btn(AB.focus);
        focus_ring(hdc, &f, AB.focus == AB_SITE ? 2 : 4, AB.focus >= AB_UPDATES ? T_WIN : T_CARD);
        fill_round(hdc, &con, 4, AB.hover == AB_CONTACT ? T_CTLHOV : T_CTL, T_CTLBRD);
        text_at(hdc, L"Contact us", con, G.f_row, T_T1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text_at(hdc, AB_LINK, site, G.f_row, AB.hover == AB_SITE ? T_ACCHOV : T_ACCENT,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        fill_round(hdc, &upd, 4, AB.hover == AB_UPDATES ? T_CTLHOV : T_CTL, T_CTLBRD);
        text_at(hdc, L"Check for updates", upd, G.f_row, T_T1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        fill_round(hdc, &ok, 4, AB.hover == AB_OK ? T_ACCHOV : T_ACCENT, T_ACCENT);
        text_at(hdc, L"OK", ok, G.f_row, T_ONACC, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        BitBlt(wdc, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, ob);
        DeleteObject(bm);
        DeleteDC(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR: /* the hand over the link, as on a web page */
        if (AB.hover == AB_SITE) { SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE; }
        break;
    case WM_MOUSEMOVE: {
        int h = about_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != AB.hover) { AB.hover = h; InvalidateRect(hwnd, NULL, FALSE); }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (AB.hover) { AB.hover = 0; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        about_press(about_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: about_close(); break;
        case VK_RETURN: case VK_SPACE: about_press(AB.focus); break;
        case VK_TAB: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: {
            bool back = wp == VK_LEFT || wp == VK_UP || (wp == VK_TAB && GetKeyState(VK_SHIFT) < 0);
            AB.focus = back ? (AB.focus == 1 ? AB_N : AB.focus - 1) : (AB.focus == AB_N ? 1 : AB.focus + 1);
            acc_desc(hwnd, AB_NAMES[AB.focus]);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        }
        return 0;
    case WM_CLOSE:
        about_close();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void about_open(void) {
    if (AB.wnd) { SetForegroundWindow(AB.wnd); return; }
    RECT mr;
    GetWindowRect(G.main, &mr);
    int W = S(AB_W), H = S(AB_H);
    int x = (mr.left + mr.right - W) / 2, y = (mr.top + mr.bottom - H) / 2;
    AB.hover = 0;
    AB.focus = AB_OK; /* as a ContentDialog's default button */
    AB.wnd = CreateWindowExW(0, ABOUT_CLASS, L"About " APP_NAME, WS_POPUP, x, y, W, H,
        G.main, NULL, GetModuleHandleW(NULL), NULL);
    if (!AB.wnd) return;
    DWORD corner = 2; /* DWMWCP_ROUND */
    DwmSetWindowAttribute(AB.wnd, 33, &corner, sizeof(corner));
    acc_control(AB.wnd, L"About " APP_NAME, ROLE_SYSTEM_DIALOG,
        L"Version " APP_VERSION L". " AB_DESC L" Questions, concerns or a gig to offer? "
        L"\x00A9 2026 PowerNapps (by Denis Platonov), Los Angeles. Controls: Contact us, " AB_LINK L", Check for updates, OK.");
    EnableWindow(G.main, FALSE);
    ShowWindow(AB.wnd, SW_SHOW);
    SetForegroundWindow(AB.wnd);
    SetFocus(AB.wnd);
}
