/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecuplw - Windows front end for freecupl and freecsim: editor,
   compile and simulate commands, message pane, viewers for the output
   files.  The tools are run as separate programs from the directory of
   this one.  Windows XP SP3 and later.  DARKSOFT. */
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <tom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "proc.h"
#include "resource.h"
#include "../version.h"

#define APPNAME     "FreeCUPL"
#define CLASSNAME   "FreeCUPLW"
#define WM_APP_LINE (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define MAXDOCS     32
#define COLOUR_MAX  400000L     /* characters; larger files stay plain */
#define TIMER_COLOUR 1         /* scan after typing */
#define TIMER_SLICE 2          /* colour another part of the text */

enum { DOC_SRC, DOC_VIEW, DOC_HELP };
enum { STEP_COMPILE, STEP_SIM };

typedef struct {
    HWND edit;
    char path[MAX_PATH];        /* "" until saved */
    char title[80];
    int kind;
    int lf;                     /* LF line ends on disk */
    int shownmod;
    int loading;
    FILETIME ft;
    ITextDocument *tom;
    unsigned char *text, *attr, *want;  /* colouring state */
    long ntext;
    long sync;                  /* next position to colour, -1: done */
    int pass;
    int viasel;                 /* colours are set through the selection */
    int slow;                   /* slow range calls seen */
} Doc;

typedef struct {
    char path[MAX_PATH];
    int existed;
    FILETIME ft;
} Snap;

static const GUID iid_textdoc =
    { 0x8cc497c0, 0xa1df, 0x11ce, { 0x80, 0x98, 0x00, 0xaa, 0x00, 0x47, 0xbe, 0x5d } };

static HINSTANCE g_inst;
static HWND g_main, g_tool, g_status, g_tab, g_list, g_find;
static HACCEL g_accel;
static HFONT g_listfont, g_uifont;
static int g_dpi = 96;
static Doc *g_docs[MAXDOCS];
static int g_ndocs, g_cur = -1;
static int g_newcount;
static char g_exedir[MAX_PATH], g_inipath[MAX_PATH];
static GuiIni *g_ini;
static char g_recent[GUI_NRECENT][GUI_PATH];
static int g_recentsep;

static char g_face[LF_FACESIZE] = "Courier New";
static int g_fontset;           /* the font comes from the settings file */
static BYTE g_charset = ANSI_CHARSET;
static int g_elevated;
static int g_pt = 10, g_tabw = 8, g_colour = 1, g_colour_ok = 1;
static int g_msgh = 140, g_split, g_dragging;

static FINDREPLACEA g_fr;
static char g_findwhat[256], g_replwith[256];
static UINT g_findmsg;
static DWORD g_findflags = FR_DOWN;

static char **g_msgfiles;
static int g_nmsgfiles;
static int g_msgextent;
static char g_lastpld[MAX_PATH];

static struct {
    int active, stopped;
    int queue[2], nq, qi, flags;
    Proc proc;
    HANDLE thread;
    char pld[MAX_PATH], si[MAX_PATH], dir[MAX_PATH];
    GuiOpts o;
    int nerr, nwarn;
    int fpld, fsi;              /* message file numbers of the sources */
    Snap snap[32];
    int nsnap;
    FILETIME start;
} g_run;

static void doc_activate(int i);
static int doc_save(Doc *d, int saveas);
static int open_file(const char *path);
static void update_ui(void);
static void colour_scan(Doc *d, int full);

/* ---- small helpers ---- */

static FARPROC lib_proc(const char *dll, const char *name)
{
    HMODULE m = LoadLibraryA(dll);
    return m ? GetProcAddress(m, name) : NULL;
}

/* Is the program running with administrator rights (Vista and later)?
   Windows then keeps Explorer, which is not, from dragging to it. */
static void elevation(void)
{
    typedef BOOL (WINAPI *OpenTok)(HANDLE, DWORD, HANDLE *);
    typedef BOOL (WINAPI *GetTok)(HANDLE, int, void *, DWORD, DWORD *);
    OpenTok open_tok = (OpenTok)lib_proc("advapi32.dll", "OpenProcessToken");
    GetTok get_tok = (GetTok)lib_proc("advapi32.dll", "GetTokenInformation");
    HANDLE tok;
    DWORD elev = 0, n = 0;
    if (!open_tok || !get_tok || !open_tok(GetCurrentProcess(), 0x0008 /* TOKEN_QUERY */, &tok))
        return;
    /* 20: TokenElevation; fails before Vista */
    if (get_tok(tok, 20, &elev, sizeof elev, &n)) g_elevated = elev != 0;
    CloseHandle(tok);
}

/* Let WM_DROPFILES through from a program with fewer rights (Windows 7:
   per window; Vista: for the process).  Without it nothing can be
   dropped on a program run as administrator. */
static void drop_allow(HWND h)
{
    typedef BOOL (WINAPI *FilterEx)(HWND, UINT, DWORD, void *);
    typedef BOOL (WINAPI *Filter)(UINT, DWORD);
    static const UINT msgs[] = { WM_DROPFILES, WM_COPYDATA, 0x0049 /* WM_COPYGLOBALDATA */ };
    FilterEx fex = (FilterEx)lib_proc("user32.dll", "ChangeWindowMessageFilterEx");
    Filter f = (Filter)lib_proc("user32.dll", "ChangeWindowMessageFilter");
    size_t i;
    for (i = 0; i < sizeof msgs / sizeof msgs[0]; i++) {
        if (fex) fex(h, msgs[i], 1 /* MSGFLT_ALLOW */, NULL);
        else if (f) f(msgs[i], 1 /* MSGFLT_ADD */);
    }
}

/* The edit control is a drop target of its own (OLE): it takes dragged
   text, and for files it asks the parent with EN_DROPFILES.  A read-only
   control refuses every drop, and so does Windows between a program run
   as administrator and Explorer.  In both cases the control's target is
   removed; the drop then comes as WM_DROPFILES like anywhere else on the
   window. */
static void drop_plain(HWND edit)
{
    typedef HRESULT (WINAPI *Revoke)(HWND);
    Revoke revoke = (Revoke)lib_proc("ole32.dll", "RevokeDragDrop");
    if (revoke) revoke(edit);
}

static int scale(int px)
{
    return MulDiv(px, g_dpi, 96);
}

static Doc *cur(void)
{
    return g_cur >= 0 ? g_docs[g_cur] : NULL;
}

static int msgbox(UINT flags, const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return MessageBoxA(g_main, b, APPNAME, flags);
}

static void status(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    SendMessage(g_status, SB_SETTEXTA, 0, (LPARAM)b);
}

static int file_time(const char *path, FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a) ||
        (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    if (ft) *ft = a.ftLastWriteTime;
    return 1;
}

static void errtext(DWORD err, char *out, size_t n)
{
    size_t l;
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL, err, 0, out, (DWORD)n, NULL))
        snprintf(out, n, "error %lu", (unsigned long)err);
    l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == '.' ||
                 out[l - 1] == ' '))
        out[--l] = 0;
}

/* ---- settings ---- */

static void settings_load(void)
{
    int i;
    const char *s;
    g_ini = gui_ini_load(g_inipath);
    if (!g_ini) return;
    s = gui_ini_get(g_ini, "editor", "font", "");
    if (*s) {
        lstrcpynA(g_face, s, sizeof g_face);
        g_fontset = 1;
    }
    g_pt = gui_ini_int(g_ini, "editor", "size", g_pt);
    if (g_pt < 6 || g_pt > 72) g_pt = 10;
    g_tabw = gui_ini_int(g_ini, "editor", "tab", g_tabw);
    if (g_tabw < 1 || g_tabw > 16) g_tabw = 8;
    g_colour = gui_ini_int(g_ini, "editor", "colour", 1) != 0;
    g_msgh = scale(gui_ini_int(g_ini, "window", "messages", 140));
    for (i = 0; i < GUI_NRECENT; i++) {
        char k[16];
        snprintf(k, sizeof k, "file%d", i + 1);
        lstrcpynA(g_recent[i], gui_ini_get(g_ini, "recent", k, ""), GUI_PATH);
    }
}

