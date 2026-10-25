/* USER32 and GDI32: one top-level window, its message queue and window procedure, keyboard
   helpers, wsprintfA. The host window itself (SDL) lives in win.c; this file is the Windows
   side the program talks to. */
#define _GNU_SOURCE
#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include "runtime.h"
#include "win.h"

/* ------------------------------------------------------------------ window state */
#define HWND_MAIN 0x00010010u
uint32_t main_hwnd;
static uint32_t wndproc;               /* the program's window procedure */
static char class_name[64];
static int32_t win_x, win_y, win_w, win_h;
static uint32_t win_style, win_exstyle;
static int cursor_count;
static int painted;

/* ------------------------------------------------------------------ message queue */
typedef struct { uint32_t hwnd, msg, wparam, lparam, time; } Msg;
#define QSIZE 1024
static Msg queue[QSIZE];
static int qhead, qtail;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static int quit_posted;
static uint32_t quit_code;

void post_message(uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    pthread_mutex_lock(&qlock);
    int next = (qtail + 1) % QSIZE;
    if (next != qhead) {
        queue[qtail] = (Msg){main_hwnd, msg, wparam, lparam, (uint32_t)now_ms()};
        qtail = next;
    }
    pthread_mutex_unlock(&qlock);
}

static uint32_t send(CPU *c, uint32_t msg, uint32_t w, uint32_t l)
{
    if (!wndproc)
        return 0;
    return guest_call(c, wndproc, 4, main_hwnd, msg, w, l);
}

/* ------------------------------------------------------------------ class, window */
API(USER32, RegisterClassA, 1)
{
    uint32_t wc = ARG(0);
    wndproc = R32(wc + 4);
    snprintf(class_name, sizeof class_name, "%s", gstr(R32(wc + 36)));
    LOG(LOG_WIN, "RegisterClassA(\"%s\", wndproc=%08x, style=%x)", class_name, wndproc, R32(wc));
    return 0xc001;
}

static void frame_size(uint32_t style, int32_t *l, int32_t *t, int32_t *r, int32_t *b)
{
    /* Windows XP classic metrics: caption 18 + frame */
    int fr = 0, cap = 0;
    if (style & 0x00040000u) fr = 4;                         /* WS_THICKFRAME */
    else if (style & 0x00400000u) fr = 3;                    /* WS_DLGFRAME */
    else if (style & 0x00800000u) fr = 1;                    /* WS_BORDER */
    if ((style & 0x00c00000u) == 0x00c00000u) cap = 18;      /* WS_CAPTION */
    *l = fr; *r = fr; *t = fr + cap; *b = fr;
}

static void adjust(uint32_t rect, uint32_t style)
{
    int32_t l, t, r, b;
    frame_size(style, &l, &t, &r, &b);
    W32(rect, R32(rect) - l);
    W32(rect + 4, R32(rect + 4) - t);
    W32(rect + 8, R32(rect + 8) + r);
    W32(rect + 12, R32(rect + 12) + b);
}

API(USER32, AdjustWindowRect, 3) { adjust(ARG(0), ARG(1)); return 1; }
API(USER32, AdjustWindowRectEx, 4) { adjust(ARG(0), ARG(1)); return 1; }

API(USER32, CreateWindowExA, 12)
{
    win_exstyle = ARG(0);
    win_style = ARG(3);
    win_x = (int32_t)ARG(4);
    win_y = (int32_t)ARG(5);
    win_w = (int32_t)ARG(6);
    win_h = (int32_t)ARG(7);
    main_hwnd = HWND_MAIN;
    LOG(LOG_WIN, "CreateWindowExA(\"%s\", \"%s\", style %x, %d,%d %dx%d) -> %x", gstr(ARG(1)),
        gstr(ARG(2)), win_style, win_x, win_y, win_w, win_h, main_hwnd);
    host_window_create(gstr(ARG(2)), win_w, win_h);
    /* WM_NCCREATE / WM_CREATE carry a CREATESTRUCT */
    uint32_t cs = galloc(48);
    W32(cs, ARG(11)); W32(cs + 4, ARG(10)); W32(cs + 8, ARG(9)); W32(cs + 12, ARG(8));
    W32(cs + 16, win_h); W32(cs + 20, win_w); W32(cs + 24, win_y); W32(cs + 28, win_x);
    W32(cs + 32, win_style); W32(cs + 36, ARG(2)); W32(cs + 40, ARG(1)); W32(cs + 44, win_exstyle);
    send(c, 0x0081, 0, cs);              /* WM_NCCREATE */
    send(c, 0x0001, 0, cs);              /* WM_CREATE */
    gfree(cs);
    return main_hwnd;
}

