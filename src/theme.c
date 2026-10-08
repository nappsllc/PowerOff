/* PowerOff: theme tokens, GDI drawing helpers, icons. Part of the unity build: included by poweroff.c, in order. */

/* ------------------------------------------------------------------ theme (artifact tokens) */

static COLORREF T_WIN, T_CARD, T_CARDBRD, T_T1, T_T2, T_CTL, T_CTLBRD,
                T_CTLBOT, T_ACCENT, T_ONACC, T_FLY, T_FLYBRD, T_SUBTLE, T_TOGBRD, T_TOGFILL,
                T_CARDHOV, T_CTLHOV, T_ACCHOV; /* hover fills */

static void theme_tokens(void) {
    if (G.dark) {
        T_WIN = RGB(32, 32, 32);      T_CARD = RGB(43, 43, 43);
        T_CARDBRD = RGB(58, 58, 58);  T_T1 = RGB(255, 255, 255);
        T_T2 = RGB(200, 200, 200);    T_CTL = RGB(47, 47, 47);
        T_CTLBRD = RGB(58, 58, 58);   T_CTLBOT = RGB(61, 61, 61);
        T_ACCENT = RGB(96, 205, 255); T_ONACC = RGB(0, 0, 0);
        T_FLY = RGB(44, 44, 44);      T_FLYBRD = RGB(51, 51, 51);
        T_SUBTLE = RGB(56, 56, 56);   T_TOGBRD = RGB(200, 200, 200);
        T_TOGFILL = RGB(26, 26, 26);
        T_CARDHOV = RGB(50, 50, 50);  T_CTLHOV = RGB(56, 56, 56);
        T_ACCHOV = RGB(90, 185, 230);
        G.accent = T_ACCENT;
    } else {
        T_WIN = RGB(243, 243, 243);   T_CARD = RGB(251, 251, 251);
        T_CARDBRD = RGB(239, 239, 239); T_T1 = RGB(27, 27, 27);
        T_T2 = RGB(90, 90, 90);       T_CTL = RGB(250, 250, 250);
        T_CTLBRD = RGB(239, 239, 239); T_CTLBOT = RGB(214, 214, 214);
        T_ACCENT = RGB(0, 95, 184);   T_ONACC = RGB(255, 255, 255);
        T_FLY = RGB(249, 249, 249);   T_FLYBRD = RGB(239, 239, 239);
        T_SUBTLE = RGB(234, 234, 234); T_TOGBRD = RGB(90, 90, 90);
        T_TOGFILL = RGB(246, 246, 246);
        T_CARDHOV = RGB(246, 246, 246); T_CTLHOV = RGB(246, 246, 246);
        T_ACCHOV = RGB(26, 111, 191);
        G.accent = T_ACCENT;
    }
}

static void read_theme(void) {
    HKEY hk;
    G.dark = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
        DWORD type = 0, data = 1, cb = sizeof(data);
        if (RegQueryValueExW(hk, L"AppsUseLightTheme", NULL, &type, (BYTE *)&data, &cb) == ERROR_SUCCESS)
            G.dark = (data == 0);
        RegCloseKey(hk);
    }
}

typedef int (WINAPI *SetPrefModeFn)(int);
typedef int (WINAPI *AllowDarkFn)(HWND, BOOL);
typedef void (WINAPI *RefreshPolicyFn)(void);

