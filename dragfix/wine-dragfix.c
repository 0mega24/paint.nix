/*
 * wine-dragfix.so -- LD_PRELOAD shim for Paint.NET-on-Wine under GNOME/XWayland.
 *
 * Problem: winex11 hands caption drags of managed windows to the window manager
 * (_NET_WM_MOVERESIZE, see move_resize_window() in dlls/winex11.drv/mouse.c) and
 * then polls XQueryPointer() until the mouse button is released. Under XWayland
 * the compositor owns the pointer during that drag, so XWayland reports no
 * buttons held: winex11 "sees" the release on its very first poll, sends
 * WM_LBUTTONUP + WM_EXITSIZEMOVE before the window has moved, and the app never
 * gets WM_MOVING. Paint.NET's snap manager then treats the compositor's move as
 * an unexpected one and snaps its floating palettes back to the canvas corners.
 *
 * Fix (the compositor keeps doing the actual, smooth drag):
 *   - while a WM-driven move is in progress, report the button as still held from
 *     XQueryPointer so winex11's move loop stays alive;
 *   - on each poll, read where the compositor has moved the X window and
 *     SendMessage(WM_MOVING) with the matching rect, like Windows' move loop;
 *   - end the drag when the compositor hands the pointer back to XWayland on
 *     release (EnterNotify / ButtonRelease seen by winex11), or after a timeout.
 * No pointer grabs and no synthetic X events.
 *
 * Optional: WINE_DRAGFIX_LOG=/path/file appends one line per drag start/end.
 *
 * Build: gcc -shared -fPIC -O2 -o wine-dragfix.so wine-dragfix.c -ldl -lpthread
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* minimal Win32 / win32u definitions (from Wine's include/ntuser.h) */
typedef void *HWND;
typedef struct { int32_t left, top, right, bottom; } RECT;
typedef struct { int32_t x, y; } POINT;
struct get_window_rects_params { RECT *rect; unsigned int dpi; };
typedef struct { int32_t dx, dy; uint32_t mouseData, dwFlags, time; uintptr_t dwExtraInfo; } MOUSEINPUT;
typedef struct { uint32_t type; MOUSEINPUT mi; uint8_t pad[40 - 8 - sizeof(MOUSEINPUT)]; } INPUT;  /* 40 bytes on x64 */
struct send_hardware_input_params { unsigned int flags; const INPUT *input; intptr_t lparam; };
_Static_assert(sizeof(INPUT) == 40, "INPUT must match Win32 x64 layout");

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