API(USER32, ShowWindow, 2)
{
    LOG(LOG_WIN, "ShowWindow(%x, %u)", ARG(0), ARG(1));
    host_window_show(1);
    send(c, 0x0018, 1, 0);               /* WM_SHOWWINDOW */
    send(c, 0x001c, 1, 0);               /* WM_ACTIVATEAPP */
    send(c, 0x0006, 1, 0);               /* WM_ACTIVATE (WA_ACTIVE) */
    send(c, 0x0007, 0, 0);               /* WM_SETFOCUS */
    send(c, 0x0005, 0, ((uint32_t)win_h << 16) | (uint32_t)win_w);   /* WM_SIZE */
    send(c, 0x0003, 0, ((uint32_t)win_y << 16) | ((uint32_t)win_x & 0xffff)); /* WM_MOVE */
    painted = 0;
    return 0;
}

API(USER32, UpdateWindow, 1)
{
    if (!painted) {
        painted = 1;
        send(c, 0x000f, 0, 0);           /* WM_PAINT */
    }
    return 1;
}

API(USER32, SetWindowPos, 7)
{
    uint32_t flags = ARG(6);
    if (!(flags & 2)) { win_x = (int32_t)ARG(2); win_y = (int32_t)ARG(3); }   /* SWP_NOMOVE */
    if (!(flags & 1)) { win_w = (int32_t)ARG(4); win_h = (int32_t)ARG(5); }   /* SWP_NOSIZE */
    LOG(LOG_WIN, "SetWindowPos(%x, after %x, %d,%d %dx%d, flags %x)", ARG(0), ARG(1),
        (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), flags);
    if (!(flags & 1))
        host_window_resize(win_w, win_h);
    return 1;
}

API(USER32, GetWindowRect, 2)
{
    uint32_t r = ARG(1);
    W32(r, win_x); W32(r + 4, win_y); W32(r + 8, win_x + win_w); W32(r + 12, win_y + win_h);
    return 1;
}

API(USER32, GetClientRect, 2)
{
    int32_t l, t, rr, b;
    frame_size(win_style, &l, &t, &rr, &b);
    uint32_t r = ARG(1);
    W32(r, 0); W32(r + 4, 0); W32(r + 8, win_w - l - rr); W32(r + 12, win_h - t - b);
    return 1;
}

API(USER32, ClientToScreen, 2)
{
    int32_t l, t, rr, b;
    frame_size(win_style, &l, &t, &rr, &b);
    uint32_t p = ARG(1);
    W32(p, R32(p) + win_x + l);
    W32(p + 4, R32(p + 4) + win_y + t);
    return 1;
}

API(USER32, OffsetRect, 3)
{
    uint32_t r = ARG(0);
    int32_t dx = (int32_t)ARG(1), dy = (int32_t)ARG(2);
    W32(r, R32(r) + dx); W32(r + 4, R32(r + 4) + dy);
    W32(r + 8, R32(r + 8) + dx); W32(r + 12, R32(r + 12) + dy);
    return 1;
}

API(USER32, SetWindowTextA, 2)
{
    LOG(LOG_WIN, "SetWindowTextA(\"%s\")", gstr(ARG(1)));
    host_window_title(gstr(ARG(1)));
    return 1;
}

API(USER32, GetSystemMetrics, 1)
{
    int w, h;
    host_desktop_size(&w, &h);
    switch (ARG(0)) {
    case 0: return w;                    /* SM_CXSCREEN */
    case 1: return h;                    /* SM_CYSCREEN */
    case 4: return 18;                   /* SM_CYCAPTION */
    case 5: case 6: return 1;            /* border */
    case 7: case 8: return 3;            /* dialog frame */
    case 32: case 33: return 4;          /* sizing frame */
    default:
        LOG(LOG_WIN, "GetSystemMetrics(%u) -> 0", ARG(0));
        return 0;
    }
}

/* ------------------------------------------------------------------ messages */
API(USER32, PeekMessageA, 5)
{
    uint32_t m = ARG(0), remove = ARG(4) & 1;
    host_pump();
    pthread_mutex_lock(&qlock);
    if (qhead == qtail) {
        int quit = quit_posted;
        if (quit && remove) quit_posted = 0;
        pthread_mutex_unlock(&qlock);
        if (quit) {
            W32(m, main_hwnd); W32(m + 4, 0x0012); W32(m + 8, quit_code); W32(m + 12, 0);
            W32(m + 16, (uint32_t)now_ms()); W32(m + 20, 0); W32(m + 24, 0);
            return 1;
        }
        return 0;
    }
    Msg q = queue[qhead];
    if (remove)
        qhead = (qhead + 1) % QSIZE;
    pthread_mutex_unlock(&qlock);
    W32(m, q.hwnd); W32(m + 4, q.msg); W32(m + 8, q.wparam); W32(m + 12, q.lparam);
    W32(m + 16, q.time); W32(m + 20, 0); W32(m + 24, 0);
    LOG(LOG_WIN, "PeekMessageA -> msg %x w=%x l=%x", q.msg, q.wparam, q.lparam);
    return 1;
}

