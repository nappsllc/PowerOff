/* PowerOff: accessibility. Part of the unity build: included by poweroff.c, in order. */

/* ------------------------------------------------------------------ accessibility
 * Every control is an owner-drawn BUTTON, so a screen reader would hear unnamed
 * buttons. Dynamic Annotation (IAccPropServices) overrides what the standard MSAA
 * proxy reports for each window: name, role, state, value, description. COM and
 * oleacc are delay-loaded and only start with the window. */

static IAccPropServices *g_acc;

static void acc_init(void) {
    if (g_acc) return;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(CoCreateInstance(&CLSID_AccPropServices, NULL, CLSCTX_INPROC_SERVER,
            &IID_IAccPropServices, (void **)&g_acc)))
        g_acc = NULL;
}

static void acc_str(HWND h, const MSAAPROPID *prop, const wchar_t *s) {
    if (g_acc && h) IAccPropServices_SetHwndPropStr(g_acc, h, OBJID_CLIENT, CHILDID_SELF, *prop, s);
}

static void acc_int(HWND h, const MSAAPROPID *prop, LONG v) {
    if (!g_acc || !h) return;
    VARIANT var;
    memset(&var, 0, sizeof(var));
    var.vt = VT_I4;
    var.lVal = v;
    IAccPropServices_SetHwndProp(g_acc, h, OBJID_CLIENT, CHILDID_SELF, *prop, var);
}

/* name + role + description in one go */
static void acc_control(HWND h, const wchar_t *name, LONG role, const wchar_t *desc) {
    acc_str(h, &PROPID_ACC_NAME, name);
    acc_int(h, &PROPID_ACC_ROLE, role);
    if (desc) acc_str(h, &PROPID_ACC_DESCRIPTION, desc);
}

/* Check-box state. Overriding the state replaces the proxy's, so focus is added here
 * too; call again when the value or the focus changes. */
static void acc_checked(HWND h, bool on) {
    if (!h) return;
    LONG st = STATE_SYSTEM_FOCUSABLE | (on ? STATE_SYSTEM_CHECKED : 0) |
              (GetFocus() == h ? STATE_SYSTEM_FOCUSED : 0) |
              (IsWindowVisible(h) ? 0 : STATE_SYSTEM_INVISIBLE);
    acc_int(h, &PROPID_ACC_STATE, st);
    NotifyWinEvent(EVENT_OBJECT_STATECHANGE, h, OBJID_CLIENT, CHILDID_SELF);
}

static void acc_value(HWND h, const wchar_t *v) {
    acc_str(h, &PROPID_ACC_VALUE, v);
    if (h) NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, h, OBJID_CLIENT, CHILDID_SELF);
}

static void acc_desc(HWND h, const wchar_t *d) {
    acc_str(h, &PROPID_ACC_DESCRIPTION, d);
    if (h) NotifyWinEvent(EVENT_OBJECT_DESCRIPTIONCHANGE, h, OBJID_CLIENT, CHILDID_SELF);
}
