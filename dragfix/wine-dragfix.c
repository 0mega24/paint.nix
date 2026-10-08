/* XWayland freezes pointer queries during compositor-owned moves.
 * Use window displacement to keep Wine's cursor and WM_MOVING in step. */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* Wine 11.18 include/ntuser.h. Recheck these private APIs when updating Wine. */
typedef void *HWND;
typedef struct
{
    int32_t left, top, right, bottom;
} RECT;

typedef struct
{
    int32_t x, y;
} POINT;

struct get_window_rects_params
{
    RECT *rect;
    unsigned int dpi;
};

typedef struct
{
    int32_t dx, dy;
    uint32_t mouseData, dwFlags, time;
    uintptr_t dwExtraInfo;
} MOUSEINPUT;

typedef struct
{
    uint32_t type;
    MOUSEINPUT mi;
} INPUT;

struct send_hardware_input_params
{
    unsigned int flags;
    const INPUT *input;
    intptr_t lparam;
};
_Static_assert(sizeof(INPUT) == 40, "INPUT must match Win32 x64 layout");
_Static_assert(offsetof(INPUT, mi) == 8, "INPUT.mi must match Win32 x64 alignment");

#define WM_MOVING                        0x0216
#define GA_ROOT                          2
#define NTUSER_SEND_MESSAGE              0x02b1  /* NtUserSendMessage */
#define NTUSER_HWNDPARAM_GET_WINDOW_RECT 13      /* NtUserCallHwndParam_GetWindowRect */
#define NTUSER_HWNDPARAM_SEND_HW_INPUT   26      /* NtUserCallHwndParam_SendHardwareInput */
#define SEND_HWMSG_RAWINPUT              0x02
#define INPUT_MOUSE                      0
#define MOUSEEVENTF_MOVE                 0x0001
#define MOUSEEVENTF_ABSOLUTE             0x8000
#define NET_WM_MOVERESIZE_MOVE           8
#define DRAG_TIMEOUT_MS                  30000
#define ENTER_GRACE_MS                   200     /* ignore EnterNotify right after the drag starts */

typedef intptr_t (*NtUserMessageCall_fn)(HWND, unsigned int, uintptr_t, intptr_t, void *, uint32_t, int);
typedef uintptr_t (*NtUserCallHwndParam_fn)(HWND, uintptr_t, uint32_t);
typedef int (*NtUserGetCursorPos_fn)(POINT *);
typedef HWND (*NtUserWindowFromPoint_fn)(int32_t, int32_t);
typedef HWND (*NtUserGetAncestor_fn)(HWND, unsigned int);

typedef Status (*XSendEvent_fn)(Display *, Window, Bool, long, XEvent *);
typedef Bool (*XQueryPointer_fn)(Display *, Window, Window *, Window *, int *, int *, int *, int *, unsigned int *);
typedef Bool (*XCheckIfEvent_fn)(Display *, XEvent *, Bool (*)(Display *, XEvent *, XPointer), XPointer);
typedef Bool (*XTranslateCoordinates_fn)(Display *, Window, Window, int, int, int *, int *, Window *);
typedef Atom (*XInternAtom_fn)(Display *, const char *, Bool);