API(USER32, TranslateMessage, 1)
{
    uint32_t m = ARG(0), msg = R32(m + 4), vk = R32(m + 8);
    if (msg == 0x0100 || msg == 0x0104) {          /* WM_KEYDOWN / WM_SYSKEYDOWN */
        int ch = vk_to_char(vk, host_shift_down());
        if (ch)
            post_message(msg == 0x0100 ? 0x0102 : 0x0106, ch, R32(m + 12));
    }
    return 0;
}

API(USER32, DispatchMessageA, 1)
{
    uint32_t m = ARG(0);
    if (!wndproc || R32(m + 4) == 0x0012)
        return 0;
    return guest_call(c, wndproc, 4, R32(m), R32(m + 4), R32(m + 8), R32(m + 12));
}

API(USER32, SendMessageA, 4)
{
    LOG(LOG_WIN, "SendMessageA(%x, %x, %x, %x)", ARG(0), ARG(1), ARG(2), ARG(3));
    return guest_call(c, wndproc, 4, ARG(0), ARG(1), ARG(2), ARG(3));
}

API(USER32, PostQuitMessage, 1)
{
    LOG(LOG_WIN, "PostQuitMessage(%u)", ARG(0));
    pthread_mutex_lock(&qlock);
    quit_posted = 1;
    quit_code = ARG(0);
    pthread_mutex_unlock(&qlock);
    return 0;
}

API(USER32, DefWindowProcA, 4)
{
    uint32_t msg = ARG(1);
    if (msg == 0x0010) {                 /* WM_CLOSE: DestroyWindow -> WM_DESTROY */
        guest_call(c, wndproc, 4, main_hwnd, 0x0002, 0, 0);
        return 0;
    }
    if (msg == 0x0081) return 1;         /* WM_NCCREATE */
    return 0;
}

/* ------------------------------------------------------------------ painting, GDI */
#define FAKE_DC 0x0001d0c0u
API(USER32, GetDC, 1) { return FAKE_DC; }
API(USER32, ReleaseDC, 2) { return 1; }

API(USER32, BeginPaint, 2)
{
    uint32_t ps = ARG(1);
    memset(gptr(ps), 0, 64);
    W32(ps, FAKE_DC);
    W32(ps + 16, win_w);
    W32(ps + 20, win_h);
    return FAKE_DC;
}

API(USER32, EndPaint, 2) { return 1; }

static uint32_t selected_bitmap;

API(GDI32, CreateCompatibleDC, 1) { return FAKE_DC + 4; }
API(GDI32, DeleteDC, 1) { return 1; }
API(GDI32, DeleteObject, 1) { return 1; }
API(GDI32, SelectObject, 2)
{
    uint32_t old = selected_bitmap;
    selected_bitmap = ARG(1);
    return old ? old : 0x0001b000u;
}

API(GDI32, BitBlt, 9)
{
    LOG(LOG_WIN, "BitBlt(dst %x %d,%d %dx%d from %x %d,%d rop %x)", ARG(0), (int)ARG(1), (int)ARG(2),
        (int)ARG(3), (int)ARG(4), ARG(5), (int)ARG(6), (int)ARG(7), ARG(8));
    if (ARG(5) == FAKE_DC + 4 && selected_bitmap)
        host_show_bitmap(selected_bitmap, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4));
    return 1;
}

/* LoadBitmapA: the splash screen. Our handle is the resource's address in the image. */
API(USER32, LoadBitmapA, 2)
{
    uint32_t data, size;
    if (!resource_find(2, ARG(1), &data, &size)) {
        LOG(LOG_WIN, "LoadBitmapA(%u) -> not found", ARG(1));
        return 0;
    }
    LOG(LOG_WIN, "LoadBitmapA(%u) -> %08x (%u bytes)", ARG(1), data, size);
    return data;
}

API(USER32, LoadIconA, 2) { return 0x00011000u + (ARG(1) & 0xffff); }
API(USER32, LoadCursorA, 2) { return 0x00012000u + (ARG(1) & 0xffff); }