static void settings_save(void)
{
    WINDOWPLACEMENT wp;
    int i;
    if (!g_ini) return;
    wp.length = sizeof wp;
    if (g_main && GetWindowPlacement(g_main, &wp)) {
        gui_ini_set_int(g_ini, "window", "x", wp.rcNormalPosition.left);
        gui_ini_set_int(g_ini, "window", "y", wp.rcNormalPosition.top);
        gui_ini_set_int(g_ini, "window", "w",
                        wp.rcNormalPosition.right - wp.rcNormalPosition.left);
        gui_ini_set_int(g_ini, "window", "h",
                        wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
        gui_ini_set_int(g_ini, "window", "max", wp.showCmd == SW_SHOWMAXIMIZED);
    }
    gui_ini_set_int(g_ini, "window", "messages", MulDiv(g_msgh, 96, g_dpi));
    /* the default font is not written, so that it stays the default */
    if (g_fontset) {
        gui_ini_set(g_ini, "editor", "font", g_face);
        gui_ini_set_int(g_ini, "editor", "size", g_pt);
    }
    gui_ini_set_int(g_ini, "editor", "tab", g_tabw);
    gui_ini_set_int(g_ini, "editor", "colour", g_colour);
    for (i = 0; i < GUI_NRECENT; i++) {
        char k[16];
        snprintf(k, sizeof k, "file%d", i + 1);
        gui_ini_set(g_ini, "recent", k, g_recent[i]);
    }
    /* a read-only directory: the settings are not kept */
    gui_ini_save(g_ini, g_inipath);
}

static void opts_for(const char *pld, GuiOpts *o)
{
    char sec[MAX_PATH + 8];
    gui_file_section(pld, sec, sizeof sec);
    if (pld[0] && gui_ini_has_section(g_ini, sec)) gui_opts_load(g_ini, sec, o);
    else gui_opts_load(g_ini, "options", o);
}

/* ---- editor formatting ---- */

/* WM_SETREDRAW shows the window when drawing is turned on again, and
   only the editor of the active file is visible. */
static int redraw_off(Doc *d)
{
    if (!IsWindowVisible(d->edit)) return 0;
    SendMessage(d->edit, WM_SETREDRAW, FALSE, 0);
    return 1;
}

static void redraw_on(Doc *d, int was_off)
{
    if (!was_off) return;
    SendMessage(d->edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->edit, NULL, TRUE);
}

typedef struct {
    CHARRANGE sel;
    POINT sp;
    long first;
    LRESULT mask, mod;
    int redraw, moved, keep;
} Fmt;

static void fmt_begin(Doc *d, Fmt *s, int quiet)
{
    s->mask = SendMessage(d->edit, EM_SETEVENTMASK, 0, 0);
    s->redraw = quiet && redraw_off(d);
    s->moved = s->keep = 0;
    s->first = (long)SendMessage(d->edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessage(d->edit, EM_EXGETSEL, 0, (LPARAM)&s->sel);
    SendMessage(d->edit, EM_GETSCROLLPOS, 0, (LPARAM)&s->sp);
    s->mod = SendMessage(d->edit, EM_GETMODIFY, 0, 0);
    if (d->tom) ITextDocument_Undo(d->tom, tomSuspend, NULL);
}

static void fmt_end(Doc *d, Fmt *s)
{
    if (s->moved) {
        long now;
        SendMessage(d->edit, EM_EXSETSEL, 0, (LPARAM)&s->sel);
        SendMessage(d->edit, EM_SETSCROLLPOS, 0, (LPARAM)&s->sp);
        /* EM_SETSCROLLPOS of RichEdit 3.0 goes wrong past 65535 pixels */
        now = (long)SendMessage(d->edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        if (now != s->first) SendMessage(d->edit, EM_LINESCROLL, 0, s->first - now);
    }
    if (d->tom) ITextDocument_Undo(d->tom, tomResume, NULL);
    else SendMessage(d->edit, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessage(d->edit, EM_SETMODIFY, (WPARAM)s->mod, 0);
    if (s->redraw && s->keep) SendMessage(d->edit, WM_SETREDRAW, TRUE, 0);
    else redraw_on(d, s->redraw);
    SendMessage(d->edit, EM_SETEVENTMASK, 0, s->mask);
}

/* The one font of the editors and the message pane: face and size of
   the settings, the character set of the system code page (ANSI_CHARSET
   on a western system), fixed pitch. */
static HFONT text_font(void)
{
    return CreateFontA(-MulDiv(g_pt, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                       g_charset, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, g_face);
}

static int CALLBACK font_found(const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM l)
{
    (void)lf;
    (void)tm;
    (void)type;
    *(int *)l = 1;
    return 0;
}

/* Consolas where it is installed (Vista and later), Courier New on XP */
static void font_default(void)
{
    CHARSETINFO ci;
    LOGFONTA lf;
    HDC dc = GetDC(NULL);
    int found = 0;
    if (TranslateCharsetInfo((DWORD *)(DWORD_PTR)GetACP(), &ci, TCI_SRCCODEPAGE))
        g_charset = (BYTE)ci.ciCharset;
    if (!g_fontset) {
        memset(&lf, 0, sizeof lf);
        lf.lfCharSet = DEFAULT_CHARSET;
        lstrcpyA(lf.lfFaceName, "Consolas");
        EnumFontFamiliesExA(dc, &lf, font_found, (LPARAM)&found, 0);
        lstrcpyA(g_face, found ? "Consolas" : "Courier New");
        g_pt = 10;
    }
    ReleaseDC(NULL, dc);
}

static void list_font(void)
{
    HDC dc;
    HFONT old;
    TEXTMETRICA tm;
    if (g_listfont) DeleteObject(g_listfont);
    g_listfont = text_font();
    if (!g_list) return;
    SendMessage(g_list, WM_SETFONT, (WPARAM)g_listfont, TRUE);
    dc = GetDC(g_list);
    old = SelectObject(dc, g_listfont);
    if (GetTextMetricsA(dc, &tm)) SendMessage(g_list, LB_SETITEMHEIGHT, 0, tm.tmHeight + 2);
    SelectObject(dc, old);
    ReleaseDC(g_list, dc);
    InvalidateRect(g_list, NULL, TRUE);
}

/* width of one character of the editor font, in twips */
static int char_twips(void)
{
    HDC dc = GetDC(g_main);
    HFONT f = text_font();
    HFONT old = SelectObject(dc, f);
    SIZE sz;
    int tw = 120;
    if (GetTextExtentPoint32A(dc, "0000000000", 10, &sz) && sz.cx > 0)
        tw = MulDiv(sz.cx, 1440, g_dpi * 10);
    SelectObject(dc, old);
    DeleteObject(f);
    ReleaseDC(g_main, dc);
    return tw;
}

/* font and tab stops of the whole text */
static void doc_format(Doc *d)
{
    CHARFORMAT2A cf;
    PARAFORMAT pf;
    CHARRANGE all = { 0, -1 };
    Fmt s;
    int i, tw = char_twips() * g_tabw;

    fmt_begin(d, &s, 1);
    /* the text there is and, as the default format, the text to come */
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE |
                CFM_STRIKEOUT | CFM_CHARSET | CFM_WEIGHT | CFM_OFFSET | CFM_SPACING;
    cf.yHeight = g_pt * 20;
    cf.wWeight = FW_NORMAL;
    cf.bCharSet = g_charset;
    cf.bPitchAndFamily = FIXED_PITCH | FF_MODERN;
    lstrcpynA(cf.szFaceName, g_face, LF_FACESIZE);
    SendMessage(d->edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessage(d->edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);

    memset(&pf, 0, sizeof pf);
    pf.cbSize = sizeof pf;
    pf.dwMask = PFM_TABSTOPS;
    pf.cTabCount = MAX_TAB_STOPS;
    for (i = 0; i < MAX_TAB_STOPS; i++) pf.rgxTabs[i] = (i + 1) * tw;
    SendMessage(d->edit, EM_EXSETSEL, 0, (LPARAM)&all);
    SendMessage(d->edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    /* past the last stop */
    if (d->tom) ITextDocument_SetDefaultTabStop(d->tom, (float)tw / 20.0f);
    s.moved = 1;
    fmt_end(d, &s);
}

/* The text with one byte per character position, CR line ends; bytes
   over 127 stand for any other character. */
static unsigned char *doc_text(Doc *d, long *n)
{
    GETTEXTLENGTHEX gl;
    GETTEXTEX gt;
    WCHAR *w;
    unsigned char *t;
    long len, i;

    gl.flags = GTL_NUMCHARS | GTL_PRECISE;
    gl.codepage = 1200;
    len = (long)SendMessage(d->edit, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    if (len < 0) len = 0;
    w = malloc(((size_t)len + 1) * sizeof *w);
    if (!w) return NULL;
    memset(&gt, 0, sizeof gt);
    gt.cb = (DWORD)((len + 1) * (long)sizeof *w);
    gt.flags = GT_DEFAULT;
    gt.codepage = 1200;
    w[0] = 0;
    len = (long)SendMessage(d->edit, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)w);
    t = malloc((size_t)len + 1);
    if (t) {
        for (i = 0; i < len; i++) t[i] = w[i] < 128 ? (unsigned char)w[i] : 0x80;
        t[len] = 0;
        *n = len;
    }
    free(w);
    return t;
}

/* ---- syntax colouring ---- */

/* The control keeps a colour per character.  text is the text as last
   scanned, want the class of each character, attr the class the
   control has been given (ATTR_NONE: not known, after an edit).  Where
   the two differ the colour is set: at once near an edit and at the top
   of a new file, the rest in slices from a timer, so that a long file
   can be used while it is coloured.

   A colour is set through a text range, which leaves the selection and
   the scroll position alone.  Where that is slow (Wine: each call takes
   time in proportion to the length of the text) the selection is used
   instead, with drawing off, and put back afterwards. */
#define ATTR_NONE 0xff

static const COLORREF g_attrcol[] = {
    0, RGB(0, 128, 0), RGB(0, 0, 200), RGB(160, 0, 96)
};

static void set_colour(Doc *d, Fmt *s, long a, long b, int attr)
{
    ITextRange *r = NULL;
    ITextFont *f = NULL;
    HRESULT hr = E_FAIL;
    if (d->tom && !d->viasel) hr = ITextDocument_Range(d->tom, a, b, &r);
    if (SUCCEEDED(hr) && r) {
        hr = ITextRange_GetFont(r, &f);
        if (SUCCEEDED(hr) && f) {
            hr = ITextFont_SetForeColor(f, attr ? (LONG)g_attrcol[attr] : tomAutoColor);
            ITextFont_Release(f);
        }
        ITextRange_Release(r);
    }
    if (FAILED(hr)) {
        /* through the selection */
        CHARFORMATA cf;
        CHARRANGE cr;
        cr.cpMin = a;
        cr.cpMax = b;
        memset(&cf, 0, sizeof cf);
        cf.cbSize = sizeof cf;
        cf.dwMask = CFM_COLOR;
        if (attr) cf.crTextColor = g_attrcol[attr];
        else cf.dwEffects = CFE_AUTOCOLOR;
        SendMessage(d->edit, EM_EXSETSEL, 0, (LPARAM)&cr);
        SendMessage(d->edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        s->moved = 1;
    }
}

static void colour_drop(Doc *d)
{
    free(d->text);
    free(d->attr);
    free(d->want);
    d->text = d->attr = d->want = NULL;
    d->ntext = 0;
    d->sync = -1;
}

static int colour_wanted(const Doc *d)
{
    return g_colour && g_colour_ok && d->tom && d->kind == DOC_SRC;
}

static void colour_off(Doc *d)
{
    Fmt s;
    CHARFORMATA cf;
    if (!d->text) return;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR;
    cf.dwEffects = CFE_AUTOCOLOR;
    fmt_begin(d, &s, 1);
    SendMessage(d->edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    fmt_end(d, &s);
    colour_drop(d);
}

/* Set the colours that differ in [from, to), at most budget runs.
   Returns where it stopped. */
static long colour_sync(Doc *d, long from, long to, int budget, int quiet)
{
    Fmt s;
    long i = from, j, lo = -1, hi = -1;
    int began = 0, undo = 0;
    while (i < to) {
        int a = d->want[i];
        DWORD t0;
        if (d->attr[i] == a) {
            i++;
            continue;
        }
        if (budget-- <= 0) break;
        j = i + 1;
        while (j < to && d->want[j] == a && d->attr[j] != a) j++;
        if (!began) {
            undo = (int)SendMessage(d->edit, EM_CANUNDO, 0, 0);
            fmt_begin(d, &s, quiet || d->viasel);
            began = 1;
            lo = i;
        }
        t0 = GetTickCount();
        set_colour(d, &s, i, j, a);
        memset(d->attr + i, a, (size_t)(j - i));
        hi = i = j;
        if (!d->viasel && GetTickCount() - t0 >= 30 && ++d->slow >= 3) {
            /* the other way from the next call on */
            d->viasel = 1;
            break;
        }
    }
    if (began) {
        if (s.redraw && !quiet) {
            /* drawn again only when the change is on the screen */
            POINTL pt;
            RECT rc;
            long top = (long)SendMessage(d->edit, EM_LINEINDEX, (WPARAM)s.first, 0), bot;
            GetClientRect(d->edit, &rc);
            pt.x = rc.right;
            pt.y = rc.bottom;
            bot = (long)SendMessage(d->edit, EM_CHARFROMPOS, 0, (LPARAM)&pt);
            if (hi <= top || lo > bot) s.keep = 1;
        }
        fmt_end(d, &s);
        if (!undo && SendMessage(d->edit, EM_CANUNDO, 0, 0)) {
            /* the control took the colours as an undo step: no
               colouring with this control */
            SendMessage(d->edit, EM_EMPTYUNDOBUFFER, 0, 0);
            g_colour_ok = 0;
            return to;
        }
    }
    return i;
}

/* Scan the text again after a change (full: all of it is plain, after
   loading or when colouring is turned on). */
static void colour_scan(Doc *d, int full)
{
    unsigned char *text, *attr, *want;
    long n = 0, a = 0, b = 0;

    if (!colour_wanted(d)) {
        colour_off(d);
        return;
    }
    text = doc_text(d, &n);
    if (!text) return;
    if (n > COLOUR_MAX) {
        free(text);
        colour_off(d);
        return;
    }
    attr = malloc((size_t)n + 1);
    want = malloc((size_t)n + 1);
    if (!attr || !want) {
        free(text);
        free(attr);
        free(want);
        return;
    }
    gui_scan(text, (size_t)n, want);
    if (full || !d->text) {
        /* nothing is coloured: the text was just read, or colouring
           was off */
        memset(attr, GA_PLAIN, (size_t)n);
        b = n;
    } else {
        size_t p, sfx;
        gui_diff(d->text, (size_t)d->ntext, text, (size_t)n, &p, &sfx);
        a = (long)p;
        b = n - (long)sfx;
        /* new text takes the colour of its neighbour */
        memcpy(attr, d->attr, p);
        memset(attr + a, ATTR_NONE, (size_t)(b - a));
        memcpy(attr + b, d->attr + (d->ntext - (long)sfx), sfx);
    }
    colour_drop(d);
    d->text = text;
    d->attr = attr;
    d->want = want;
    d->ntext = n;
    colour_sync(d, a, b, full ? 300 : 2000, full);
    if (!g_colour_ok) {
        colour_off(d);
        return;
    }
    d->sync = 0;
    d->pass = 0;
    if (d == cur()) SetTimer(g_main, TIMER_SLICE, 10, NULL);
}

/* one slice of the remaining work */
static void colour_slice(void)
{
    Doc *d = cur();
    DWORD t0 = GetTickCount();
    if (!d || !d->text || d->sync < 0 || !g_colour_ok) {
        KillTimer(g_main, TIMER_SLICE);
        if (d && !g_colour_ok) colour_off(d);
        return;
    }
    if (d->pass == 0) {
        /* from what is on the screen, then round to it */
        long line = (long)SendMessage(d->edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        d->sync = (long)SendMessage(d->edit, EM_LINEINDEX, (WPARAM)line, 0);
        if (d->sync < 0 || d->sync > d->ntext) d->sync = 0;
        d->pass = 1;
    }
    do {
        d->sync = colour_sync(d, d->sync, d->ntext, 16, 0);
        if (d->sync >= d->ntext) {
            if (d->pass == 2) {
                d->sync = -1;
                KillTimer(g_main, TIMER_SLICE);
                return;
            }
            d->pass = 2;
            d->sync = 0;
        }
    } while (g_colour_ok && GetTickCount() - t0 < 12);
}

/* ---- documents ---- */

static void tab_text(int i)
{
    Doc *d = g_docs[i];
    char b[100];
    TCITEMA it;
    snprintf(b, sizeof b, "%s%s", d->title, d->shownmod ? " *" : "");
    it.mask = TCIF_TEXT;
    it.pszText = b;
    SendMessage(g_tab, TCM_SETITEMA, (WPARAM)i, (LPARAM)&it);
}

static void update_title(void)
{
    Doc *d = cur();
    char b[MAX_PATH + 40];
    if (!d) snprintf(b, sizeof b, "%s", APPNAME);
    else snprintf(b, sizeof b, "%s - %s", d->path[0] ? d->path : d->title, APPNAME);
    SetWindowTextA(g_main, b);
}

static void update_pos(void)
{
    Doc *d = cur();
    char b[64];
    b[0] = 0;
    if (d) {
        CHARRANGE cr;
        long line, start;
        SendMessage(d->edit, EM_EXGETSEL, 0, (LPARAM)&cr);
        line = (long)SendMessage(d->edit, EM_EXLINEFROMCHAR, 0, cr.cpMin);
        start = (long)SendMessage(d->edit, EM_LINEINDEX, (WPARAM)line, 0);
        snprintf(b, sizeof b, "Ln %ld, Col %ld", line + 1, cr.cpMin - start + 1);
    }
    SendMessage(g_status, SB_SETTEXTA, 1, (LPARAM)b);
    SendMessage(g_status, SB_SETTEXTA, 2,
                (LPARAM)(!d ? "" : d->kind != DOC_SRC ? "Read only" : d->lf ? "LF" : "CRLF"));
}

static void doc_modified(Doc *d)
{
    int i, m = d->kind == DOC_SRC && SendMessage(d->edit, EM_GETMODIFY, 0, 0);
    if (m == d->shownmod) return;
    d->shownmod = m;
    for (i = 0; i < g_ndocs; i++)
        if (g_docs[i] == d) tab_text(i);
}

static void layout(void)
{
    RECT rc, r;
    int top, bottom, w, listy, sp = scale(5);
    int parts[3];
    Doc *d = cur();

    if (!g_main || !g_status) return;
    GetClientRect(g_main, &rc);
    w = rc.right;
    SendMessage(g_tool, TB_AUTOSIZE, 0, 0);
    SendMessage(g_status, WM_SIZE, 0, 0);
    GetWindowRect(g_tool, &r);
    top = r.bottom - r.top;
    GetWindowRect(g_status, &r);
    bottom = rc.bottom - (r.bottom - r.top);
    parts[0] = w - scale(260);
    parts[1] = w - scale(110);
    parts[2] = -1;
    SendMessage(g_status, SB_SETPARTS, 3, (LPARAM)parts);

    if (g_msgh > bottom - top - scale(60)) g_msgh = bottom - top - scale(60);
    if (g_msgh < scale(30)) g_msgh = scale(30);
    listy = bottom - g_msgh;
    g_split = listy - sp;
    MoveWindow(g_tab, 0, top + 2, w, g_split - top - 2, TRUE);
    MoveWindow(g_list, 0, listy, w, g_msgh, TRUE);
    if (d) {
        GetClientRect(g_tab, &r);
        SendMessage(g_tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&r);
        MapWindowPoints(g_tab, g_main, (POINT *)&r, 2);
        SetWindowPos(d->edit, HWND_TOP, r.left, r.top, r.right - r.left,
                     r.bottom - r.top, SWP_SHOWWINDOW);
    }
}

static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC old = (WNDPROC)GetWindowLongPtr(h, GWLP_USERDATA);
    int ctrl = GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0;
    /* PrintScreen is the system's: straight to the default procedure */
    if (w == VK_SNAPSHOT && (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN ||
                             m == WM_SYSKEYUP))
        return DefWindowProcA(h, m, w, l);
    switch (m) {
    case WM_DROPFILES:
        return SendMessage(g_main, m, w, l);
    case WM_KEYDOWN:
        /* the control's own Ctrl keys set paragraph and character
           formats (Ctrl+E, Ctrl+R, Ctrl+5 ...); only the editing ones
           are let through */
        if (ctrl && ((w >= 'A' && w <= 'Z') || (w >= '0' && w <= '9') ||
                     (w >= 0xba && w <= 0xe2)) && !strchr("ACXZY", (int)w))
            return 0;
        break;
    case WM_CHAR:
        /* Ctrl+I and Ctrl+M are not Tab and Enter */
        if (ctrl && w < 0x20) return 0;
        break;
    case WM_CONTEXTMENU: {
        POINT pt;
        pt.x = (short)LOWORD(l);
        pt.y = (short)HIWORD(l);
        if (pt.x == -1 && pt.y == -1) {
            GetCaretPos(&pt);
            ClientToScreen(h, &pt);
        }
        TrackPopupMenu(GetSubMenu(GetMenu(g_main), 1), TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                       g_main, NULL);
        return 0;
    }
    }
    return CallWindowProc(old, h, m, w, l);
}

static Doc *doc_new(int kind)
{
    Doc *d;
    TCITEMA it;
    IUnknown *unk = NULL;
    WNDPROC old;

    if (g_ndocs == MAXDOCS) {
        msgbox(MB_ICONWARNING, "Too many open files.");
        return NULL;
    }
    d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->kind = kind;
    d->sync = -1;
    d->edit = CreateWindowExA(WS_EX_CLIENTEDGE, "RichEdit20A", "",
                              WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                              ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL |
                              (kind == DOC_SRC ? 0 : ES_READONLY),
                              0, 0, 10, 10, g_main, (HMENU)(INT_PTR)IDC_EDIT, g_inst, NULL);
    if (!d->edit) {
        free(d);
        return NULL;
    }
    old = (WNDPROC)SetWindowLongPtr(d->edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    SetWindowLongPtr(d->edit, GWLP_USERDATA, (LONG_PTR)old);
    SendMessage(d->edit, EM_EXLIMITTEXT, 0, 0x7ffffffe);
    SendMessage(d->edit, EM_SETUNDOLIMIT, 1000, 0);
    SendMessage(d->edit, EM_SETLANGOPTIONS, 0, 0);
    SendMessage(d->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                MAKELPARAM(scale(4), scale(4)));
    DragAcceptFiles(d->edit, TRUE);
    drop_allow(d->edit);
    if (kind != DOC_SRC || g_elevated) drop_plain(d->edit);
    if (SendMessage(d->edit, EM_GETOLEINTERFACE, 0, (LPARAM)&unk) && unk) {
        void *td = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(unk, &iid_textdoc, &td))) d->tom = td;
        IUnknown_Release(unk);
    }
    doc_format(d);
    SendMessage(d->edit, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE | ENM_DROPFILES);

    g_docs[g_ndocs] = d;
    memset(&it, 0, sizeof it);
    it.mask = TCIF_TEXT;
    it.pszText = (char *)"";
    SendMessage(g_tab, TCM_INSERTITEMA, (WPARAM)g_ndocs, (LPARAM)&it);
    g_ndocs++;
    return d;
}

static int doc_index(const Doc *d)
{
    int i;
    for (i = 0; i < g_ndocs; i++)
        if (g_docs[i] == d) return i;
    return -1;
}

static int doc_find(const char *path)
{
    int i;
    for (i = 0; i < g_ndocs; i++)
        if (g_docs[i]->path[0] && !lstrcmpiA(g_docs[i]->path, path)) return i;
    return -1;
}

static void doc_set_path(Doc *d, const char *path)
{
    int i = doc_index(d);
    lstrcpynA(d->path, path, MAX_PATH);
    lstrcpynA(d->title, gui_basename(path), sizeof d->title);
    if (i >= 0) tab_text(i);
}

typedef struct {
    HANDLE h;
    const char *mem;
    size_t n;
    int lf, cr, err;
} Stream;

static DWORD CALLBACK stream_in(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG *done)
{
    Stream *s = (Stream *)cookie;
    DWORD got = 0;
    if (s->mem) {
        got = (DWORD)(s->n < (size_t)cb ? s->n : (size_t)cb);
        memcpy(buf, s->mem, got);
        s->mem += got;
        s->n -= got;
    } else if (!ReadFile(s->h, buf, (DWORD)cb, &got, NULL)) {
        s->err = 1;
        *done = 0;
        return 1;
    }
    *done = (LONG)got;
    return 0;
}

static DWORD CALLBACK stream_out(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG *done)
{
    Stream *s = (Stream *)cookie;
    char tmp[4096];
    LONG i = 0;
    DWORD put;
    *done = cb;
    if (!s->lf) {
        if (!WriteFile(s->h, buf, (DWORD)cb, &put, NULL) || put != (DWORD)cb) s->err = 1;
        return (DWORD)s->err;
    }
    /* CRLF from the control, LF in the file */
    while (i < cb && !s->err) {
        DWORD k = 0;
        while (i < cb && k + 2 < sizeof tmp) {
            char c = (char)buf[i++];
            if (s->cr && c != '\n') tmp[k++] = '\r';
            s->cr = c == '\r';
            if (!s->cr) tmp[k++] = c;
        }
        if (!WriteFile(s->h, tmp, k, &put, NULL) || put != k) s->err = 1;
    }
    return (DWORD)s->err;
}

static void doc_fill(Doc *d, Stream *s)
{
    EDITSTREAM es;
    CHARRANGE top = { 0, 0 };
    int off = redraw_off(d);
    d->loading = 1;
    es.dwCookie = (DWORD_PTR)s;
    es.dwError = 0;
    es.pfnCallback = stream_in;
    SendMessage(d->edit, EM_STREAMIN, SF_TEXT, (LPARAM)&es);
    doc_format(d);
    SendMessage(d->edit, EM_EXSETSEL, 0, (LPARAM)&top);
    SendMessage(d->edit, EM_SETMODIFY, FALSE, 0);
    SendMessage(d->edit, EM_EMPTYUNDOBUFFER, 0, 0);
    colour_drop(d);
    if (colour_wanted(d)) {
        colour_scan(d, 1);
    }
    SendMessage(d->edit, WM_VSCROLL, SB_TOP, 0);
    redraw_on(d, off);
    d->loading = 0;
    d->shownmod = 0;
}

static int doc_load(Doc *d, const char *path)
{
    Stream s;
    char head[65536];
    DWORD got = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, head, sizeof head, &got, NULL);
    d->lf = gui_detect_lf(head, got);
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    memset(&s, 0, sizeof s);
    s.h = h;
    doc_fill(d, &s);
    CloseHandle(h);
    file_time(path, &d->ft);
    return !s.err;
}

static int doc_write(Doc *d, const char *path)
{
    Stream s;
    EDITSTREAM es;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put;
    if (h == INVALID_HANDLE_VALUE) return 0;
    memset(&s, 0, sizeof s);
    s.h = h;
    s.lf = d->lf;
    es.dwCookie = (DWORD_PTR)&s;
    es.dwError = 0;
    es.pfnCallback = stream_out;
    SendMessage(d->edit, EM_STREAMOUT, SF_TEXT, (LPARAM)&es);
    if (s.cr && !s.err && !WriteFile(h, "\r", 1, &put, NULL)) s.err = 1;
    if (!CloseHandle(h)) s.err = 1;
    return !s.err;
}

static void recent_add(const char *path)
{
    gui_recent_add(g_recent, path);
}

static int doc_save(Doc *d, int saveas)
{
    char path[MAX_PATH];
    if (d->kind != DOC_SRC) return 1;
    lstrcpynA(path, d->path[0] ? d->path : d->title, MAX_PATH);
    if (saveas || !d->path[0]) {
        OPENFILENAMEA of;
        memset(&of, 0, sizeof of);
        of.lStructSize = sizeof of;
        of.hwndOwner = g_main;
        of.lpstrFilter = "CUPL source (*.pld)\0*.pld\0Simulation input (*.si)\0*.si\0"
                         "All files (*.*)\0*.*\0";
        of.nFilterIndex = gui_has_ext(path, ".si") ? 2 : 1;
        of.lpstrFile = path;
        of.nMaxFile = MAX_PATH;
        of.lpstrDefExt = gui_has_ext(path, ".si") ? "si" : "pld";
        of.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameA(&of)) return 0;
    }
    if (!doc_write(d, path)) {
        char e[256];
        errtext(GetLastError(), e, sizeof e);
        msgbox(MB_ICONERROR, "Cannot write %s\n%s.", path, e);
        return 0;
    }
    SendMessage(d->edit, EM_SETMODIFY, FALSE, 0);
    doc_set_path(d, path);
    file_time(path, &d->ft);
    doc_modified(d);
    recent_add(path);
    if (d == cur()) update_title();
    return 1;
}

static void doc_free(Doc *d)
{
    colour_drop(d);
    if (d->tom) ITextDocument_Release(d->tom);
    DestroyWindow(d->edit);
    free(d);
}

/* 0 when the user cancels */
static int doc_close(int i)
{
    Doc *d = g_docs[i];
    int j;
    if (d->kind == DOC_SRC && SendMessage(d->edit, EM_GETMODIFY, 0, 0)) {
        int r;
        doc_activate(i);
        r = msgbox(MB_YESNOCANCEL | MB_ICONQUESTION, "Save changes to %s?", d->title);
        if (r == IDCANCEL) return 0;
        if (r == IDYES && !doc_save(d, 0)) return 0;
    }
    KillTimer(g_main, TIMER_COLOUR);
    KillTimer(g_main, TIMER_SLICE);
    doc_free(d);
    for (j = i; j + 1 < g_ndocs; j++) g_docs[j] = g_docs[j + 1];
    g_ndocs--;
    SendMessage(g_tab, TCM_DELETEITEM, (WPARAM)i, 0);
    g_cur = -1;
    if (g_ndocs) doc_activate(i < g_ndocs ? i : g_ndocs - 1);
    else {
        update_title();
        update_pos();
        update_ui();
        InvalidateRect(g_main, NULL, TRUE);
    }
    return 1;
}

static void doc_activate(int i)
{
    Doc *old = cur();
    if (i < 0 || i >= g_ndocs) return;
    if (old && old != g_docs[i]) ShowWindow(old->edit, SW_HIDE);
    g_cur = i;
    SendMessage(g_tab, TCM_SETCURSEL, (WPARAM)i, 0);
    layout();
    SetFocus(g_docs[i]->edit);
    if (g_docs[i]->sync >= 0) SetTimer(g_main, TIMER_SLICE, 10, NULL);
    update_title();
    update_pos();
    update_ui();
}

static void goto_line(Doc *d, int line)
{
    CHARRANGE cr;
    long n = (long)SendMessage(d->edit, EM_GETLINECOUNT, 0, 0);
    if (line > n) line = (int)n;
    if (line < 1) line = 1;
    cr.cpMin = (long)SendMessage(d->edit, EM_LINEINDEX, (WPARAM)(line - 1), 0);
    cr.cpMax = cr.cpMin + (long)SendMessage(d->edit, EM_LINELENGTH, (WPARAM)cr.cpMin, 0);
    /* the line selected, so that it is seen */
    SendMessage(d->edit, EM_EXSETSEL, 0, (LPARAM)&cr);
    SendMessage(d->edit, EM_SCROLLCARET, 0, 0);
    SetFocus(d->edit);
    update_pos();
}

static void reload_views(void)
{
    int i;
    for (i = 0; i < g_ndocs; i++) {
        Doc *d = g_docs[i];
        FILETIME ft;
        long first;
        if (d->kind != DOC_VIEW || !d->path[0] || !file_time(d->path, &ft)) continue;
        if (!CompareFileTime(&ft, &d->ft)) continue;
        first = (long)SendMessage(d->edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        doc_load(d, d->path);
        SendMessage(d->edit, EM_LINESCROLL, 0, first);
    }
}

static int open_file(const char *name)
{
    char path[MAX_PATH], *part;
    Doc *d;
    int i;
    if (!GetFullPathNameA(name, MAX_PATH, path, &part)) lstrcpynA(path, name, MAX_PATH);
    i = doc_find(path);
    if (i >= 0) {
        reload_views();
        doc_activate(i);
        return 1;
    }
    if (!file_time(path, NULL)) {
        msgbox(MB_ICONWARNING, "Cannot open %s", path);
        return 0;
    }
    d = doc_new(gui_is_output(path) ? DOC_VIEW : DOC_SRC);
    if (!d) return 0;
    doc_set_path(d, path);
    if (!doc_load(d, path)) {
        /* an empty tab under that name could be saved over the file */
        g_ndocs--;
        SendMessage(g_tab, TCM_DELETEITEM, (WPARAM)g_ndocs, 0);
        doc_free(d);
        msgbox(MB_ICONWARNING, "Cannot read %s", path);
        return 0;
    }
    if (d->kind == DOC_SRC) recent_add(path);
    doc_activate(g_ndocs - 1);
    return 1;
}

static void new_file(void)
{
    static const char tmpl[] =
        "Name     XXXXX ;\r\nPartNo   XXXXX ;\r\nDate     XX/XX/XX ;\r\n"
        "Revision XX ;\r\nDesigner XXXXX ;\r\nCompany  XXXXX ;\r\n"
        "Assembly XXXXX ;\r\nLocation XXXXX ;\r\nDevice   XXXXX ;\r\n\r\n";
    Stream s;
    Doc *d = doc_new(DOC_SRC);
    if (!d) return;
    snprintf(d->title, sizeof d->title, "new%d.pld", ++g_newcount);
    tab_text(g_ndocs - 1);
    memset(&s, 0, sizeof s);
    s.mem = tmpl;
    s.n = sizeof tmpl - 1;
    doc_fill(d, &s);
    doc_activate(g_ndocs - 1);
}

static void open_dialog(void)
{
    static char buf[8192];
    OPENFILENAMEA of;
    Doc *d = cur();
    char dir[MAX_PATH];
    memset(buf, 0, sizeof buf);
    memset(&of, 0, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = g_main;
    of.lpstrFilter = "CUPL files (*.pld;*.si)\0*.pld;*.si\0"
                     "Output files (*.jed;*.doc;*.lst;*.so)\0*.jed;*.doc;*.lst;*.so\0"
                     "All files (*.*)\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = sizeof buf;
    if (d && d->path[0]) {
        gui_dirname(d->path, dir, sizeof dir);
        of.lpstrInitialDir = dir;
    }
    of.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameA(&of)) return;
    if (buf[strlen(buf) + 1]) {
        /* directory, then the names */
        const char *p = buf + strlen(buf) + 1;
        for (; *p; p += strlen(p) + 1) {
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s\\%s", buf, p);
            open_file(path);
        }
    } else {
        open_file(buf);
    }
}

static void show_guide(void)
{
    HRSRC r;
    HGLOBAL g;
    Stream s;
    Doc *d;
    int i;
    for (i = 0; i < g_ndocs; i++)
        if (g_docs[i]->kind == DOC_HELP) {
            doc_activate(i);
            return;
        }
    r = FindResourceA(g_inst, MAKEINTRESOURCEA(IDR_GUIDE), (LPCSTR)RT_RCDATA);
    g = r ? LoadResource(g_inst, r) : NULL;
    if (!g) return;
    d = doc_new(DOC_HELP);
    if (!d) return;
    lstrcpyA(d->title, "User Guide");
    tab_text(g_ndocs - 1);
    memset(&s, 0, sizeof s);
    s.mem = LockResource(g);
    s.n = SizeofResource(g_inst, r);
    doc_fill(d, &s);
    doc_activate(g_ndocs - 1);
}

/* ---- the design of the active file ---- */

static int target(char *pld, char *si)
{
    Doc *d = cur();
    if (d && d->kind == DOC_SRC && d->path[0]) {
        if (gui_has_ext(d->path, ".si")) {
            lstrcpyA(si, d->path);
            gui_set_ext(d->path, ".pld", pld, MAX_PATH);
        } else {
            lstrcpyA(pld, d->path);
            gui_set_ext(d->path, ".si", si, MAX_PATH);
        }
        return pld[0] && si[0];
    }
    if (g_lastpld[0]) {
        lstrcpyA(pld, g_lastpld);
        gui_set_ext(g_lastpld, ".si", si, MAX_PATH);
        return 1;
    }
    return 0;
}

static void join(char *out, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    snprintf(out, MAX_PATH, "%s%s%s", dir,
             l && (dir[l - 1] == '\\' || dir[l - 1] == '/') ? "" : "\\", name);
}

/* names the outputs of pld can have: the Name item, the source name,
   no_name */
static int out_names(const char *pld, char names[3][MAX_PATH])
{
    char head[65536], nm[80], dir[MAX_PATH], base[MAX_PATH];
    DWORD got = 0;
    int n = 0;
    HANDLE h = CreateFileA(pld, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    gui_dirname(pld, dir, sizeof dir);
    gui_set_ext(pld, "", base, sizeof base);
    if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, head, sizeof head, &got, NULL);
        CloseHandle(h);
        if (gui_design_name(head, got, nm, sizeof nm) && !strpbrk(nm, "\\/:*?\"<>| \t"))
            join(names[n++], dir, nm);
    }
    if (!n || lstrcmpiA(names[0], base)) lstrcpynA(names[n++], base, MAX_PATH);
    join(names[n++], dir, "no_name");
    return n;
}

/* the JEDEC file a run of pld writes, without extension: named after the
   source with -n, else after the Name item, else no_name */
static void jed_name(const char *pld, int namesrc, char out[MAX_PATH])
{
    char head[65536], nm[80], dir[MAX_PATH];
    DWORD got = 0;
    HANDLE h;
    if (namesrc) { gui_set_ext(pld, "", out, MAX_PATH); return; }
    h = CreateFileA(pld, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    gui_dirname(pld, dir, sizeof dir);
    if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, head, sizeof head, &got, NULL);
        CloseHandle(h);
    }
    join(out, dir, gui_design_name(head, got, nm, sizeof nm) ? nm : "no_name");
}

static void view_output(const char *ext)
{
    char pld[MAX_PATH], si[MAX_PATH], names[3][MAX_PATH], path[MAX_PATH];
    int n, i;
    if (!target(pld, si)) {
        MessageBeep(MB_ICONWARNING);
        status("Open a .pld file first.");
        return;
    }
    if (!strcmp(ext, ".pld") || !strcmp(ext, ".si")) {
        const char *p = strcmp(ext, ".si") ? pld : si;
        if (file_time(p, NULL)) open_file(p);
        else status("No file %s", gui_basename(p));
        return;
    }
    n = out_names(pld, names);
    /* only the JEDEC file is named after the Name item */
    if (strcmp(ext, ".jed")) {
        gui_set_ext(pld, "", names[0], MAX_PATH);
        n = 1;
    }
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s%s", names[i], ext);
        if (file_time(path, NULL)) {
            open_file(path);
            return;
        }
    }
    MessageBeep(MB_ICONWARNING);
    snprintf(path, sizeof path, "%s%s", names[0], ext);
    status("No file %s", gui_basename(path));
}

/* ---- message pane ---- */

static void msg_clear(void)
{
    int i;
    SendMessage(g_list, LB_RESETCONTENT, 0, 0);
    SendMessage(g_list, LB_SETHORIZONTALEXTENT, 0, 0);
    for (i = 0; i < g_nmsgfiles; i++) free(g_msgfiles[i]);
    free(g_msgfiles);
    g_msgfiles = NULL;
    g_nmsgfiles = 0;
    g_msgextent = 0;
}

/* item data: kind, file (index into g_msgfiles + 1, 0 for none), line */
#define ITEM(kind, file, line) ((kind) | ((file) << 4) | ((line) << 12))
#define ITEM_KIND(d) ((int)((d) & 15))
#define ITEM_FILE(d) ((int)(((d) >> 4) & 255))
#define ITEM_LINE(d) ((int)((d) >> 12))

static void msg_add(const char *text, int kind, int file, int line)
{
    LRESULT i = SendMessage(g_list, LB_ADDSTRING, 0, (LPARAM)text);
    HDC dc;
    HFONT old;
    SIZE sz;
    if (i < 0) return;
    if (file > 255) file = 0;
    if (line < 0 || line > 0x7ffff) line = 0;
    SendMessage(g_list, LB_SETITEMDATA, (WPARAM)i, (LPARAM)ITEM(kind, file, line));
    dc = GetDC(g_list);
    old = SelectObject(dc, g_listfont);
    if (GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz) &&
        sz.cx + scale(12) > g_msgextent) {
        g_msgextent = sz.cx + scale(12);
        SendMessage(g_list, LB_SETHORIZONTALEXTENT, (WPARAM)g_msgextent, 0);
    }
    SelectObject(dc, old);
    ReleaseDC(g_list, dc);
    SendMessage(g_list, LB_SETTOPINDEX, (WPARAM)i, 0);
}

/* a file the messages can refer to; returns its number, 0 on failure */
static int msg_file(const char *path)
{
    char **nf = realloc(g_msgfiles, (size_t)(g_nmsgfiles + 1) * sizeof *nf);
    size_t l = strlen(path) + 1;
    if (!nf) return 0;
    g_msgfiles = nf;
    nf[g_nmsgfiles] = malloc(l);
    if (!nf[g_nmsgfiles]) return 0;
    memcpy(nf[g_nmsgfiles], path, l);
    return ++g_nmsgfiles;
}

static void msg_add_file(const char *path)
{
    char b[MAX_PATH + 16];
    snprintf(b, sizeof b, "    %s", gui_basename(path));
    msg_add(b, GM_FILE, msg_file(path), 0);
}

static void msg_tool_line(const char *line)
{
    GuiMsg m;
    gui_parse_msg(line, &m);
    if (m.kind == GM_ERROR) g_run.nerr++;
    else if (m.kind == GM_WARN || m.kind == GM_NOTE) g_run.nwarn++;
    /* simulator messages count lines of the .si file */
    msg_add(line, m.kind, m.code[4] == 's' ? g_run.fsi : g_run.fpld, m.line);
}

/* The simulator writes its messages into the listing only. */
static void msg_scan_listing(const char *so)
{
    FILE *f = fopen(so, "r");
    char line[1024], b[1200];
    int n = 0, file = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        GuiMsg m;
        n++;
        if (line[0] != '[') continue;
        line[strcspn(line, "\r\n")] = 0;
        gui_parse_msg(line, &m);
        if (m.kind != GM_ERROR && m.kind != GM_WARN && m.kind != GM_NOTE) continue;
        if (m.kind == GM_ERROR) g_run.nerr++;
        else g_run.nwarn++;
        if (!file) file = msg_file(so);
        snprintf(b, sizeof b, "%s(%d): %s", gui_basename(so), n, line);
        msg_add(b, m.kind, file, n);
    }
    fclose(f);
}

static void msg_open(int i)
{
    LRESULT data = SendMessage(g_list, LB_GETITEMDATA, (WPARAM)i, 0);
    int file, line;
    if (data == LB_ERR) return;
    file = ITEM_FILE(data);
    line = ITEM_LINE(data);
    if (file < 1 || file > g_nmsgfiles) return;
    if (ITEM_KIND(data) != GM_FILE && line < 1) return;
    if (open_file(g_msgfiles[file - 1]) && line > 0) goto_line(cur(), line);
}

static void msg_draw(const DRAWITEMSTRUCT *di)
{
    char b[2048];
    int kind = ITEM_KIND(di->itemData);
    int sel = (di->itemState & ODS_SELECTED) != 0;
    COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
    RECT r = di->rcItem;
    if (di->itemID == (UINT)-1) return;
    if (kind == GM_ERROR || kind == GM_ABORT) fg = RGB(192, 0, 0);
    else if (kind == GM_WARN || kind == GM_NOTE) fg = RGB(128, 96, 0);
    else if (kind == GM_FILE) fg = RGB(0, 0, 192);
    else if (kind == GM_CMD) fg = GetSysColor(COLOR_GRAYTEXT);
    if (sel) fg = GetSysColor(COLOR_HIGHLIGHTTEXT);
    FillRect(di->hDC, &r, GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
    b[0] = 0;
    if (SendMessage(g_list, LB_GETTEXTLEN, di->itemID, 0) < (LRESULT)sizeof b)
        SendMessage(g_list, LB_GETTEXT, di->itemID, (LPARAM)b);
    SetTextColor(di->hDC, fg);
    SetBkMode(di->hDC, TRANSPARENT);
    r.left += scale(4);
    {
        HFONT old = SelectObject(di->hDC, g_listfont);
        DrawTextA(di->hDC, b, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_EXPANDTABS);
        SelectObject(di->hDC, old);
    }
    if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &di->rcItem);
}

static void msg_copy(int all)
{
    int n = (int)SendMessage(g_list, LB_GETCOUNT, 0, 0), i, from = 0, to = n;
    size_t len = 1, k = 0;
    HGLOBAL g;
    char *p;
    if (!all) {
        from = (int)SendMessage(g_list, LB_GETCURSEL, 0, 0);
        if (from < 0) return;
        to = from + 1;
    }
    for (i = from; i < to; i++)
        len += (size_t)SendMessage(g_list, LB_GETTEXTLEN, (WPARAM)i, 0) + 2;
    g = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!g) return;
    p = GlobalLock(g);
    for (i = from; i < to; i++) {
        k += (size_t)SendMessage(g_list, LB_GETTEXT, (WPARAM)i, (LPARAM)(p + k));
        if (all) {
            p[k++] = '\r';
            p[k++] = '\n';
        }
    }
    p[k] = 0;
    GlobalUnlock(g);
    if (OpenClipboard(g_main)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_TEXT, g)) GlobalFree(g);
        CloseClipboard();
    } else {
        GlobalFree(g);
    }
}