static struct
{
    int active, ended, in_send, saw_leave;
    pthread_t thread;
    HWND hwnd;
    Window xwin;
    RECT start_rect;
    int start_x, start_y, last_dx, last_dy;
    int want_dx, want_dy;    /* pointer delta, as applied by the compositor */
    int snap_x, snap_y;      /* X position the app last asked for in WM_MOVING */
    int press_x, press_y;    /* root coordinates of the button press that started the drag */
    POINT cursor;            /* Win32 cursor position at the press */
    unsigned long sent, polls, snapped;
    long start_ms;
    char events[64];
    int nevents;
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

/* libX11 and win32u are loaded by Wine, not linked into this shim: resolve at runtime */
static void *x11(const char *name)
{
    return dlsym(RTLD_DEFAULT, name);
}

static void *w32u(const char *name)
{
    static void *handle;
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
    if (!get_cursor(&pt)) { log_line("drag skipped: GetCursorPos failed"); return; }
    if (!(hwnd = from_point(pt.x, pt.y))) { log_line("drag skipped: no window at (%d,%d)", pt.x, pt.y); return; }
    if (!(hwnd = ancestor(hwnd, GA_ROOT))) { log_line("drag skipped: no root window"); return; }
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
    drag.thread = pthread_self();
    drag.last_dx = drag.last_dy = drag.want_dx = drag.want_dy = 0;
    drag.cursor = pt;
    drag.press_x = press_x;
    drag.press_y = press_y;
    drag.snap_x = drag.start_x;
    drag.snap_y = drag.start_y;
    drag.sent = drag.polls = drag.snapped = 0;
    drag.ended = drag.in_send = drag.saw_leave = 0;
    drag.nevents = 0;
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
    static XSendEvent_fn real;
    static Atom moveresize;

    if (!real) real = (XSendEvent_fn)dlsym(RTLD_NEXT, "XSendEvent");
    if (event && event->type == ClientMessage)
    {
        if (!moveresize)
        {
            XInternAtom_fn intern_atom = x11("XInternAtom");
            if (intern_atom) moveresize = intern_atom(display, "_NET_WM_MOVERESIZE", True);
        }
        if (moveresize && event->xclient.message_type == moveresize)
            log_line("_NET_WM_MOVERESIZE window=%lx pos=(%ld,%ld) direction=%ld button=%ld", event->xclient.window,
                     event->xclient.data.l[0], event->xclient.data.l[1], event->xclient.data.l[2], event->xclient.data.l[3]);
        if (moveresize && event->xclient.message_type == moveresize &&
            event->xclient.data.l[2] == NET_WM_MOVERESIZE_MOVE)
            start_drag(display, event->xclient.window, event->xclient.data.l[0], event->xclient.data.l[1]);
    }
    return real(display, w, propagate, mask, event);
}

/* winex11 reads all X events through XCheckIfEvent; watch for the drag's end */
Bool XCheckIfEvent(Display *display, XEvent *event, Bool (*predicate)(Display *, XEvent *, XPointer), XPointer arg)
{
    static XCheckIfEvent_fn real;
    Bool ret;

    if (!real) real = (XCheckIfEvent_fn)dlsym(RTLD_NEXT, "XCheckIfEvent");
    ret = real(display, event, predicate, arg);
    if (!ret || !drag.active || drag.ended || !pthread_equal(drag.thread, pthread_self())) return ret;

    if (drag.nevents < (int)sizeof(drag.events) - 1)
    {
        drag.events[drag.nevents++] = event->type < 10 ? '0' + event->type : 'a' + (event->type - 10) % 26;
        drag.events[drag.nevents] = 0;
    }
    if (event->type == LeaveNotify) drag.saw_leave = 1;
    else if (event->type == ButtonRelease) { drag.ended = 1; drag.end_reason = "ButtonRelease"; }
    else if (event->type == ButtonPress) { drag.ended = 1; drag.end_reason = "ButtonPress"; }
    /* XWayland only gets pointer motion again once the compositor released its drag */
    else if (event->type == MotionNotify && drag.saw_leave) { drag.ended = 1; drag.end_reason = "MotionNotify"; }
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
    static XQueryPointer_fn real;
    static NtUserMessageCall_fn message_call;
    Bool ret;
    int x, y;

    if (!real) real = (XQueryPointer_fn)dlsym(RTLD_NEXT, "XQueryPointer");
    ret = real(display, w, root, child, root_x, root_y, win_x, win_y, mask);
    if (!ret || !drag.active || drag.ended || !pthread_equal(drag.thread, pthread_self())) goto check_end;

    /* XWayland freezes the pointer while the compositor drags: report where it really
     * is (press point + the delta the compositor applied), as Windows' GetCursorPos would. */
    if (w == DefaultRootWindow(display))
    {
        *win_x = *root_x = drag.press_x + drag.want_dx;
        *win_y = *root_y = drag.press_y + drag.want_dy;
    }
    *mask |= Button1Mask;  /* the compositor owns the pointer; the button is still down */
    if (drag.in_send) return ret;

check_end:
    if (!ret || !drag.active || drag.in_send || !pthread_equal(drag.thread, pthread_self())) return ret;
    drag.polls++;
    if (!drag.ended && now_ms() - drag.start_ms > DRAG_TIMEOUT_MS) { drag.ended = 1; drag.end_reason = "timeout"; }
    if (drag.ended)
    {
        drag.active = 0;
        log_line("drag end hwnd=%p reason=%s pointer-delta=(%d,%d) WM_MOVING=%lu snapped=%lu polls=%lu ms=%ld",
                 drag.hwnd, drag.end_reason, drag.want_dx, drag.want_dy, drag.sent, drag.snapped,
                 drag.polls, now_ms() - drag.start_ms);
        return ret;  /* real state: no button held, winex11 finishes the move */
    }

    if (!window_origin(display, drag.xwin, &x, &y)) return ret;
    /* The compositor places the window at start + pointer delta. Positions equal to
     * where the app last snapped it are the app's own doing: ignore those, so the
     * delta keeps growing with the pointer like Windows' move loop until the
     * palette is dragged past the snap distance. */
    if (x != drag.snap_x || y != drag.snap_y)
    {
        drag.want_dx = x - drag.start_x;
        drag.want_dy = y - drag.start_y;
    }
    int dx = drag.want_dx, dy = drag.want_dy;
    if (dx == drag.last_dx && dy == drag.last_dy) return ret;
    if (!message_call) message_call = w32u("NtUserMessageCall");
    if (!message_call) return ret;

    /* move Wine's cursor like real mouse input would, so GetCursorPos and message
     * positions follow the drag (the app computes its snapping from the cursor) */
    {
        static NtUserCallHwndParam_fn hwnd_param;
        INPUT input = {.type = INPUT_MOUSE};
        struct send_hardware_input_params params = {.flags = SEND_HWMSG_RAWINPUT, .input = &input};
        input.mi.dx = drag.cursor.x + dx;
        input.mi.dy = drag.cursor.y + dy;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        if (!hwnd_param) hwnd_param = w32u("NtUserCallHwndParam");
        if (hwnd_param) hwnd_param(drag.hwnd, (uintptr_t)&params, NTUSER_HWNDPARAM_SEND_HW_INPUT);
    }

    RECT rect = drag.start_rect;
    rect.left += dx; rect.right += dx;
    rect.top += dy;  rect.bottom += dy;
    drag.last_dx = dx;
    drag.last_dy = dy;
    drag.sent++;
    drag.in_send = 1;  /* the window proc may call GetCursorPos -> XQueryPointer */
    message_call(drag.hwnd, WM_MOVING, 0, (intptr_t)&rect, NULL, NTUSER_SEND_MESSAGE, 0);
    drag.in_send = 0;
    /* the app may adjust the rect (snapping); remember where it wants the window */
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