API(USER32, SetCursor, 1) { return 0; }

API(USER32, ShowCursor, 1)
{
    cursor_count += ARG(0) ? 1 : -1;
    host_cursor_visible(cursor_count >= 0);
    return (uint32_t)cursor_count;
}

API(USER32, MessageBoxA, 4)
{
    const char *text = gstr(ARG(1)), *cap = gstr(ARG(2));
    LOG(LOG_ALWAYS, "MessageBoxA(\"%s\", \"%s\", %x)", cap ? cap : "", text ? text : "", ARG(3));
    host_message_box(cap ? cap : "Test Drive 6", text ? text : "");
    return 1;                            /* IDOK */
}

API(USER32, DialogBoxParamA, 5)
{
    LOG(LOG_ALWAYS, "DialogBoxParamA(template %u): dialogs are not supported", ARG(1));
    return 0xffffffffu;
}

API(USER32, EndDialog, 2) { return 1; }
API(USER32, SetDlgItemTextA, 3) { return 1; }

/* ------------------------------------------------------------------ keyboard */
API(USER32, GetKeyboardState, 1)
{
    for (int vk = 0; vk < 256; vk++)
        W8(ARG(0) + vk, host_vk_down(vk) ? 0x80 : 0);
    return 1;
}

API(USER32, GetAsyncKeyState, 1)
{
    if ((ARG(0) & 0xff) == 0x1b && video_skip_taken())  /* VK_ESCAPE: an intro video's skip (video.c) */
        return 0x8000;
    return host_vk_down(ARG(0) & 0xff) ? 0x8000 : 0;
}

API(USER32, GetKeyboardLayout, 1) { return 0x04090409u; }

API(USER32, MapVirtualKeyExA, 3)
{
    uint32_t code = ARG(0), type = ARG(1);
    switch (type) {
    case 0: return vk_to_scan(code);     /* MAPVK_VK_TO_VSC */
    case 1: return scan_to_vk(code);     /* MAPVK_VSC_TO_VK */
    case 2: return vk_to_char(code, 0);  /* MAPVK_VK_TO_CHAR */
    case 3: return scan_to_vk(code);
    }
    return 0;
}

API(USER32, ToAsciiEx, 6)
{
    uint32_t vk = ARG(0), state = ARG(2);
    int shift = state && ((R8(state + 0x10) & 0x80) || (R8(state + 0xa0) & 0x80) || (R8(state + 0xa1) & 0x80));
    int ch = vk_to_char(vk, shift);
    if (!ch) return 0;
    W16(ARG(3), (uint16_t)ch);
    return 1;
}

API(USER32, GetKeyNameTextA, 3)
{
    uint32_t scan = (ARG(0) >> 16) & 0x1ff;
    const char *n = scan_name(scan);
    gstrncpy(ARG(1), n, ARG(2));
    return strlen(gptr(ARG(1)));
}

/* ------------------------------------------------------------------ wsprintfA */
/* Windows' own printf subset: %[-][#][0][width][.prec][l|h]{d,i,u,x,X,c,C,s,S} (no floats). */
int wsprintf_impl(char *out, size_t cap, const char *fmt, uint32_t args)
{
    size_t k = 0;
    for (const char *p = fmt; *p && k + 1 < cap; p++) {
        if (*p != '%') { out[k++] = *p; continue; }
        p++;
        if (*p == '%') { out[k++] = '%'; continue; }
        char spec[32] = "%";
        int s = 1;
        while (*p == '-' || *p == '#' || *p == '0' || *p == '+' || *p == ' ')
            spec[s++] = *p++;
        while (isdigit((unsigned char)*p) && s < 20) spec[s++] = *p++;
        if (*p == '.') { spec[s++] = *p++; while (isdigit((unsigned char)*p) && s < 28) spec[s++] = *p++; }
        while (*p == 'l' || *p == 'h') p++;
        char conv = *p;
        if (!conv) break;
        char tmp[512];
        uint32_t v = R32(args);
        args += 4;
        switch (conv) {
        case 'd': case 'i': spec[s++] = 'd'; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, (int32_t)v); break;
        case 'u': spec[s++] = 'u'; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, v); break;
        case 'x': case 'X': spec[s++] = conv; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, v); break;
        case 'c': case 'C': spec[s++] = 'c'; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, (int)(v & 0xff)); break;
        case 's': case 'S': spec[s++] = 's'; spec[s] = 0; snprintf(tmp, sizeof tmp, spec, v ? gstr(v) : "(null)"); break;
        default: tmp[0] = 0; args -= 4; break;
        }
        for (char *t = tmp; *t && k + 1 < cap; t++)
            out[k++] = *t;
    }
    out[k] = 0;
    return (int)k;
}

