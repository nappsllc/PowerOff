/* PowerOff: tiny auto shutdown / sleep scheduler for Windows (C, plain Win32).
 *
 * The window copies the design (Windows 11 Settings page): Schedule hero card, Task
 * rows (Action / Schedule / Time), Options toggles, Related settings cards, Fluent
 * light/dark tokens, Segoe UI Variable, full-color Fluent icons. Win32 has no cards,
 * toggles or flyouts, so every control is an owner-drawn BUTTON painted with GDI
 * (anti-aliased by hand), DPI-scaled, keyboard-navigable and annotated for screen
 * readers. Scheduled jobs are handed to Windows Task Scheduler, so the app itself can
 * exit. Build with build.bat (MSVC); poweroff.c includes the parts in src/.
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <powrprof.h>
#include <dwmapi.h>
#include <appmodel.h>  /* GetCurrentPackageFullName: Store (MSIX) build detection */
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <fcntl.h>
#include <io.h>
#include <strsafe.h>
#define COBJMACROS
#include <wincodec.h>
#include <taskschd.h>
#include <initguid.h>   /* defines oleacc's PROPID_ACC_* GUIDs in this translation unit */
#include <oleacc.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "oleaut32.lib")

/* ------------------------------------------------------------------ sources
 * Unity build: one translation unit, so everything stays static and the build is a
 * single cl call. Order matters: each part uses only what comes before it. */
#include "src/model.c"
#include "src/taskschd.c"
#include "src/theme.c"
#include "src/access.c"
#include "src/flyout.c"
#include "src/layout.c"
#include "src/tray.c"
#include "src/views.c"
#include "src/timepicker.c"
#include "src/about.c"
#include "src/reminder.c"
#include "src/window.c"
#include "src/cli.c"