static void apply_dark_mode(HWND hwnd) {
    BOOL on = G.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &on, sizeof(on));
    HMODULE ux = LoadLibraryW(L"uxtheme.dll"); /* delay-loaded: may not be mapped yet */
    if (ux) {
        SetPrefModeFn set_mode = (SetPrefModeFn)GetProcAddress(ux, (LPCSTR)135);
        if (set_mode) set_mode(G.dark ? 1 : 0);
        AllowDarkFn allow = (AllowDarkFn)GetProcAddress(ux, (LPCSTR)133);
        if (allow) allow(hwnd, on);
        SetWindowTheme(hwnd, G.dark ? L"DarkMode_Explorer" : NULL, NULL);
        RefreshPolicyFn refresh = (RefreshPolicyFn)GetProcAddress(ux, (LPCSTR)104);
        if (refresh) refresh();
    }
    theme_tokens();
    /* design: the title bar is the window background, one surface with the page
     * (Windows 11; older builds ignore these attributes) */
    COLORREF cap = T_WIN, txt = T_T1;
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &txt, sizeof(txt));
    if (G.br_ctl) DeleteObject(G.br_ctl);
    G.br_ctl = CreateSolidBrush(T_CTL);
}

/* ------------------------------------------------------------------ DPI
 * Geometry is written in the design's 96-DPI pixels and scaled to the window's
 * monitor (per-monitor DPI aware, see poweroff.exe.manifest): S() for whole pixels,
 * SF() for the anti-aliased shapes. Hairlines stay 1 px until 200%, like WinUI. */

static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }
static float SF(float v) { return v * (float)g_dpi / 96.0f; }
static int SB(int w) { int b = w * g_dpi / 96; return b < 1 ? 1 : b; }

/* ------------------------------------------------------------------ GDI helpers */

/* px: design pixels (negative = character height, as CreateFont), scaled to the DPI */
static HFONT make_font(const wchar_t *primary, const wchar_t *fallback, int px, int weight) {
    px = MulDiv(px, g_dpi, 96);
    HFONT f = CreateFontW(px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, primary);
    if (!f) f = CreateFontW(px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, fallback);
    return f;
}

/* GDI draws curves without anti-aliasing. Rounded shapes are instead composited per
 * pixel from a signed distance field, over whatever is already painted (read back
 * from the DC), so edges get fractional coverage like WinUI's. r = corner radius;
 * ew > 0 draws an inside border of that width in `edge`. */
