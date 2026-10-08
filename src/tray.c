/* PowerOff: notification-area icon. Part of the unity build: included by poweroff.c, in order. */

/* ------------------------------------------------------------------ tray */

static void add_tray(HWND hwnd) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0); /* poweroff.rc */
    wcscpy_s(nid.szTip, 128, L"PowerOff");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void del_tray(HWND hwnd) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void set_tray_tip(void) {
    if (!G.tray) return;
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = G.tray;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wchar_t d[96] = L"";
    if (G.rt.active) rt_describe(&G.rt, d, 96);
    StringCchPrintfW(nid.szTip, 128, L"PowerOff%ls%ls%ls", d[0] ? L"\n" : L"", d,
        G.delegated ? L"\nScheduled in Windows Task Scheduler" : L"");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void show_tray_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Show window");
    AppendMenuW(menu, MF_STRING, IDM_START, G.rt.active ? L"Cancel task" : L"Start task");
    AppendMenuW(menu, MF_STRING, IDM_NOW, L"Sleep now");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, G.delegated ? L"Exit (Windows keeps the task)" : L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