API_CDECL(USER32, wsprintfA)
{
    char buf[1025];
    int n = wsprintf_impl(buf, sizeof buf, gstr(ARG(1)), c->esp + 12);
    memcpy(gptr(ARG(0)), buf, n + 1);
    return n;
}

/* ------------------------------------------------------------------ TD6's further needs */
API(USER32, RegisterClassExA, 1)
{
    uint32_t wc = ARG(0);                /* WNDCLASSEXA: cbSize first, then WNDCLASSA's fields */
    wndproc = R32(wc + 8);
    snprintf(class_name, sizeof class_name, "%s", gstr(R32(wc + 40)));
    LOG(LOG_WIN, "RegisterClassExA(\"%s\", wndproc=%08x, style=%x)", class_name, wndproc, R32(wc + 4));
    return 0xc001;
}

API(USER32, wvsprintfA, 3)
{
    char buf[1025];
    int n = wsprintf_impl(buf, sizeof buf, gstr(ARG(1)), ARG(2));
    memcpy(gptr(ARG(0)), buf, n + 1);
    return n;
}

API(USER32, SetRect, 5)
{
    W32(ARG(0), ARG(1));
    W32(ARG(0) + 4, ARG(2));
    W32(ARG(0) + 8, ARG(3));
    W32(ARG(0) + 12, ARG(4));
    return 1;
}

/* The game's keyboard shortcuts (accelerator table 0x71) are never translated: the window
   procedure sees the keys themselves */
API(USER32, LoadAcceleratorsA, 2) { return 0x00013000u + (ARG(1) & 0xffff); }
API(USER32, TranslateAcceleratorA, 3) { return 0; }
API(USER32, SetFocus, 1) { return ARG(0); }
API(USER32, EnableWindow, 2) { return 0; }
API(USER32, DestroyWindow, 1) { return 1; }
API(USER32, GetDesktopWindow, 0) { return 0x00010010u; }
API(USER32, RedrawWindow, 4) { return 1; }
API(USER32, SetWindowLongA, 3) { return 0; }
API(USER32, SetCursorPos, 2) { return 1; }

/* The "Properties for Test Drive 6" dialog: the game is started with its own NoProps switch
   (GetCommandLineA), so it is never asked for; should it be, it closes as with OK */
API(COMCTL32, PropertySheetA, 1)
{
    LOG(LOG_ALWAYS, "PropertySheetA: the game's properties dialog is not shown");
    return 1;
}
API(USER32, GetDlgItem, 2) { return 0; }
API(USER32, GetDlgItemTextA, 4) { if (ARG(3)) W8(ARG(2), 0); return 0; }
API(USER32, SendDlgItemMessageA, 5) { return 0; }
API(USER32, CheckDlgButton, 3) { return 1; }
API(USER32, IsDlgButtonChecked, 2) { return 0; }

/* GDI: the screen's depth; text is drawn only by a debug info screen and the frame-rate overlay
   (nothing shows) */
API(GDI32, GetDeviceCaps, 2)
{
    switch (ARG(1)) {
    case 8: return 640;                  /* HORZRES */
    case 10: return 480;                 /* VERTRES */
    case 12: return 32;                  /* BITSPIXEL */
    case 14: return 1;                   /* PLANES */
    case 24: return 0;                   /* NUMCOLORS (-1 on true colour; 0 harmless) */
    case 116: return 60;                 /* VREFRESH */
    default: return 0;
    }
}
API(GDI32, GetStockObject, 1) { return 0x00014000u + (ARG(0) & 0xff); }
API(GDI32, GetSystemPaletteEntries, 4)
{
    if (ARG(3)) memset(gptr(ARG(3)), 0, 4 * ARG(2));
    return ARG(2);
}
API(GDI32, CreateFontA, 14) { return 0x00015000u; }
API(GDI32, SetTextColor, 2) { return 0; }
API(GDI32, SetBkColor, 2) { return 0; }
API(GDI32, SetBkMode, 2) { return 1; }
API(GDI32, TextOutA, 5) { return 1; }
API(GDI32, ExtTextOutA, 8) { return 1; }
static uint32_t text_extent(uint32_t n, uint32_t size)
{
    W32(size, 10 * n);                   /* a 24-pixel Arial's average */
    W32(size + 4, 24);
    return 1;
}
API(GDI32, GetTextExtentPointA, 4) { return text_extent(ARG(2), ARG(3)); }
API(GDI32, GetTextExtentPoint32A, 4) { return text_extent(ARG(2), ARG(3)); }