/* ---- compile and simulate ---- */

static void snap_add(const char *base, const char *ext)
{
    Snap *s;
    if (g_run.nsnap == (int)(sizeof g_run.snap / sizeof g_run.snap[0])) return;
    s = &g_run.snap[g_run.nsnap++];
    snprintf(s->path, sizeof s->path, "%s%s", base, ext);
    s->existed = file_time(s->path, &s->ft);
}

static void post_line(void *ctx, const char *line)
{
    size_t l = strlen(line) + 1;
    char *p = malloc(l);
    int tries = 0;
    (void)ctx;
    if (!p) return;
    memcpy(p, line, l);
    /* the queue is full when the tool prints faster than the list fills */
    while (!PostMessage(g_main, WM_APP_LINE, 0, (LPARAM)p)) {
        if (++tries > 2000) {
            free(p);
            return;
        }
        Sleep(5);
    }
}

static DWORD WINAPI read_thread(LPVOID arg)
{
    (void)arg;
    proc_read(&g_run.proc, post_line, NULL);
    while (!PostMessage(g_main, WM_APP_DONE, 0, 0) && IsWindow(g_main)) Sleep(20);
    return 0;
}

static void run_end(void)
{
    g_run.active = 0;
    update_ui();
}

static void step_done(DWORD rc);