static COLORREF mix_c(COLORREF a, COLORREF b, float t) { /* a -> b */
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t + 0.5f),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t + 0.5f),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t + 0.5f));
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void aa_rrect(HDC hdc, float x, float y, float w, float h, float r,
                     COLORREF fill, COLORREF edge, float ew) {
    RECT sr = { (int)floorf(x), (int)floorf(y), (int)ceilf(x + w), (int)ceilf(y + h) }, cb;
    int rgn = GetClipBox(hdc, &cb); /* only the part being repainted */
    if (rgn == NULLREGION) return;
    if (rgn != ERROR && !IntersectRect(&sr, &sr, &cb)) return;
    int x0 = sr.left, y0 = sr.top, W = sr.right - sr.left, H = sr.bottom - sr.top;
    if (W <= 0 || H <= 0) return;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HDC mdc = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!mdc || !bm) { if (bm) DeleteObject(bm); if (mdc) DeleteDC(mdc); return; }
    HGDIOBJ old = SelectObject(mdc, bm);
    BitBlt(mdc, 0, 0, W, H, hdc, x0, y0, SRCCOPY);
    GdiFlush();
    DWORD *px = (DWORD *)bits;
    float cx = x + w / 2, cy = y + h / 2, hx = w / 2 - r, hy = h / 2 - r;
    for (int j = 0; j < H; j++) {
        for (int i = 0; i < W; i++) {
            float qx = fabsf(x0 + i + 0.5f - cx) - hx, qy = fabsf(y0 + j + 0.5f - cy) - hy;
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
            float a = clamp01(0.5f - d);
            if (a <= 0) continue;
            COLORREF c = ew > 0 ? mix_c(edge, fill, clamp01(0.5f - (d + ew))) : fill;
            DWORD p = px[j * W + i]; /* 0x00RRGGBB */
            COLORREF under = RGB((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
            COLORREF o = mix_c(under, c, a);
            px[j * W + i] = (GetRValue(o) << 16) | (GetGValue(o) << 8) | GetBValue(o);
        }
    }
    BitBlt(hdc, x0, y0, W, H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteObject(bm);
    DeleteDC(mdc);
}

/* rc in device pixels; rad = the design's border-radius (4 for controls and cards) */
static void fill_round(HDC hdc, const RECT *rc, int rad, COLORREF fill, COLORREF edge) {
    aa_rrect(hdc, (float)rc->left, (float)rc->top, (float)(rc->right - rc->left),
        (float)(rc->bottom - rc->top), SF((float)rad), fill, edge, edge != fill ? (float)SB(1) : 0.0f);
}

static void fill_rect_c(HDC hdc, const RECT *rc, COLORREF fill) {
    HBRUSH br = CreateSolidBrush(fill);
    FillRect(hdc, rc, br);
    DeleteObject(br);
}

static void text_at(HDC hdc, const wchar_t *s, RECT rc, HFONT font, COLORREF color, UINT flags) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    SelectObject(hdc, font);
    DrawTextW(hdc, s, -1, &rc, flags | DT_NOPREFIX); /* "&" is text, not a mnemonic */
}

static void line_c(HDC hdc, int x0, int y0, int x1, int y1, COLORREF color, int w) {
    HPEN pen = CreatePen(PS_SOLID, SB(w), color); /* w: design width */
    HGDIOBJ op = SelectObject(hdc, pen);
    MoveToEx(hdc, x0, y0, NULL);
    LineTo(hdc, x1, y1);
    SelectObject(hdc, op);
    DeleteObject(pen);
}

/* ------------------------------------------------------------------ icons
 * Glyphs from Segoe Fluent Icons, the font Windows 11 Settings draws its icons with
 * (Segoe MDL2 Assets on Windows 10). Text rendering gives them proper anti-aliasing. */

static const wchar_t ICON_GLYPH[] = {
    0xE7E8, /* IC_POWER   PowerButton */
    0xE72C, /* IC_RESTART Refresh */
    0xE708, /* IC_MOON    QuietHours */
    0xF3B1, /* IC_SIGNOUT SignOut */
    0xE72E, /* IC_LOCK    Lock */
    0xE7F4, /* IC_MONITOR TVMonitor */
    0xEA8F, /* IC_ALERT   Ringer */
    0xE7C4, /* IC_APPS    TaskView */
    0xE7B5, /* IC_ROCKET  SetLockScreen (window + arrow: start with Windows) */
    0xEC92, /* IC_CALCLK  DateTime */
    0xE823, /* IC_CLOCK   Recent */
    0xE706, /* IC_SUN     Brightness */
    0xE708, /* IC_HIBER   QuietHours */
};

static struct { int px; HFONT f; } g_icon_fonts[6]; /* device px -> font */

static HFONT icon_font(int px) { /* px: device pixels */
    static const wchar_t *face;
    for (int i = 0; i < 6; i++)
        if (g_icon_fonts[i].px == px) return g_icon_fonts[i].f;
    if (!face) { /* CreateFont never fails on a missing face, so check what we got */
        HFONT t = make_font(L"Segoe Fluent Icons", L"Segoe MDL2 Assets", -16, FW_NORMAL);
        HDC dc = GetDC(NULL);
        HGDIOBJ old = SelectObject(dc, t);
        wchar_t got[LF_FACESIZE] = L"";
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        ReleaseDC(NULL, dc);
        DeleteObject(t);
        face = wcscmp(got, L"Segoe Fluent Icons") == 0 ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }
    HFONT f = make_font(face, face, -MulDiv(px, 96, g_dpi), FW_NORMAL); /* make_font rescales */
    for (int i = 0; i < 6; i++)
        if (!g_icon_fonts[i].px) { g_icon_fonts[i].px = px; g_icon_fonts[i].f = f; break; }
    return f;
}

static void glyph_px(HDC hdc, wchar_t g, RECT rc, int px, COLORREF c) { /* px: device */
    wchar_t t[2] = { g, 0 };
    text_at(hdc, t, rc, icon_font(px), c, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
}

static void glyph_at(HDC hdc, wchar_t g, RECT rc, int px, COLORREF c) { /* px: design */
    glyph_px(hdc, g, rc, S(px), c);
}

/* Full-color Fluent icons from the design (icons\*_48.png, embedded by poweroff.rc as
 * RCDATA 100+id). Decoded with WIC, scaled with the Fant filter to the exact device size
 * (20 px rows / 32 px hero times the DPI), premultiplied, drawn with AlphaBlend.
 * windowscodecs/msimg32 are delay-loaded, so only the window ever maps them. Missing
 * bitmaps fall back to the monochrome glyph. */
HRESULT WINAPI WICCreateImagingFactory_Proxy(UINT sdk, IWICImagingFactory **out);

static HBITMAP load_png_res(int res, UINT size) {
    static IWICImagingFactory *fac;
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(res), RT_RCDATA);
    if (!r) return NULL;
    void *data = LockResource(LoadResource(NULL, r));
    DWORD len = SizeofResource(NULL, r);
    if (!fac && FAILED(WICCreateImagingFactory_Proxy(WINCODEC_SDK_VERSION, &fac))) return NULL;
    HBITMAP bm = NULL;
    IWICStream *st = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    IWICBitmapScaler *sc = NULL;
    UINT w = size, h = size;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(fac, &st)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(st, (BYTE *)data, len)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(fac, (IStream *)st, NULL,
            WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) &&
        SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(fac, &sc)) &&
        SUCCEEDED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)fr, size, size,
            WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(fac, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)sc,
            &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = (LONG)w;
        bi.bmiHeader.biHeight = -(LONG)h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void *bits = NULL;
        bm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bm && FAILED(IWICFormatConverter_CopyPixels(cv, NULL, w * 4, w * h * 4, (BYTE *)bits))) {
            DeleteObject(bm);
            bm = NULL;
        }
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (sc) IWICBitmapScaler_Release(sc);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
    return bm;
}