static _Thread_local struct
{
    int active, ended, in_send, saw_leave;
    Display *display;
    HWND hwnd;
    Window xwin;
    RECT start_rect;
    int start_x, start_y, last_dx, last_dy;
    int want_dx, want_dy;
    int snap_x, snap_y;      /* App-adjusted position from the last WM_MOVING. */
    int press_x, press_y;    /* X root coordinates. */
    POINT cursor;           /* Win32 coordinates at the start of the drag. */
    unsigned long sent, polls, snapped;
    long start_ms;
    const char *end_reason;
} drag;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *fmt, ...)
{
    const char *path = getenv("WINE_DRAGFIX_LOG");
    FILE *f;
    va_list args;
    if (!path || !(f = fopen(path, "a"))) return;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

/* Wine loads these libraries; the shim does not link against them. */
static void *x11(const char *name)
{
    return dlsym(RTLD_DEFAULT, name);
}

static void *w32u(const char *name)
{
    static _Thread_local void *handle;
    void *sym = dlsym(RTLD_DEFAULT, name);
    if (sym) return sym;
    if (!handle) handle = dlopen("win32u.so", RTLD_NOW | RTLD_NOLOAD);  /* matches by SONAME */
    return handle ? dlsym(handle, name) : NULL;
}

static int window_origin(Display *display, Window xwin, int *x, int *y)
{
    XTranslateCoordinates_fn translate = x11("XTranslateCoordinates");
    Window child;
    return translate && translate(display, xwin, DefaultRootWindow(display), 0, 0, x, y, &child);
}

static void start_drag(Display *display, Window xwin, int press_x, int press_y)
{
    NtUserGetCursorPos_fn get_cursor = w32u("NtUserGetCursorPos");
    NtUserWindowFromPoint_fn from_point = w32u("NtUserWindowFromPoint");
    NtUserGetAncestor_fn ancestor = w32u("NtUserGetAncestor");
    NtUserCallHwndParam_fn hwnd_param = w32u("NtUserCallHwndParam");
    struct get_window_rects_params params = {.rect = &drag.start_rect, .dpi = 0};
    POINT pt;
    HWND hwnd;

    drag.active = 0;
    if (!get_cursor || !from_point || !ancestor || !hwnd_param)
    {
        log_line("drag skipped: win32u symbols missing (%p %p %p %p)", get_cursor, from_point, ancestor, hwnd_param);
        return;
    }
    if (!get_cursor(&pt))
    {
        log_line("drag skipped: GetCursorPos failed");
        return;
    }
    if (!(hwnd = from_point(pt.x, pt.y)))
    {
        log_line("drag skipped: no window at (%d,%d)", pt.x, pt.y);
        return;
    }
    if (!(hwnd = ancestor(hwnd, GA_ROOT)))
    {
        log_line("drag skipped: no root window");
        return;
    }
    if (!hwnd_param(hwnd, (uintptr_t)&params, NTUSER_HWNDPARAM_GET_WINDOW_RECT))
    {
        log_line("drag skipped: GetWindowRect(%p) failed", hwnd);
        return;
    }
    if (!window_origin(display, xwin, &drag.start_x, &drag.start_y))
    {
        log_line("drag skipped: XTranslateCoordinates(%lx) failed", xwin);
        return;
    }

    drag.hwnd = hwnd;
    drag.xwin = xwin;
    drag.display = display;
    drag.last_dx = drag.last_dy = drag.want_dx = drag.want_dy = 0;
    drag.cursor = pt;
    drag.press_x = press_x;
    drag.press_y = press_y;
    drag.snap_x = drag.start_x;
    drag.snap_y = drag.start_y;
    drag.sent = drag.polls = drag.snapped = 0;
    drag.ended = drag.in_send = drag.saw_leave = 0;
    drag.end_reason = "?";
    drag.start_ms = now_ms();
    drag.active = 1;
    log_line("drag start hwnd=%p rect=(%d,%d)-(%d,%d) cursor=(%d,%d)", hwnd, drag.start_rect.left,
             drag.start_rect.top, drag.start_rect.right, drag.start_rect.bottom, pt.x, pt.y);
}

__attribute__((constructor)) static void loaded(void)
{
    if (getenv("WINE_DRAGFIX_LOG")) log_line("dragfix loaded in pid %d", (int)getpid());
}

Status XSendEvent(Display *display, Window w, Bool propagate, long mask, XEvent *event)
{
    static _Thread_local XSendEvent_fn real;
    Atom moveresize;
    Status ret;

    if (!real) real = (XSendEvent_fn)dlsym(RTLD_NEXT, "XSendEvent");
    if (!real) return 0;
    if (event && event->type == ClientMessage && event->xclient.format == 32 && !drag.in_send)
    {
        XInternAtom_fn intern_atom = x11("XInternAtom");
        moveresize = intern_atom ? intern_atom(display, "_NET_WM_MOVERESIZE", True) : None;
        if (moveresize && event->xclient.message_type == moveresize)
            log_line("_NET_WM_MOVERESIZE window=%lx pos=(%ld,%ld) direction=%ld button=%ld", event->xclient.window,
                     event->xclient.data.l[0], event->xclient.data.l[1], event->xclient.data.l[2], event->xclient.data.l[3]);
        if (moveresize && event->xclient.message_type == moveresize &&
            event->xclient.data.l[2] == NET_WM_MOVERESIZE_MOVE && event->xclient.data.l[3] == 1)
            start_drag(display, event->xclient.window, event->xclient.data.l[0], event->xclient.data.l[1]);
    }
    ret = real(display, w, propagate, mask, event);
    if (!ret && drag.display == display) drag.active = 0;
    return ret;
}

Bool XCheckIfEvent(Display *display, XEvent *event, Bool (*predicate)(Display *, XEvent *, XPointer), XPointer arg)
{
    static _Thread_local XCheckIfEvent_fn real;
    Bool ret;

    if (!real) real = (XCheckIfEvent_fn)dlsym(RTLD_NEXT, "XCheckIfEvent");
    if (!real) return False;
    ret = real(display, event, predicate, arg);
    if (!ret || !drag.active || drag.ended || drag.display != display) return ret;
    if (event->type == LeaveNotify) drag.saw_leave = 1;
    else if (event->type == ButtonRelease)
    {
        drag.ended = 1;
        drag.end_reason = "ButtonRelease";
    }
    else if (event->type == ButtonPress)
    {
        drag.ended = 1;
        drag.end_reason = "ButtonPress";
    }
    /* Pointer motion resumes when the compositor gives the pointer back. */
    else if (event->type == MotionNotify && drag.saw_leave)
    {
        drag.ended = 1;
        drag.end_reason = "MotionNotify";
    }
    else if (event->type == EnterNotify && (drag.saw_leave || now_ms() - drag.start_ms > ENTER_GRACE_MS))
    {
        drag.ended = 1;
        drag.end_reason = "EnterNotify";
    }
    return ret;
}

Bool XQueryPointer(Display *display, Window w, Window *root, Window *child, int *root_x, int *root_y,
                   int *win_x, int *win_y, unsigned int *mask)
{
    static _Thread_local XQueryPointer_fn real;
    static _Thread_local NtUserMessageCall_fn message_call;
    Bool ret;
    int x, y;

    if (!real) real = (XQueryPointer_fn)dlsym(RTLD_NEXT, "XQueryPointer");
    if (!real) return False;
    ret = real(display, w, root, child, root_x, root_y, win_x, win_y, mask);
    if (!drag.active || drag.display != display) return ret;
    if (!ret)
    {
        drag.active = 0;
        return ret;
    }
    if (!drag.ended && now_ms() - drag.start_ms > DRAG_TIMEOUT_MS)
    {
        drag.ended = 1;
        drag.end_reason = "timeout";
    }
    if (drag.ended)
    {
        drag.active = 0;
        log_line("drag end hwnd=%p reason=%s pointer-delta=(%d,%d) WM_MOVING=%lu snapped=%lu polls=%lu ms=%ld",
                 drag.hwnd, drag.end_reason, drag.want_dx, drag.want_dy, drag.sent, drag.snapped,
                 drag.polls, now_ms() - drag.start_ms);
        return ret;
    }

    /* XWayland's real button state is unavailable during the compositor's move. */
    if (w == DefaultRootWindow(display))
    {
        *win_x = *root_x = drag.press_x + drag.want_dx;
        *win_y = *root_y = drag.press_y + drag.want_dy;
    }
    *mask |= Button1Mask;
    if (drag.in_send) return ret;
    drag.polls++;

    if (!window_origin(display, drag.xwin, &x, &y)) return ret;
    /* Ignore the app's last snap position when measuring compositor movement. */
    if (x != drag.snap_x || y != drag.snap_y)
    {
        drag.want_dx = x - drag.start_x;
        drag.want_dy = y - drag.start_y;
    }
    int dx = drag.want_dx, dy = drag.want_dy;
    if (dx == drag.last_dx && dy == drag.last_dy) return ret;
    if (!message_call) message_call = w32u("NtUserMessageCall");
    if (!message_call) return ret;

    /* Both calls can reenter XQueryPointer through Wine. */
    drag.in_send = 1;
    {
        static _Thread_local NtUserCallHwndParam_fn hwnd_param;
        INPUT input = {.type = INPUT_MOUSE};
        struct send_hardware_input_params params = {.flags = SEND_HWMSG_RAWINPUT, .input = &input};
        input.mi.dx = drag.cursor.x + dx;
        input.mi.dy = drag.cursor.y + dy;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        if (!hwnd_param) hwnd_param = w32u("NtUserCallHwndParam");
        if (hwnd_param) hwnd_param(drag.hwnd, (uintptr_t)&params, NTUSER_HWNDPARAM_SEND_HW_INPUT);
    }

    RECT rect = drag.start_rect;
    rect.left += dx;
    rect.right += dx;
    rect.top += dy;
    rect.bottom += dy;
    drag.last_dx = dx;
    drag.last_dy = dy;
    drag.sent++;
    message_call(drag.hwnd, WM_MOVING, 0, (intptr_t)&rect, NULL, NTUSER_SEND_MESSAGE, 0);
    drag.in_send = 0;
    /* Paint.NET may have snapped the requested rect to a nearby edge. */
    drag.snap_x = drag.start_x + (rect.left - drag.start_rect.left);
    drag.snap_y = drag.start_y + (rect.top - drag.start_rect.top);
    if (rect.left != drag.start_rect.left + dx || rect.top != drag.start_rect.top + dy) drag.snapped++;
    if (drag.sent <= 3 || drag.sent % 10 == 0)
    {
        NtUserGetCursorPos_fn get_cursor = w32u("NtUserGetCursorPos");
        POINT now = {0, 0};
        if (get_cursor) get_cursor(&now);
        log_line("  WM_MOVING #%lu sent (%d,%d) -> app returned (%d,%d) cursor now (%d,%d)", drag.sent,
                 drag.start_rect.left + dx, drag.start_rect.top + dy, rect.left, rect.top, now.x, now.y);
    }
    return ret;
}