static void step_start(void)
{
    int step = g_run.queue[g_run.qi];
    char cmd[2 * MAX_PATH + 128], shown[MAX_PATH + 128], exe[MAX_PATH];
    char jn[MAX_PATH], base[MAX_PATH];
    const char *tool = step == STEP_COMPILE ? "freecupl" : "freecsim";
    const char *file = gui_basename(step == STEP_COMPILE ? g_run.pld : g_run.si);
    int flags = step == STEP_COMPILE ? g_run.flags : 0;
    int r;
    DWORD err, tid;

    g_run.nerr = g_run.nwarn = 0;
    g_run.nsnap = 0;
    snprintf(exe, sizeof exe, "%s\\%s.exe", g_exedir, tool);
    if (step == STEP_COMPILE) {
        r = gui_compile_cmd(&g_run.o, exe, file, flags, cmd, sizeof cmd);
        gui_compile_cmd(&g_run.o, tool, file, flags, shown, sizeof shown);
    } else {
        r = gui_sim_cmd(&g_run.o, exe, file, cmd, sizeof cmd);
        gui_sim_cmd(&g_run.o, tool, file, shown, sizeof shown);
    }
    msg_add(shown, GM_CMD, 0, 0);
    status("%s %s...", step == STEP_COMPILE ? "Compiling" : "Simulating", file);

    gui_set_ext(g_run.pld, "", base, sizeof base);
    jn[0] = 0;
    if (file_time(g_run.pld, NULL)) jed_name(g_run.pld, g_run.o.namesrc, jn);
    if (step == STEP_COMPILE) {
        if (jn[0]) snap_add(jn, ".jed");
        snap_add(base, ".doc");
        snap_add(base, ".abs");
        snap_add(base, ".lst");
        snap_add(base, ".mx");
    } else if (g_run.o.simjed) {
        if (jn[0]) snap_add(jn, ".jed");
    }
    if (step == STEP_SIM || (flags & GC_SIM)) {
        gui_set_ext(g_run.si, "", base, sizeof base);
        snap_add(base, ".so");
        snap_add(base, ".wo");
    }
    GetSystemTimeAsFileTime(&g_run.start);

    err = r ? ERROR_FILENAME_EXCED_RANGE : proc_start(&g_run.proc, cmd, g_run.dir);
    if (!err) {
        g_run.thread = CreateThread(NULL, 0, read_thread, NULL, 0, &tid);
        if (!g_run.thread) {
            err = GetLastError();
            TerminateProcess(g_run.proc.process, 1);
            proc_finish(&g_run.proc);
        }
    }
    if (err) {
        char e[256], b[MAX_PATH + 300];
        errtext(err, e, sizeof e);
        snprintf(b, sizeof b, "Cannot run %s.exe: %s", tool, e);
        msg_add(b, GM_ABORT, 0, 0);
        step_done(1);
    }
}