static struct { int id, px; HBITMAP bm; } g_color_icons[32]; /* (icon, device px) -> bitmap */

static HBITMAP color_icon(IconId id, int px) {
    int i;
    for (i = 0; i < 32 && g_color_icons[i].px; i++)
        if (g_color_icons[i].id == (int)id && g_color_icons[i].px == px) return g_color_icons[i].bm;
    HBITMAP bm = load_png_res(100 + id, (UINT)px);
    if (i < 32) { g_color_icons[i].id = id; g_color_icons[i].px = px; g_color_icons[i].bm = bm; }
    return bm;
}

/* After a DPI change: drop everything rendered at the old size */
static void theme_dpi_reset(void) {
    for (int i = 0; i < 6; i++)
        if (g_icon_fonts[i].f) DeleteObject(g_icon_fonts[i].f);
    for (int i = 0; i < 32; i++)
        if (g_color_icons[i].bm) DeleteObject(g_color_icons[i].bm);
    memset(g_icon_fonts, 0, sizeof(g_icon_fonts));
    memset(g_color_icons, 0, sizeof(g_color_icons));
}

/* s x s icon at (x, y), all device pixels; c only colors the glyph fallback */
static void icon_bg(HDC hdc, int x, int y, int s, IconId id, COLORREF c, COLORREF bg) {
    (void)bg;
    HBITMAP bm = color_icon(id, s);
    if (bm) {
        HDC mdc = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(mdc, bm);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, x, y, s, s, mdc, 0, 0, s, s, bf);
        SelectObject(mdc, old);
        DeleteDC(mdc);
        return;
    }
    RECT rc = { x, y, x + s, y + s };
    glyph_px(hdc, ICON_GLYPH[id], rc, s, c);
}

/* down chevron (combo), inside rc */
static void chevron_down(HDC hdc, const RECT *rc, COLORREF c) {
    glyph_at(hdc, 0xE70D, *rc, 12, c);
}