static void step_done(DWORD rc)
{
    int step = g_run.queue[g_run.qi];
    char sum[128];
    ULARGE_INTEGER t0, t1;
    int i;

    t0.LowPart = g_run.start.dwLowDateTime;
    t0.HighPart = g_run.start.dwHighDateTime;
    for (i = 0; i < g_run.nsnap; i++) {
        Snap *s = &g_run.snap[i];
        FILETIME ft;
        if (!file_time(s->path, &ft)) continue;
        t1.LowPart = ft.dwLowDateTime;
        t1.HighPart = ft.dwHighDateTime;
        /* FAT keeps two-second times */
        if (!s->existed || CompareFileTime(&ft, &s->ft) ||
            t1.QuadPart + 20000000u >= t0.QuadPart) {
            if (gui_has_ext(s->path, ".so")) msg_scan_listing(s->path);
            msg_add_file(s->path);
        }
    }
    if (g_run.stopped) msg_add("Stopped.", GM_ABORT, 0, 0);
    gui_summary(sum, sizeof sum, step == STEP_COMPILE ? "Compile" : "Simulate",
                g_run.nerr, g_run.nwarn, (int)rc);
    msg_add(sum, rc || g_run.nerr ? GM_ABORT : GM_SUMMARY, 0, 0);
    status("%s", sum);
    reload_views();
    g_run.qi++;
    if (rc == 0 && !g_run.stopped && g_run.qi < g_run.nq) step_start();
    else run_end();
}

static void run(int what)
{
    Doc *d = cur();
    char pld[MAX_PATH], si[MAX_PATH];
    int i, havepld, havesi;

    if (g_run.active) return;
    if (d && d->kind == DOC_SRC && !d->path[0] && !doc_save(d, 1)) return;
    if (!target(pld, si)) {
        msgbox(MB_ICONINFORMATION, "Open a .pld file first.");
        return;
    }
    for (i = 0; i < g_ndocs; i++) {
        d = g_docs[i];
        if (d->kind == DOC_SRC && d->path[0] &&
            SendMessage(d->edit, EM_GETMODIFY, 0, 0) && !doc_save(d, 0))
            return;
    }
    havepld = file_time(pld, NULL);
    havesi = file_time(si, NULL);
    if (what == STEP_COMPILE && !havepld) {
        msgbox(MB_ICONINFORMATION, "No source file %s", gui_basename(pld));
        return;
    }
    if (what == STEP_SIM && !havesi) {
        msgbox(MB_ICONINFORMATION, "No simulation input %s", gui_basename(si));
        return;
    }
    lstrcpyA(g_run.pld, pld);
    lstrcpyA(g_run.si, si);
    lstrcpyA(g_lastpld, pld);
    gui_dirname(pld, g_run.dir, sizeof g_run.dir);
    opts_for(pld, &g_run.o);
    g_run.nq = g_run.qi = 0;
    g_run.flags = 0;
    g_run.stopped = 0;
    msg_clear();
    g_run.fpld = msg_file(pld);
    g_run.fsi = msg_file(si);
    if (what == STEP_COMPILE) {
        g_run.queue[g_run.nq++] = STEP_COMPILE;
        if (g_run.o.simafter) {
            if (havesi) {
                g_run.flags = GC_SIM;
            } else {
                char b[MAX_PATH + 40];
                snprintf(b, sizeof b, "No %s: not simulated.", gui_basename(si));
                msg_add(b, GM_TEXT, 0, 0);
            }
        }
    } else {
        /* the simulator reads the absolute file of the compiler */
        if (havepld) {
            g_run.queue[g_run.nq++] = STEP_COMPILE;
            g_run.flags = GC_ABS;
        }
        g_run.queue[g_run.nq++] = STEP_SIM;
    }
    g_run.active = 1;
    update_ui();
    step_start();
}

static void run_stop(void)
{
    if (!g_run.active || !g_run.proc.process) return;
    g_run.stopped = 1;
    TerminateProcess(g_run.proc.process, 1);
}

/* ---- find and replace ---- */

static int find_next(DWORD flags, int quiet)
{
    Doc *d = cur();
    FINDTEXTEXA ft;
    CHARRANGE sel;
    DWORD f = flags & (FR_DOWN | FR_MATCHCASE | FR_WHOLEWORD);
    if (!d || !g_findwhat[0]) return 0;
    SendMessage(d->edit, EM_EXGETSEL, 0, (LPARAM)&sel);
    ft.lpstrText = g_findwhat;
    if (f & FR_DOWN) {
        ft.chrg.cpMin = sel.cpMax;
        ft.chrg.cpMax = -1;
    } else {
        ft.chrg.cpMin = sel.cpMin;
        ft.chrg.cpMax = 0;
    }
    if (SendMessage(d->edit, EM_FINDTEXTEX, f, (LPARAM)&ft) < 0) {
        /* once more from the other end */
        if (f & FR_DOWN) {
            ft.chrg.cpMin = 0;
            ft.chrg.cpMax = -1;
        } else {
            ft.chrg.cpMin = 0x7ffffffe;
            ft.chrg.cpMax = 0;
        }
        if (SendMessage(d->edit, EM_FINDTEXTEX, f, (LPARAM)&ft) < 0) {
            if (!quiet)
                MessageBoxA(g_find ? g_find : g_main, "Text not found.", APPNAME,
                            MB_ICONINFORMATION);
            return 0;
        }
    }
    SendMessage(d->edit, EM_EXSETSEL, 0, (LPARAM)&ft.chrgText);
    SendMessage(d->edit, EM_SCROLLCARET, 0, 0);
    return 1;
}

static int sel_matches(Doc *d, DWORD flags)
{
    char b[4 * sizeof g_findwhat + 4];      /* bytes of the code page */
    CHARRANGE sel;
    SendMessage(d->edit, EM_EXGETSEL, 0, (LPARAM)&sel);
    if (sel.cpMax - sel.cpMin <= 0 || sel.cpMax - sel.cpMin >= (long)sizeof g_findwhat)
        return 0;
    SendMessage(d->edit, EM_GETSELTEXT, 0, (LPARAM)b);
    return (flags & FR_MATCHCASE) ? !lstrcmpA(b, g_findwhat) : !lstrcmpiA(b, g_findwhat);
}

static void replace_all(DWORD flags)
{
    Doc *d = cur();
    FINDTEXTEXA ft;
    CHARRANGE sel;
    DWORD f = (flags & (FR_MATCHCASE | FR_WHOLEWORD)) | FR_DOWN;
    long pos = 0;
    int n = 0, off;
    if (!d || d->kind != DOC_SRC || !g_findwhat[0]) return;
    off = redraw_off(d);
    for (;;) {
        ft.lpstrText = g_findwhat;
        ft.chrg.cpMin = pos;
        ft.chrg.cpMax = -1;
        if (SendMessage(d->edit, EM_FINDTEXTEX, f, (LPARAM)&ft) < 0) break;
        SendMessage(d->edit, EM_EXSETSEL, 0, (LPARAM)&ft.chrgText);
        SendMessage(d->edit, EM_REPLACESEL, TRUE, (LPARAM)g_replwith);
        SendMessage(d->edit, EM_EXGETSEL, 0, (LPARAM)&sel);
        pos = sel.cpMax;
        n++;
    }
    redraw_on(d, off);
    status("%d replaced", n);
}

static void find_dialog(int replace)
{
    Doc *d = cur();
    CHARRANGE sel;
    if (!d) return;
    if (g_find) DestroyWindow(g_find);
    SendMessage(d->edit, EM_EXGETSEL, 0, (LPARAM)&sel);
    if (sel.cpMax > sel.cpMin && sel.cpMax - sel.cpMin < 100) {
        char b[512];                        /* bytes of the code page */
        SendMessage(d->edit, EM_GETSELTEXT, 0, (LPARAM)b);
        if (!strpbrk(b, "\r\n")) lstrcpynA(g_findwhat, b, sizeof g_findwhat);
    }
    memset(&g_fr, 0, sizeof g_fr);
    g_fr.lStructSize = sizeof g_fr;
    g_fr.hwndOwner = g_main;
    g_fr.lpstrFindWhat = g_findwhat;
    g_fr.wFindWhatLen = sizeof g_findwhat;
    g_fr.lpstrReplaceWith = g_replwith;
    g_fr.wReplaceWithLen = sizeof g_replwith;
    g_fr.Flags = g_findflags & (FR_DOWN | FR_MATCHCASE | FR_WHOLEWORD);
    if (replace && d->kind == DOC_SRC) g_find = ReplaceTextA(&g_fr);
    else g_find = FindTextA(&g_fr);
}

static void find_message(const FINDREPLACEA *fr)
{
    Doc *d = cur();
    if (fr->Flags & FR_DIALOGTERM) {
        g_find = NULL;
        return;
    }
    g_findflags = fr->Flags;
    if (!d) return;
    if (fr->Flags & FR_FINDNEXT) {
        find_next(fr->Flags, 0);
    } else if (fr->Flags & FR_REPLACE) {
        if (d->kind == DOC_SRC && sel_matches(d, fr->Flags))
            SendMessage(d->edit, EM_REPLACESEL, TRUE, (LPARAM)g_replwith);
        find_next(fr->Flags | FR_DOWN, 0);
    } else if (fr->Flags & FR_REPLACEALL) {
        replace_all(fr->Flags);
    }
}

/* ---- dialogs ---- */

static void center(HWND h)
{
    RECT o, r, wa;
    int x, y;
    GetWindowRect(g_main, &o);
    GetWindowRect(h, &r);
    x = o.left + ((o.right - o.left) - (r.right - r.left)) / 2;
    y = o.top + ((o.bottom - o.top) - (r.bottom - r.top)) / 2;
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
        if (x + (r.right - r.left) > wa.right) x = wa.right - (r.right - r.left);
        if (y + (r.bottom - r.top) > wa.bottom) y = wa.bottom - (r.bottom - r.top);
        if (x < wa.left) x = wa.left;
        if (y < wa.top) y = wa.top;
    }
    SetWindowPos(h, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static INT_PTR CALLBACK goto_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_INITDIALOG:
        center(h);
        SetDlgItemInt(h, IDC_G_LINE, (UINT)l, FALSE);
        SendDlgItemMessage(h, IDC_G_LINE, EM_SETSEL, 0, -1);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            EndDialog(h, (INT_PTR)GetDlgItemInt(h, IDC_G_LINE, NULL, FALSE));
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void about_line(void *ctx, const char *line)
{
    char *b = ctx;
    if (!b[0]) lstrcpynA(b, line, 120);
}

static INT_PTR CALLBACK about_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_INITDIALOG: {
        char cmd[MAX_PATH + 16], exe[MAX_PATH], ver[128], b[200];
        Proc p;
        center(h);
        SetDlgItemTextA(h, IDC_A_VERSION, FREECUPL_VERSION_STRING);
        snprintf(exe, sizeof exe, "%s\\freecupl.exe", g_exedir);
        ver[0] = 0;
        cmd[0] = 0;
        gui_quote_arg(exe, cmd, sizeof cmd - 4);
        lstrcatA(cmd, " -V");
        if (!proc_start(&p, cmd, NULL)) {
            proc_read(&p, about_line, ver);
            proc_finish(&p);
            snprintf(b, sizeof b, "Compiler: %s", ver);
        } else {
            snprintf(b, sizeof b, "freecupl.exe was not found in the directory of this program.");
        }
        if (g_elevated) lstrcatA(b, "\nRunning as administrator.");
        SetDlgItemTextA(h, IDC_A_TOOLS, b);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static const struct {
    int id;
    size_t off;
} g_optctl[] = {
    { IDC_O_JED, offsetof(GuiOpts, jed) },
    { IDC_O_DOC, offsetof(GuiOpts, doc) },
    { IDC_O_FUSEPLOT, offsetof(GuiOpts, fuseplot) },
    { IDC_O_LST, offsetof(GuiOpts, lst) },
    { IDC_O_ABS, offsetof(GuiOpts, abs) },
    { IDC_O_MX, offsetof(GuiOpts, mx) },
    { IDC_O_NAMESRC, offsetof(GuiOpts, namesrc) },
    { IDC_O_KB, offsetof(GuiOpts, kb) },
    { IDC_O_SECURE, offsetof(GuiOpts, secure) },
    { IDC_O_COMPAT, offsetof(GuiOpts, compat) },
    { IDC_O_SIMAFTER, offsetof(GuiOpts, simafter) },
    { IDC_O_SIMLST, offsetof(GuiOpts, simlst) },
    { IDC_O_SIMJED, offsetof(GuiOpts, simjed) },
    { IDC_O_SIMLATCH, offsetof(GuiOpts, simlatch) }
};
#define NOPTCTL (sizeof g_optctl / sizeof g_optctl[0])

static INT_PTR CALLBACK options_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    static char pld[MAX_PATH], sec[MAX_PATH + 8];
    static const char *const levels[] = {
        "0 - none", "1 - quick", "2", "3", "4 - strongest"
    };
    GuiOpts o;
    size_t i;
    int k;
    (void)l;
    switch (m) {
    case WM_INITDIALOG: {
        char si[MAX_PATH];
        int perfile = 0;
        HWND dev = GetDlgItem(h, IDC_O_DEVICE);
        center(h);
        pld[0] = sec[0] = 0;
        if (target(pld, si)) {
            char t[MAX_PATH + 16];
            gui_file_section(pld, sec, sizeof sec);
            perfile = gui_ini_has_section(g_ini, sec);
            snprintf(t, sizeof t, "Options - %s", gui_basename(pld));
            SetWindowTextA(h, t);
        } else {
            EnableWindow(GetDlgItem(h, IDC_O_PERFILE), FALSE);
        }
        opts_for(pld, &o);
        for (i = 0; i < NOPTCTL; i++)
            CheckDlgButton(h, g_optctl[i].id,
                           *(int *)((char *)&o + g_optctl[i].off) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_O_PERFILE, perfile ? BST_CHECKED : BST_UNCHECKED);
        for (k = 0; k < 5; k++)
            SendDlgItemMessageA(h, IDC_O_MIN, CB_ADDSTRING, 0, (LPARAM)levels[k]);
        SendDlgItemMessage(h, IDC_O_MIN, CB_SETCURSEL, (WPARAM)o.min, 0);
        SendMessageA(dev, CB_ADDSTRING, 0, (LPARAM)"(from the source)");
        for (k = 0; k < gui_ndevs; k++)
            SendMessageA(dev, CB_ADDSTRING, 0, (LPARAM)gui_devs[k].mn);
        k = o.device[0] ? (int)SendMessageA(dev, CB_FINDSTRINGEXACT, 0, (LPARAM)o.device) : 0;
        SendMessage(dev, CB_SETCURSEL, (WPARAM)(k < 0 ? 0 : k), 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            gui_opts_default(&o);
            for (i = 0; i < NOPTCTL; i++)
                *(int *)((char *)&o + g_optctl[i].off) =
                    IsDlgButtonChecked(h, g_optctl[i].id) == BST_CHECKED;
            k = (int)SendDlgItemMessage(h, IDC_O_MIN, CB_GETCURSEL, 0, 0);
            o.min = k < 0 ? 1 : k;
            k = (int)SendDlgItemMessage(h, IDC_O_DEVICE, CB_GETCURSEL, 0, 0);
            if (k > 0 && k <= gui_ndevs) lstrcpynA(o.device, gui_devs[k - 1].mn, sizeof o.device);
            if (sec[0] && IsDlgButtonChecked(h, IDC_O_PERFILE) == BST_CHECKED) {
                gui_opts_save(g_ini, sec, &o);
            } else {
                if (sec[0]) gui_ini_del_section(g_ini, sec);
                gui_opts_save(g_ini, "options", &o);
            }
            settings_save();
            EndDialog(h, 1);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* column the device list is sorted by (-1: order of the table) and the
   direction; kept while the program runs */
static int g_dev_col = -1, g_dev_desc;

static void devices_fill(HWND h)
{
    HWND lv = GetDlgItem(h, IDC_D_LIST), hd = (HWND)SendMessage(lv, LVM_GETHEADER, 0, 0);
    char filter[80], b[64];
    int i, k, n, keep = -1, sel = 0, *idx;
    GetDlgItemTextA(h, IDC_D_FILTER, filter, sizeof filter);
    /* the selected device stays selected when it is still listed */
    i = (int)SendMessage(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (i >= 0) {
        LVITEMA it;
        memset(&it, 0, sizeof it);
        it.mask = LVIF_PARAM;
        it.iItem = i;
        if (SendMessage(lv, LVM_GETITEMA, 0, (LPARAM)&it)) keep = (int)it.lParam;
    }
    idx = (int *)malloc(sizeof(int) * (size_t)(gui_ndevs + 1));
    if (!idx) return;
    n = gui_dev_list(filter, g_dev_col, g_dev_desc, idx);
    SendMessage(lv, WM_SETREDRAW, FALSE, 0);
    SendMessage(lv, LVM_DELETEALLITEMS, 0, 0);
    for (k = 0; k < n; k++) {
        LVITEMA it;
        i = idx[k];
        if (i == keep) sel = k;
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = k;
        it.pszText = (char *)gui_devs[i].mn;
        it.lParam = i;
        SendMessage(lv, LVM_INSERTITEMA, 0, (LPARAM)&it);
        snprintf(b, sizeof b, "%d", gui_devs[i].pins);
        it.mask = LVIF_TEXT;
        it.iSubItem = 1;
        it.pszText = b;
        SendMessage(lv, LVM_SETITEMA, 0, (LPARAM)&it);
        it.iSubItem = 2;
        it.pszText = (char *)gui_devs[i].part;
        SendMessage(lv, LVM_SETITEMA, 0, (LPARAM)&it);
    }
    free(idx);
    /* the arrow in the header of the sorted column */
    for (k = 0; hd && k < 3; k++) {
        HDITEMA hi;
        memset(&hi, 0, sizeof hi);
        hi.mask = HDI_FORMAT;
        if (!SendMessage(hd, HDM_GETITEMA, (WPARAM)k, (LPARAM)&hi)) continue;
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (k == g_dev_col) hi.fmt |= g_dev_desc ? HDF_SORTDOWN : HDF_SORTUP;
        SendMessage(hd, HDM_SETITEMA, (WPARAM)k, (LPARAM)&hi);
    }
    if (n) {
        LVITEMA it;
        memset(&it, 0, sizeof it);
        it.stateMask = it.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessage(lv, LVM_SETITEMSTATE, (WPARAM)sel, (LPARAM)&it);
        SendMessage(lv, LVM_ENSUREVISIBLE, (WPARAM)sel, FALSE);
    }
    SendMessage(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
    snprintf(b, sizeof b, "%d of %d", n, gui_ndevs);
    SetDlgItemTextA(h, IDC_D_COUNT, b);
}

/* returns the index into gui_devs + 1, or 0 */
static INT_PTR CALLBACK devices_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    HWND lv = GetDlgItem(h, IDC_D_LIST);
    switch (m) {
    case WM_INITDIALOG: {
        static const struct {
            const char *t;
            int w;
        } col[] = { { "Mnemonic", 110 }, { "Pins", 56 }, { "Part", 400 } };
        Doc *d = cur();
        int i;
        center(h);
        SendMessage(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);
        for (i = 0; i < 3; i++) {
            LVCOLUMNA c;
            memset(&c, 0, sizeof c);
            c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
            c.fmt = i == 1 ? LVCFMT_RIGHT : LVCFMT_LEFT;
            c.cx = scale(col[i].w);
            c.pszText = (char *)col[i].t;
            c.iSubItem = i;
            SendMessage(lv, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&c);
        }
        SendMessage(lv, LVM_SETCOLUMNWIDTH, 2, LVSCW_AUTOSIZE_USEHEADER);
        if (!d || d->kind != DOC_SRC) EnableWindow(GetDlgItem(h, IDOK), FALSE);
        devices_fill(h);
        return TRUE;
    }
    case WM_NOTIFY:
        if (((NMHDR *)l)->idFrom == IDC_D_LIST && ((NMHDR *)l)->code == LVN_COLUMNCLICK) {
            /* a click on a column header sorts by it, a second one reverses */
            int c = ((NMLISTVIEW *)l)->iSubItem;
            if (c == g_dev_col) g_dev_desc = !g_dev_desc;
            else { g_dev_col = c; g_dev_desc = 0; }
            devices_fill(h);
            return TRUE;
        }
        if (((NMHDR *)l)->idFrom == IDC_D_LIST && ((NMHDR *)l)->code == NM_DBLCLK &&
            IsWindowEnabled(GetDlgItem(h, IDOK)))
            PostMessage(h, WM_COMMAND, IDOK, 0);
        break;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_D_FILTER && HIWORD(w) == EN_CHANGE) {
            devices_fill(h);
            return TRUE;
        }
        if (LOWORD(w) == IDOK) {
            LVITEMA it;
            int sel = (int)SendMessage(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
            if (sel < 0) return TRUE;
            memset(&it, 0, sizeof it);
            it.mask = LVIF_PARAM;
            it.iItem = sel;
            SendMessage(lv, LVM_GETITEMA, 0, (LPARAM)&it);
            EndDialog(h, it.lParam + 1);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* Replace the Device statement, or put one on a new line at the caret. */
static void insert_device(const char *mn)
{
    Doc *d = cur();
    unsigned char *t;
    long n = 0;
    size_t a, b;
    char stmt[64];
    CHARRANGE cr;
    if (!d || d->kind != DOC_SRC) return;
    t = doc_text(d, &n);
    if (!t) return;
    snprintf(stmt, sizeof stmt, "Device   %s ;", mn);
    if (gui_device_stmt((const char *)t, (size_t)n, &a, &b)) {
        cr.cpMin = (long)a;
        cr.cpMax = (long)b;
    } else {
        long line;
        SendMessage(d->edit, EM_EXGETSEL, 0, (LPARAM)&cr);
        line = (long)SendMessage(d->edit, EM_EXLINEFROMCHAR, 0, cr.cpMin);
        cr.cpMin = cr.cpMax = (long)SendMessage(d->edit, EM_LINEINDEX, (WPARAM)line, 0);
        lstrcatA(stmt, "\r\n");
    }
    free(t);
    SendMessage(d->edit, EM_EXSETSEL, 0, (LPARAM)&cr);
    SendMessage(d->edit, EM_REPLACESEL, TRUE, (LPARAM)stmt);
    SetFocus(d->edit);
}

static void choose_font(void)
{
    CHOOSEFONTA cf;
    LOGFONTA lf;
    int i;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -MulDiv(g_pt, g_dpi, 72);
    lf.lfCharSet = g_charset;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    lstrcpynA(lf.lfFaceName, g_face, LF_FACESIZE);
    memset(&cf, 0, sizeof cf);
    cf.lStructSize = sizeof cf;
    cf.hwndOwner = g_main;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_FIXEDPITCHONLY | CF_INITTOLOGFONTSTRUCT |
               CF_NOSCRIPTSEL | CF_FORCEFONTEXIST;
    if (!ChooseFontA(&cf)) return;
    lstrcpynA(g_face, lf.lfFaceName, sizeof g_face);
    g_pt = cf.iPointSize / 10;
    if (g_pt < 6) g_pt = 6;
    if (g_pt > 72) g_pt = 72;
    g_fontset = 1;
    /* both panes */
    for (i = 0; i < g_ndocs; i++) doc_format(g_docs[i]);
    list_font();
    settings_save();
}

/* ---- menus and toolbar ---- */

static const struct {
    int id, std, img;
    const char *tip;
} g_buttons[] = {
    { ID_FILE_NEW, 1, STD_FILENEW, "New" },
    { ID_FILE_OPEN, 1, STD_FILEOPEN, "Open" },
    { ID_FILE_SAVE, 1, STD_FILESAVE, "Save" },
    { 0, 0, 0, NULL },
    { ID_EDIT_CUT, 1, STD_CUT, "Cut" },
    { ID_EDIT_COPY, 1, STD_COPY, "Copy" },
    { ID_EDIT_PASTE, 1, STD_PASTE, "Paste" },
    { 0, 0, 0, NULL },
    { ID_EDIT_UNDO, 1, STD_UNDO, "Undo" },
    { ID_EDIT_REDO, 1, STD_REDOW, "Redo" },
    { 0, 0, 0, NULL },
    { ID_EDIT_FIND, 1, STD_FIND, "Find" },
    { 0, 0, 0, NULL },
    { ID_RUN_COMPILE, 0, 0, "Compile (F7)" },
    { ID_RUN_SIMULATE, 0, 1, "Simulate (F8)" },
    { ID_RUN_STOP, 0, 2, "Stop" },
    { 0, 0, 0, NULL },
    { ID_RUN_OPTIONS, 1, STD_PROPERTIES, "Options (F9)" },
    { ID_EDIT_DEVICE, 0, 3, "Devices" }
};
#define NBUTTONS (sizeof g_buttons / sizeof g_buttons[0])

static void toolbar_create(void)
{
    TBADDBITMAP ab;
    TBBUTTON tb[NBUTTONS];
    int big = g_dpi >= 144, sz = big ? 24 : 16, std, own;
    size_t i;

    g_tool = CreateWindowExA(0, TOOLBARCLASSNAMEA, NULL,
                             WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP,
                             0, 0, 0, 0, g_main, (HMENU)(INT_PTR)IDC_TOOLBAR, g_inst, NULL);
    SendMessage(g_tool, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessage(g_tool, TB_SETBITMAPSIZE, 0, MAKELPARAM(sz, sz));
    ab.hInst = HINST_COMMCTRL;
    ab.nID = big ? IDB_STD_LARGE_COLOR : IDB_STD_SMALL_COLOR;
    std = (int)SendMessage(g_tool, TB_ADDBITMAP, 0, (LPARAM)&ab);
    ab.hInst = g_inst;
    ab.nID = big ? IDB_TOOLS24 : IDB_TOOLS16;
    own = (int)SendMessage(g_tool, TB_ADDBITMAP, 4, (LPARAM)&ab);
    memset(tb, 0, sizeof tb);
    for (i = 0; i < NBUTTONS; i++) {
        if (!g_buttons[i].id) {
            tb[i].fsStyle = TBSTYLE_SEP;
            continue;
        }
        tb[i].iBitmap = (g_buttons[i].std ? std : own) + g_buttons[i].img;
        tb[i].idCommand = g_buttons[i].id;
        tb[i].fsState = TBSTATE_ENABLED;
        tb[i].fsStyle = TBSTYLE_BUTTON;
    }
    SendMessage(g_tool, TB_ADDBUTTONS, NBUTTONS, (LPARAM)tb);
}

static void update_ui(void)
{
    Doc *d = cur();
    int src = d && d->kind == DOC_SRC;
    SendMessage(g_tool, TB_ENABLEBUTTON, ID_FILE_SAVE, src);
    SendMessage(g_tool, TB_ENABLEBUTTON, ID_EDIT_CUT, src);
    SendMessage(g_tool, TB_ENABLEBUTTON, ID_EDIT_PASTE, src);
    SendMessage(g_tool, TB_ENABLEBUTTON, ID_EDIT_UNDO, src);
    SendMessage(g_tool, TB_ENABLEBUTTON, ID_EDIT_REDO, src);
    SendMessage(g_tool, TB_ENABLEBUTTON, ID_EDIT_COPY, d != NULL);
    SendMessage(g_tool, TB_ENABLEBUTTON, ID_EDIT_FIND, d != NULL);
    SendMessage(g_tool, TB_ENABLEBUTTON, ID_RUN_COMPILE, !g_run.active);
    SendMessage(g_tool, TB_ENABLEBUTTON, ID_RUN_SIMULATE, !g_run.active);
    SendMessage(g_tool, TB_ENABLEBUTTON, ID_RUN_STOP, g_run.active);
}

static void menu_update(HMENU m)
{
    Doc *d = cur();
    int src = d && d->kind == DOC_SRC, i, pos, any = 0;
    CHARRANGE sel = { 0, 0 };
    char pld[MAX_PATH], si[MAX_PATH];
    int tgt = target(pld, si);
#define EN(id, on) EnableMenuItem(m, id, MF_BYCOMMAND | ((on) ? MF_ENABLED : MF_GRAYED))
    if (d) SendMessage(d->edit, EM_EXGETSEL, 0, (LPARAM)&sel);
    EN(ID_FILE_SAVE, src);
    EN(ID_FILE_SAVEAS, src);
    EN(ID_FILE_SAVEALL, g_ndocs > 0);
    EN(ID_FILE_CLOSE, d != NULL);
    EN(ID_EDIT_UNDO, src && SendMessage(d->edit, EM_CANUNDO, 0, 0));
    EN(ID_EDIT_REDO, src && SendMessage(d->edit, EM_CANREDO, 0, 0));
    EN(ID_EDIT_CUT, src && sel.cpMax > sel.cpMin);
    EN(ID_EDIT_COPY, d && sel.cpMax > sel.cpMin);
    EN(ID_EDIT_PASTE, src && SendMessage(d->edit, EM_CANPASTE, CF_TEXT, 0));
    EN(ID_EDIT_DELETE, src && sel.cpMax > sel.cpMin);
    EN(ID_EDIT_SELALL, d != NULL);
    EN(ID_EDIT_FIND, d != NULL);
    EN(ID_EDIT_FINDNEXT, d && g_findwhat[0]);
    EN(ID_EDIT_REPLACE, src);
    EN(ID_EDIT_GOTO, d != NULL);
    for (i = ID_VIEW_JED; i <= ID_VIEW_PLD; i++) EN(i, tgt);
    EN(ID_VIEW_COLOUR, g_colour_ok);
    EN(ID_VIEW_NEXT, g_ndocs > 1);
    EN(ID_VIEW_PREV, g_ndocs > 1);
    EN(ID_RUN_COMPILE, !g_run.active);
    EN(ID_RUN_SIMULATE, !g_run.active);
    EN(ID_RUN_STOP, g_run.active);
    EN(ID_MSG_CLEAR, !g_run.active);
#undef EN
    CheckMenuItem(m, ID_VIEW_COLOUR, MF_BYCOMMAND | (g_colour && g_colour_ok ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_VIEW_TAB2, MF_BYCOMMAND | (g_tabw == 2 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_VIEW_TAB4, MF_BYCOMMAND | (g_tabw == 4 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_VIEW_TAB8, MF_BYCOMMAND | (g_tabw == 8 ? MF_CHECKED : MF_UNCHECKED));

    /* recent files before Exit */
    if (GetMenuState(m, ID_FILE_EXIT, MF_BYCOMMAND) == (UINT)-1) return;
    for (i = 0; i < GUI_NRECENT; i++) DeleteMenu(m, (UINT)(ID_RECENT_FIRST + i), MF_BYCOMMAND);
    pos = GetMenuItemCount(m) - 1;
    if (g_recentsep) {
        DeleteMenu(m, (UINT)(pos - 1), MF_BYPOSITION);
        g_recentsep = 0;
        pos--;
    }
    for (i = 0; i < GUI_NRECENT; i++) {
        char b[GUI_PATH + 8];
        if (!g_recent[i][0]) continue;
        snprintf(b, sizeof b, "&%d %s", i + 1, g_recent[i]);
        InsertMenuA(m, (UINT)pos++, MF_BYPOSITION | MF_STRING, (UINT_PTR)(ID_RECENT_FIRST + i), b);
        any = 1;
    }
    if (any) {
        InsertMenuA(m, (UINT)pos, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
        g_recentsep = 1;
    }
}

static void command(int id)
{
    Doc *d = cur();
    int i;
    if (id >= ID_RECENT_FIRST && id < ID_RECENT_FIRST + GUI_NRECENT) {
        char path[GUI_PATH];
        lstrcpynA(path, g_recent[id - ID_RECENT_FIRST], GUI_PATH);
        if (path[0]) open_file(path);
        return;
    }
    if (id >= ID_TAB_FIRST && id < ID_TAB_FIRST + 9) {
        doc_activate(id - ID_TAB_FIRST);
        return;
    }
    switch (id) {
    case ID_FILE_NEW: new_file(); break;
    case ID_FILE_OPEN: open_dialog(); break;
    case ID_FILE_SAVE: if (d) doc_save(d, 0); break;
    case ID_FILE_SAVEAS: if (d) doc_save(d, 1); break;
    case ID_FILE_SAVEALL:
        for (i = 0; i < g_ndocs; i++)
            if (g_docs[i]->kind == DOC_SRC &&
                (SendMessage(g_docs[i]->edit, EM_GETMODIFY, 0, 0) || !g_docs[i]->path[0]) &&
                !doc_save(g_docs[i], 0))
                break;
        break;
    case ID_FILE_CLOSE: if (d) doc_close(g_cur); break;
    case ID_FILE_EXIT: SendMessage(g_main, WM_CLOSE, 0, 0); break;

    case ID_EDIT_UNDO: if (d) SendMessage(d->edit, EM_UNDO, 0, 0); break;
    case ID_EDIT_REDO: if (d) SendMessage(d->edit, EM_REDO, 0, 0); break;
    case ID_EDIT_CUT: if (d) SendMessage(d->edit, WM_CUT, 0, 0); break;
    case ID_EDIT_COPY:
        if (GetFocus() == g_list) msg_copy(0);
        else if (d) SendMessage(d->edit, WM_COPY, 0, 0);
        break;
    case ID_EDIT_PASTE:
        /* text only */
        if (d && d->kind == DOC_SRC) SendMessage(d->edit, EM_PASTESPECIAL, CF_TEXT, 0);
        break;
    case ID_EDIT_DELETE: if (d) SendMessage(d->edit, WM_CLEAR, 0, 0); break;
    case ID_EDIT_SELALL:
        if (d) {
            CHARRANGE all = { 0, -1 };
            SendMessage(d->edit, EM_EXSETSEL, 0, (LPARAM)&all);
        }
        break;
    case ID_EDIT_FIND: find_dialog(0); break;
    case ID_EDIT_REPLACE: find_dialog(1); break;
    case ID_EDIT_FINDNEXT:
        if (g_findwhat[0]) find_next(g_findflags, 0);
        else find_dialog(0);
        break;
    case ID_EDIT_GOTO:
        if (d) {
            CHARRANGE cr;
            INT_PTR line;
            SendMessage(d->edit, EM_EXGETSEL, 0, (LPARAM)&cr);
            line = DialogBoxParamA(g_inst, MAKEINTRESOURCEA(IDD_GOTO), g_main, goto_proc,
                                   SendMessage(d->edit, EM_EXLINEFROMCHAR, 0, cr.cpMin) + 1);
            if (line > 0) goto_line(d, (int)line);
        }
        break;
    case ID_EDIT_DEVICE: {
        INT_PTR r = DialogBoxParamA(g_inst, MAKEINTRESOURCEA(IDD_DEVICES), g_main,
                                    devices_proc, 0);
        if (r > 0) insert_device(gui_devs[r - 1].mn);
        break;
    }

    case ID_VIEW_PLD: view_output(".pld"); break;
    case ID_VIEW_SI: view_output(".si"); break;
    case ID_VIEW_JED: view_output(".jed"); break;
    case ID_VIEW_DOC: view_output(".doc"); break;
    case ID_VIEW_LST: view_output(".lst"); break;
    case ID_VIEW_SO: view_output(".so"); break;
    case ID_VIEW_COLOUR:
        g_colour = !g_colour;
        for (i = 0; i < g_ndocs; i++) {
            colour_off(g_docs[i]);
            colour_scan(g_docs[i], 1);
        }
        break;
    case ID_VIEW_FONT: choose_font(); break;
    case ID_VIEW_TAB2:
    case ID_VIEW_TAB4:
    case ID_VIEW_TAB8:
        g_tabw = id == ID_VIEW_TAB2 ? 2 : id == ID_VIEW_TAB4 ? 4 : 8;
        for (i = 0; i < g_ndocs; i++) doc_format(g_docs[i]);
        break;
    case ID_VIEW_NEXT: if (g_ndocs > 1) doc_activate((g_cur + 1) % g_ndocs); break;
    case ID_VIEW_PREV: if (g_ndocs > 1) doc_activate((g_cur + g_ndocs - 1) % g_ndocs); break;

    case ID_RUN_COMPILE: run(STEP_COMPILE); break;
    case ID_RUN_SIMULATE: run(STEP_SIM); break;
    case ID_RUN_STOP: run_stop(); break;
    case ID_RUN_OPTIONS:
        DialogBoxParamA(g_inst, MAKEINTRESOURCEA(IDD_OPTIONS), g_main, options_proc, 0);
        break;

    case ID_HELP_GUIDE: show_guide(); break;
    case ID_HELP_ABOUT:
        DialogBoxParamA(g_inst, MAKEINTRESOURCEA(IDD_ABOUT), g_main, about_proc, 0);
        break;

    case ID_MSG_COPY: msg_copy(0); break;
    case ID_MSG_COPYALL: msg_copy(1); break;
    case ID_MSG_CLEAR: if (!g_run.active) msg_clear(); break;
    }
}

static void drop_files(HDROP drop)
{
    UINT n = DragQueryFileA(drop, 0xffffffffu, NULL, 0), i;
    for (i = 0; i < n; i++) {
        char path[MAX_PATH];
        if (DragQueryFileA(drop, i, path, MAX_PATH)) open_file(path);
    }
}

/* ---- main window ---- */

static int close_all(void)
{
    int i;
    for (i = 0; i < g_ndocs; i++) {
        Doc *d = g_docs[i];
        int r;
        if (d->kind != DOC_SRC || !SendMessage(d->edit, EM_GETMODIFY, 0, 0)) continue;
        doc_activate(i);
        r = msgbox(MB_YESNOCANCEL | MB_ICONQUESTION, "Save changes to %s?", d->title);
        if (r == IDCANCEL) return 0;
        if (r == IDYES && !doc_save(d, 0)) return 0;
    }
    return 1;
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == g_findmsg && g_findmsg) {
        find_message((const FINDREPLACEA *)l);
        return 0;
    }
    switch (m) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) layout();
        return 0;
    case WM_SETFOCUS:
        if (cur()) SetFocus(cur()->edit);
        return 0;
    case WM_INITMENUPOPUP:
        if (!HIWORD(l)) menu_update((HMENU)w);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_EDIT && HIWORD(w) == EN_CHANGE) {
            Doc *d = cur();
            if (d && (HWND)l == d->edit && !d->loading) {
                doc_modified(d);
                /* the colouring state is of the old text until then */
                KillTimer(h, TIMER_SLICE);
                if (colour_wanted(d)) SetTimer(h, TIMER_COLOUR, 30, NULL);
            }
            return 0;
        }
        if (LOWORD(w) == IDC_MSGLIST) {
            if (HIWORD(w) == LBN_DBLCLK)
                msg_open((int)SendMessage(g_list, LB_GETCURSEL, 0, 0));
            return 0;
        }
        if (l == 0 || (HWND)l == g_tool) command(LOWORD(w));
        return 0;
    case WM_TIMER:
        if (w == TIMER_COLOUR) {
            KillTimer(h, TIMER_COLOUR);
            if (cur()) colour_scan(cur(), 0);
        } else if (w == TIMER_SLICE) {
            colour_slice();
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)l;
        if (nm->hwndFrom == g_tab && nm->code == TCN_SELCHANGE) {
            doc_activate((int)SendMessage(g_tab, TCM_GETCURSEL, 0, 0));
        } else if (nm->code == EN_SELCHANGE && cur() && nm->hwndFrom == cur()->edit) {
            update_pos();
        } else if (nm->code == EN_DROPFILES) {
            drop_files((HDROP)((ENDROPFILES *)l)->hDrop);
            return 0;
        } else if (nm->code == TTN_NEEDTEXTA) {
            size_t i;
            for (i = 0; i < NBUTTONS; i++)
                if (g_buttons[i].id && (UINT_PTR)g_buttons[i].id == nm->idFrom)
                    ((NMTTDISPINFOA *)l)->lpszText = (char *)g_buttons[i].tip;
        }
        return 0;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
        if (mi->CtlID == IDC_MSGLIST) {
            HDC dc = GetDC(h);
            HFONT old = SelectObject(dc, g_listfont);
            TEXTMETRICA tm;
            GetTextMetricsA(dc, &tm);
            mi->itemHeight = (UINT)(tm.tmHeight + 2);
            SelectObject(dc, old);
            ReleaseDC(h, dc);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)l)->CtlID == IDC_MSGLIST) {
            msg_draw((const DRAWITEMSTRUCT *)l);
            return TRUE;
        }
        break;
    case WM_CONTEXTMENU:
        if ((HWND)w == g_list) {
            HMENU mm = LoadMenuA(g_inst, MAKEINTRESOURCEA(IDR_MSGMENU));
            POINT pt;
            pt.x = (short)LOWORD(l);
            pt.y = (short)HIWORD(l);
            if (pt.x == -1 && pt.y == -1) {
                pt.x = pt.y = 0;
                ClientToScreen(g_list, &pt);
            }
            TrackPopupMenu(GetSubMenu(mm, 0), TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
            DestroyMenu(mm);
            return 0;
        }
        break;
    case WM_APP_LINE:
        msg_tool_line((const char *)l);
        free((void *)l);
        return 0;
    case WM_APP_DONE: {
        DWORD rc;
        WaitForSingleObject(g_run.thread, INFINITE);
        CloseHandle(g_run.thread);
        g_run.thread = NULL;
        rc = proc_finish(&g_run.proc);
        step_done(rc);
        return 0;
    }
    case WM_DROPFILES:
        drop_files((HDROP)w);
        DragFinish((HDROP)w);
        return 0;
    case WM_SETCURSOR:
        if ((HWND)w == h && LOWORD(l) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (pt.y >= g_split && pt.y < g_split + scale(5)) {
                SetCursor(LoadCursor(NULL, IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if ((short)HIWORD(l) >= g_split && (short)HIWORD(l) < g_split + scale(5)) {
            g_dragging = 1;
            SetCapture(h);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            RECT rc, r;
            GetClientRect(h, &rc);
            GetWindowRect(g_status, &r);
            g_msgh = rc.bottom - (r.bottom - r.top) - (short)HIWORD(l) - scale(2);
            layout();
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = 0;
            ReleaseCapture();
        }
        return 0;
    case WM_SYSCOLORCHANGE:
        SendMessage(g_tool, m, w, l);
        SendMessage(g_status, m, w, l);
        return 0;
    case WM_QUERYENDSESSION:
        if (!close_all()) return FALSE;
        settings_save();
        return TRUE;
    case WM_CLOSE:
        if (!close_all()) return 0;
        if (g_run.active) {
            run_stop();
            WaitForSingleObject(g_run.thread, 3000);
        }
        settings_save();
        while (g_ndocs) doc_free(g_docs[--g_ndocs]);
        g_cur = -1;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void place_window(void)
{
    WINDOWPLACEMENT wp;
    int x = gui_ini_int(g_ini, "window", "x", -32000);
    int y = gui_ini_int(g_ini, "window", "y", -32000);
    int w = gui_ini_int(g_ini, "window", "w", 0);
    int hh = gui_ini_int(g_ini, "window", "h", 0);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w < 200 || hh < 150) return;
    /* the title bar must be reachable */
    if (x + w < vx + 50 || x > vx + vw - 50 || y < vy - 10 || y > vy + vh - 50) return;
    wp.length = sizeof wp;
    if (!GetWindowPlacement(g_main, &wp)) return;
    wp.rcNormalPosition.left = x;
    wp.rcNormalPosition.top = y;
    wp.rcNormalPosition.right = x + w;
    wp.rcNormalPosition.bottom = y + hh;
    wp.showCmd = SW_HIDE;
    SetWindowPlacement(g_main, &wp);
}

int main(int argc, char **argv)
{
    WNDCLASSEXA wc;
    INITCOMMONCONTROLSEX ic;
    NONCLIENTMETRICSA ncm;
    MSG msg;
    HDC dc;
    char *p;
    int i;

    g_inst = GetModuleHandleA(NULL);
    GetModuleFileNameA(NULL, g_exedir, MAX_PATH);
    p = strrchr(g_exedir, '\\');
    if (p) *p = 0;
    snprintf(g_inipath, sizeof g_inipath, "%s\\freecuplw.ini", g_exedir);

    ic.dwSize = sizeof ic;
    ic.dwICC = ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&ic);
    if (!LoadLibraryA("riched20.dll")) {
        MessageBoxA(NULL, "riched20.dll cannot be loaded.", APPNAME, MB_ICONERROR);
        return 1;
    }
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (g_dpi < 48) g_dpi = 96;
    settings_load();
    if (!g_ini) return 1;
    font_default();
    elevation();
    g_findmsg = RegisterWindowMessageA(FINDMSGSTRINGA);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = main_proc;
    wc.hInstance = g_inst;
    wc.hIcon = LoadIconA(g_inst, MAKEINTRESOURCEA(IDI_APP));
    wc.hIconSm = LoadImageA(g_inst, MAKEINTRESOURCEA(IDI_APP), IMAGE_ICON,
                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszMenuName = MAKEINTRESOURCEA(IDR_MAIN);
    wc.lpszClassName = CLASSNAME;
    if (!RegisterClassExA(&wc)) return 1;
    g_main = CreateWindowExA(WS_EX_ACCEPTFILES, CLASSNAME, APPNAME,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, scale(860), scale(640),
                             NULL, NULL, g_inst, NULL);
    if (!g_main) return 1;

    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        g_uifont = CreateFontIndirectA(&ncm.lfMessageFont);
    if (!g_uifont) g_uifont = GetStockObject(DEFAULT_GUI_FONT);
    list_font();
    toolbar_create();
    g_status = CreateWindowExA(0, STATUSCLASSNAMEA, NULL,
                               WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                               g_main, (HMENU)(INT_PTR)IDC_STATUS, g_inst, NULL);
    g_tab = CreateWindowExA(0, WC_TABCONTROLA, NULL,
                            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER,
                            0, 0, 0, 0, g_main, (HMENU)(INT_PTR)IDC_TAB, g_inst, NULL);
    SendMessage(g_tab, WM_SETFONT, (WPARAM)g_uifont, FALSE);
    g_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                             LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                             LBS_HASSTRINGS,
                             0, 0, 0, 0, g_main, (HMENU)(INT_PTR)IDC_MSGLIST, g_inst, NULL);
    SendMessage(g_list, WM_SETFONT, (WPARAM)g_listfont, FALSE);
    drop_allow(g_main);
    g_accel = LoadAcceleratorsA(g_inst, MAKEINTRESOURCEA(IDR_MAIN));

    place_window();
    ShowWindow(g_main, gui_ini_int(g_ini, "window", "max", 0) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    layout();
    update_ui();
    update_pos();
    status("Ready");
    for (i = 1; i < argc; i++) open_file(argv[i]);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        /* PrintScreen is never a command or a dialog key here */
        int snap = msg.wParam == VK_SNAPSHOT &&
                   (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP ||
                    msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP);
        if (!snap) {
            if (g_find && IsDialogMessageA(g_find, &msg)) continue;
            if (TranslateAcceleratorA(g_main, g_accel, &msg)) continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    gui_ini_free(g_ini);
    return (int)msg.wParam;
}
