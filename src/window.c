// shakedown.exe: the window. Live view of every core, buttons for the runs, the verdict and the history.
// It exists only while it is open: no autostart, nothing left running. The tray icon is there for the same time,
// so a run can be watched (and its result seen) with the window minimised.
//
// Drawn with Direct2D on the CPU into a bitmap, like Blackout's panel, and copied to the window. A frame is drawn
// when a sample arrives (every 2 s) or the mouse changes something.
//
// The engine runs on one worker thread. The window thread never touches sensors: it gets a copy of the Live view.
#define SHAKEDOWN_WINDOW
#define COBJMACROS
#include "engine.c"
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>

#define WIN_CLASS L"ShakedownWindow"
#define WIN_W     880.0f
#define WIN_H     616.0f
#define PAD       16.0f
#define INSET     14.0f
#define LABEL     22.0f
#define SECT      18.0f
#define LEFT_W    424.0f
#define RIGHT_X   456.0f
#define RIGHT_W   408.0f
#define PROW      30.0f
#define ACCENT    0xE5484D
#define WARN      0xF0A35B
#define GOOD      0x3FB950
#define WM_LIVE   (WM_APP + 1)
#define WM_DONE   (WM_APP + 2)
#define WM_TRAY   (WM_APP + 3)

enum { H_NONE, H_PASS, H_TEN, H_RANK, H_STOP, H_NOTE, H_INSTALL, H_CORE, H_COUNT = H_CORE + MAX_CPU };
enum { F_BRAND, F_ROW, F_SMALL, F_BIG, F_BOLD, F_TINY, F_COUNT };
enum { K_NONE, K_PASS, K_TEN, K_RANK, K_SENSORS };

static const GUID IID_D2DFactory = {0x06152247, 0x6f50, 0x465a, {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
static const GUID IID_DWFactory = {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
typedef HRESULT(WINAPI *D2DCreate)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);
typedef HRESULT(WINAPI *DWCreate)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

static ID2D1Factory *d2f;
static IDWriteFactory *dwf;
static ID2D1DCRenderTarget *dcrt;
static ID2D1RenderTarget *rt;
static ID2D1SolidColorBrush *brush;
static IDWriteTextFormat *fmt[F_COUNT];
static HDC surfDc;
static HBITMAP surfBmp;
static void *surfBits;
static int surfW, surfH;
static float scale = 1;
static BOOL light;
static D2D1_RECT_F hit[H_COUNT];
static int hover;
static HWND window;
static NOTIFYICONDATAW nid;

// What is on screen. Written by the window thread only.
static Live view;
static Result lastResult;
static Options lastOptions;
static char lastName[96], rankText[1024], problem[200];
static BOOL haveResult, running, noteFocus, shotMode;
static int runKind;
static ULONG64 heldOut;
static WCHAR note[121];
static struct { char when[16], result[8], text[120]; } history[3];
static int historyCount;

// Shared with the worker.
static CRITICAL_SECTION lock;
static Live sharedLive;
static Result sharedResult;
static Options sharedOptions, pendingOptions;
static char sharedName[96], sharedRank[1024], sharedProblem[200];
static volatile LONG pendingKind, quitting;
static HANDLE wake, workerThread;

// ---- Drawing primitives (the same set as Blackout's panel) ----

static UINT ink(void) { return light ? 0x1B1B1F : 0xF3F3F3; }
static UINT dim(void) { return light ? 0x5F5F66 : 0xA8A8AD; }
static UINT tone(void) { return light ? 0x000000 : 0xFFFFFF; }
static UINT warn(void) { return light ? 0xB45309 : WARN; }
static UINT good(void) { return light ? 0x1A7F37 : GOOD; }
static UINT base(void) { return light ? 0xEDEDED : 0x1C1E20; }

static void paint(UINT rgb, float a) {
    D2D1_COLOR_F c = {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a};
    ID2D1SolidColorBrush_SetColor(brush, &c);
}
static D2D1_RECT_F rc(float x, float y, float w, float h) {
    D2D1_RECT_F r = {x, y, x + w, y + h};
    return r;
}
static BOOL inside(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static void fillRound(D2D1_RECT_F r, float rad, UINT rgb, float a) {
    D2D1_ROUNDED_RECT rr = {r, rad, rad};
    paint(rgb, a);
    ID2D1RenderTarget_FillRoundedRectangle(rt, &rr, (ID2D1Brush *)brush);
}
static void strokeRound(D2D1_RECT_F r, float rad, UINT rgb, float a, float w) {
    D2D1_ROUNDED_RECT rr = {r, rad, rad};
    paint(rgb, a);
    ID2D1RenderTarget_DrawRoundedRectangle(rt, &rr, (ID2D1Brush *)brush, w, NULL);
}
static void dot(float x, float y, float r, UINT rgb, float a) {
    D2D1_ELLIPSE e = {{x, y}, r, r};
    paint(rgb, a);
    ID2D1RenderTarget_FillEllipse(rt, &e, (ID2D1Brush *)brush);
}
static void line(float x0, float y0, float x1, float y1, UINT rgb, float a, float w) {
    D2D1_POINT_2F p0 = {x0, y0}, p1 = {x1, y1};
    paint(rgb, a);
    ID2D1RenderTarget_DrawLine(rt, p0, p1, (ID2D1Brush *)brush, w, NULL);
}
static void text(LPCWSTR s, int f, D2D1_RECT_F r, UINT rgb, float a, DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextFormat_SetTextAlignment(fmt[f], align);
    paint(rgb, a);
    ID2D1RenderTarget_DrawText(rt, s, lstrlenW(s), fmt[f], &r, (ID2D1Brush *)brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}
static float measure(LPCWSTR s, int f) {
    IDWriteTextLayout *l;
    DWRITE_TEXT_METRICS m = {0};
    IDWriteTextFormat_SetTextAlignment(fmt[f], DWRITE_TEXT_ALIGNMENT_LEADING);
    if (IDWriteFactory_CreateTextLayout(dwf, s, lstrlenW(s), fmt[f], 2000, 100, &l) != S_OK) return 0;
    IDWriteTextLayout_GetMetrics(l, &m);
    IDWriteTextLayout_Release(l);
    return m.widthIncludingTrailingWhitespace;
}
static void card(float x, float y, float w, float h) {
    fillRound(rc(x, y, w, h), 8, tone(), 0.045f);
    strokeRound(rc(x, y, w, h), 8, tone(), 0.07f, 1);
}
static void section(LPCWSTR name, float x, float *y) {
    text(name, F_TINY, rc(x + 2, *y + 2, 300, 16), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    *y += LABEL;
}

static BOOL drawLoad(void) {
    static const struct { LPCWSTR face; DWRITE_FONT_WEIGHT weight; float size; } spec[F_COUNT] = {
        {L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 15}, {L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 13.5f},
        {L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 11.5f},    {L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 22},
        {L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13},    {L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 10.5f},
    };
    HMODULE d2dLib = LoadLibraryW(L"d2d1.dll"), dwLib = LoadLibraryW(L"dwrite.dll");
    D2DCreate mkD2d = d2dLib ? (D2DCreate)GetProcAddress(d2dLib, "D2D1CreateFactory") : NULL;
    DWCreate mkDw = dwLib ? (DWCreate)GetProcAddress(dwLib, "DWriteCreateFactory") : NULL;
    BOOL ok = mkD2d && mkDw && mkD2d(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_D2DFactory, NULL, (void **)&d2f) == S_OK &&
              mkDw(DWRITE_FACTORY_TYPE_SHARED, &IID_DWFactory, (IUnknown **)&dwf) == S_OK;
    for (int i = 0; ok && i < F_COUNT; i++) {
        ok = IDWriteFactory_CreateTextFormat(dwf, spec[i].face, NULL, spec[i].weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, spec[i].size, L"en-gb", &fmt[i]) == S_OK;
        if (ok) IDWriteTextFormat_SetWordWrapping(fmt[i], DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return ok;
}

// ---- The picture ----

static void number(int v, LPWSTR out) {
    if (v >= 1000) wsprintfW(out, L"%d,%03d", v / 1000, v % 1000);
    else wsprintfW(out, L"%d", v);
}

static UINT heat(int temp, int limit) {
    if (limit <= 0) limit = 100;
    return temp >= limit - 10 ? ACCENT : temp >= limit - 25 ? warn() : dim();
}

static const LiveCore *shown(int i, int *errors, int *first) {
    // Errors stay on screen after a run, until the next one starts.
    const LiveCore *c = &view.core[i];
    *errors = c->errors, *first = c->firstError;
    if (!running && haveResult) *errors = lastResult.core[i].errors, *first = lastResult.core[i].firstError;
    return c;
}

static BOOL isHeld(int i) { return ((running ? view.held : heldOut) >> i & 1) != 0; }

static void button(int id, LPCWSTR label, float x, float y, float w, BOOL enabled, BOOL strong) {
    D2D1_RECT_F r = rc(x, y, w, 34);
    if (enabled) hit[id] = r;
    if (strong && enabled) fillRound(r, 7, ACCENT, hover == id ? 0.85f : 1);
    else fillRound(r, 7, tone(), !enabled ? 0.03f : hover == id ? 0.14f : 0.08f);
    text(label, F_BOLD, rc(x, y + 8, w, 20), strong && enabled ? 0xFFFFFF : ink(), enabled ? 1 : 0.35f, DWRITE_TEXT_ALIGNMENT_CENTER);
}

static void reading(LPCWSTR value, LPCWSTR label, float x, float y, float w, UINT color) {
    text(value, F_BIG, rc(x, y + 12, w, 32), color, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    text(label, F_SMALL, rc(x, y + 46, w, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_CENTER);
}

static void paintAll(void) {
    WCHAR s[256], t[64];
    D2D1_COLOR_F clear = {((base() >> 16) & 255) / 255.0f, ((base() >> 8) & 255) / 255.0f, (base() & 255) / 255.0f, 1};
    float x = PAD, y = PAD;
    int errors, first, hottest = -1;
    ID2D1RenderTarget_Clear(rt, &clear);
    memset(hit, 0, sizeof(hit));
    for (int i = 0; i < coreCount; i++)
        if (view.core[i].temp >= 0 && (hottest < 0 || view.core[i].temp > view.core[hottest].temp)) hottest = i;

    // Header: name on the left, state on the right.
    for (int i = 0; i < 9; i++) dot(x + 5 + (i % 3) * 7, y + 9 + (i / 3) * 7, 2.4f, i == 5 ? ACCENT : ink(), i == 5 ? 1 : 0.85f);
    text(L"Shakedown", F_BRAND, rc(x + 30, y + 5, 200, 22), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    {
        UINT color = dim();
        if (running) {
            int sec = view.loadSeconds;
            LPCWSTR what = view.phase == PH_LOAD || view.phase == PH_START ? L"Running" : view.phase == PH_AFTER ? L"Finishing" : L"Starting";
            if (view.wanted) wsprintfW(s, L"%s  %d:%02d of %d:00", what, sec / 60, sec % 60, view.wanted / 60);
            else wsprintfW(s, L"%s  %d:%02d", what, sec / 60, sec % 60);
            if (runKind == K_RANK && view.round) wsprintfW(s + lstrlenW(s), L"  \x00B7  round %d", view.round);
            color = ink();
        } else if (haveResult) {
            lstrcpyW(s, lastResult.pass ? L"Passed" : L"Failed");
            color = lastResult.pass ? good() : ACCENT;
        } else {
            lstrcpyW(s, L"Ready");
        }
        float w = measure(s, F_BOLD);
        if (running) dot(WIN_W - PAD - w - 12, y + 16, 4, ACCENT, 1);
        text(s, F_BOLD, rc(WIN_W - PAD - w - 2, y + 7, w + 4, 20), color, 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    y += 32 + 12;
    float top = y;

    // Left: performance cores, one row each.
    section(L"PERFORMANCE CORES", x, &y);
    {
        float h = 24 + pCount * PROW + 6, ix = x + INSET;
        card(x, y, LEFT_W, h);
        text(L"Core", F_TINY, rc(ix, y + 7, 40, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        text(L"MHz", F_TINY, rc(ix + 40, y + 7, 62, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
        text(L"Temperature", F_TINY, rc(ix + 122, y + 7, 100, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        text(L"Busy", F_TINY, rc(ix + 232, y + 7, 40, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
        text(L"Errors", F_TINY, rc(ix + 290, y + 7, 100, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        float ry = y + 24;
        for (int i = 0; i < coreCount; i++) {
            if (!cores[i].perf) continue;
            const LiveCore *c = shown(i, &errors, &first);
            BOOL held = isHeld(i);
            float a = held ? 0.38f : 1;
            D2D1_RECT_F row = rc(x + 4, ry, LEFT_W - 8, PROW);
            if (!running) hit[H_CORE + i] = row;
            if (errors) fillRound(rc(row.left, row.top + 2, LEFT_W - 8, PROW - 4), 6, ACCENT, 0.16f);
            else if (hover == H_CORE + i) fillRound(rc(row.left, row.top + 2, LEFT_W - 8, PROW - 4), 6, tone(), 0.05f);
            wsprintfW(s, L"%S", cores[i].name);
            text(s, F_BOLD, rc(ix, ry + 6, 40, 20), ink(), a, DWRITE_TEXT_ALIGNMENT_LEADING);
            if (c->mhz) number(c->mhz, s);
            else lstrcpyW(s, L"-");
            text(s, F_ROW, rc(ix + 40, ry + 6, 62, 20), ink(), a, DWRITE_TEXT_ALIGNMENT_TRAILING);
            if (c->temp >= 0) {
                float span = (float)((view.tjmax > 0 ? view.tjmax : 100) - 20), part = (c->temp - 20) / span;
                part = part < 0.03f ? 0.03f : part > 1 ? 1 : part;
                wsprintfW(s, L"%d \x00B0" L"C", c->temp);
                text(s, F_ROW, rc(ix + 122, ry + 6, 50, 20), ink(), a, DWRITE_TEXT_ALIGNMENT_LEADING);
                fillRound(rc(ix + 176, ry + 13, 56, 5), 2.5f, tone(), 0.08f);
                fillRound(rc(ix + 176, ry + 13, 56 * part, 5), 2.5f, heat(c->temp, view.tjmax), a);
            } else {
                text(L"-", F_ROW, rc(ix + 122, ry + 6, 50, 20), dim(), a, DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            wsprintfW(s, L"%d%%", c->util);
            text(s, F_ROW, rc(ix + 232, ry + 6, 40, 20), c->util >= 70 ? ink() : dim(), a, DWRITE_TEXT_ALIGNMENT_TRAILING);
            if (held) lstrcpyW(s, L"held out");
            else if (errors) wsprintfW(s, L"%d  \x00B7  first at %d:%02d", errors, first / 60, first % 60);
            else if (c->flags & F_ANY) lstrcpyW(s, L"throttled");
            else lstrcpyW(s, L"-");
            text(s, errors && !held ? F_BOLD : F_ROW, rc(ix + 290, ry + 6, 112, 20), held ? dim() : errors ? ACCENT : c->flags & F_ANY ? warn() : dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            ry += PROW;
        }
        y += h + SECT;
    }
    // Left: efficiency cores as a grid of small cells.
    if (coreCount > pCount) {
        int perRow = 8, n = 0, count = coreCount - pCount, rows = (count + perRow - 1) / perRow;
        float gap = 6, cw = (LEFT_W - 2 * 10 - (perRow - 1) * gap) / perRow, ch = 36;
        section(L"EFFICIENCY CORES", x, &y);
        card(x, y, LEFT_W, rows * ch + (rows - 1) * gap + 20);
        for (int i = 0; i < coreCount; i++) {
            if (cores[i].perf) continue;
            const LiveCore *c = shown(i, &errors, &first);
            BOOL held = isHeld(i);
            float cx = x + 10 + (n % perRow) * (cw + gap), cy = y + 10 + (n / perRow) * (ch + gap), a = held ? 0.38f : 1;
            D2D1_RECT_F cell = rc(cx, cy, cw, ch);
            if (!running) hit[H_CORE + i] = cell;
            fillRound(cell, 6, errors ? ACCENT : tone(), errors ? 0.30f : hover == H_CORE + i ? 0.12f : 0.06f);
            wsprintfW(s, L"%S", cores[i].name);
            text(s, F_TINY, rc(cx, cy + 4, cw, 14), dim(), a, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (errors) wsprintfW(s, L"%d err", errors);
            else if (held) lstrcpyW(s, L"out");
            else if (c->temp >= 0) wsprintfW(s, L"%d\x00B0", c->temp);
            else if (c->mhz) number(c->mhz, s);
            else lstrcpyW(s, L"-");
            text(s, F_SMALL, rc(cx, cy + 17, cw, 16), errors ? ink() : c->temp >= 0 ? heat(c->temp, view.tjmax) == dim() ? ink() : heat(c->temp, view.tjmax) : ink(), a,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            n++;
        }
        y += rows * ch + (rows - 1) * gap + 20 + 10;
    }
    // Left, bottom: where the numbers come from.
    {
        BOOL full = source == SRC_DIRECT || source == SRC_HELPER;
        if (full) wsprintfW(s, L"Thermal limit %d \x00B0" L"C  \x00B7  click a core to hold it out of the next run", view.tjmax);
        else lstrcpyW(s, L"No temperatures yet.");
        text(s, F_TINY, rc(x + 2, y, LEFT_W, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (!full && source != SRC_NONE) {
            float w = measure(s, F_TINY);
            LPCWSTR link = L"Install the helper (one admin prompt)";
            hit[H_INSTALL] = rc(x + 2 + w + 6, y - 2, measure(link, F_TINY) + 4, 18);
            text(link, F_TINY, rc(x + 2 + w + 8, y, 260, 14), ACCENT, hover == H_INSTALL ? 0.8f : 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }

    // Right: the four readings.
    x = RIGHT_X, y = top;
    section(L"NOW", x, &y);
    {
        float cw = RIGHT_W / 4;
        int total = running ? view.errors : haveResult ? lastResult.errors : 0;
        card(x, y, RIGHT_W, 76);
        if (view.coolant >= 0) wsprintfW(s, L"%d.%d\x00B0", (int)view.coolant, (int)(view.coolant * 10 + 0.5) % 10);
        else lstrcpyW(s, L"-");
        reading(s, L"Water", x, y, cw, ink());
        if (view.pkgWatts > 0) wsprintfW(s, L"%d W", view.pkgWatts);
        else lstrcpyW(s, L"-");
        reading(s, L"Package", x + cw, y, cw, ink());
        if (hottest >= 0) wsprintfW(s, L"%d\x00B0", view.core[hottest].temp), wsprintfW(t, L"Hottest: %S", cores[hottest].name);
        else lstrcpyW(s, L"-"), lstrcpyW(t, L"Hottest core");
        reading(s, t, x + 2 * cw, y, cw, hottest >= 0 && heat(view.core[hottest].temp, view.tjmax) != dim() ? heat(view.core[hottest].temp, view.tjmax) : ink());
        wsprintfW(s, L"%d", total);
        reading(s, total == 1 ? L"Error" : L"Errors", x + 3 * cw, y, cw, total ? ACCENT : ink());
        y += 76 + SECT;
    }
    // Right: the test.
    section(L"TEST", x, &y);
    {
        float ix = x + INSET, iw = RIGHT_W - 2 * INSET, bw = (iw - 3 * 8) / 4;
        card(x, y, RIGHT_W, 126);
        D2D1_RECT_F box = rc(ix, y + INSET, iw, 34);
        if (!running) hit[H_NOTE] = box;
        fillRound(box, 7, tone(), noteFocus ? 0.09f : 0.05f);
        strokeRound(box, 7, noteFocus ? ACCENT : tone(), noteFocus ? 0.9f : 0.10f, 1);
        if (note[0]) {
            text(note, F_ROW, rc(ix + 10, y + INSET + 8, iw - 20, 20), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        } else if (!noteFocus) {
            text(L"What is being tested? For example: P0 P1 59x, rest 58x", F_ROW, rc(ix + 10, y + INSET + 8, iw - 20, 20), dim(), 0.8f, DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        if (noteFocus) {
            float cx = ix + 10 + measure(note, F_ROW) + 1;
            if (cx < ix + iw - 8) line(cx, y + INSET + 8, cx, y + INSET + 26, ink(), 1, 1);
        }
        float by = y + INSET + 34 + 8;
        button(H_PASS, L"Single pass", ix, by, bw, !running, FALSE);
        button(H_TEN, L"10 minutes", ix + (bw + 8), by, bw, !running, FALSE);
        button(H_RANK, L"Rank cores", ix + 2 * (bw + 8), by, bw, !running, FALSE);
        button(H_STOP, L"Stop", ix + 3 * (bw + 8), by, bw, running, TRUE);
        float hy = by + 34 + 10;
        if (running && view.wanted) {
            float part = (float)view.loadSeconds / view.wanted;
            fillRound(rc(ix, hy + 4, iw, 5), 2.5f, tone(), 0.08f);
            fillRound(rc(ix, hy + 4, iw * (part > 1 ? 1 : part), 5), 2.5f, ACCENT, 1);
        } else if (problem[0]) {
            wsprintfW(s, L"%S", problem);
            text(s, F_TINY, rc(ix, hy, iw, 14), warn(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        } else {
            LPCWSTR hint = running ? L"A crash closes the load by itself. Stop ends it now." : L"Rank repeats a 10 minute run, each time holding out the cores that failed.";
            text(hint, F_TINY, rc(ix, hy, iw, 14), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        y += 126 + SECT;
    }
    // Right: the last result.
    section(L"RESULT", x, &y);
    {
        float ix = x + INSET, iw = RIGHT_W - 2 * INSET;
        card(x, y, RIGHT_W, 124);
        if (running) {
            text(L"Running.", F_ROW, rc(ix, y + 14, iw, 20), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            text(L"The result appears here when the run ends. A core turns red the moment", F_SMALL, rc(ix, y + 44, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            text(L"Windows logs an error for it.", F_SMALL, rc(ix, y + 62, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        } else if (!haveResult) {
            text(L"No run yet.", F_ROW, rc(ix, y + 14, iw, 20), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            text(L"A run passes when the load finishes, Windows logs no hardware error", F_SMALL, rc(ix, y + 44, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            text(L"and no core throttles.", F_SMALL, rc(ix, y + 62, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        } else {
            const Result *r = &lastResult;
            LPCWSTR word = r->pass ? L"PASS" : L"FAIL";
            text(word, F_BOLD, rc(ix, y + 12, 50, 20), r->pass ? good() : ACCENT, 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            wsprintfW(s, L"%d:%02d", r->seconds / 60, r->seconds % 60);
            if (r->wanted) wsprintfW(s + lstrlenW(s), L" of %d:00", r->wanted / 60);
            if (r->score[0]) wsprintfW(s + lstrlenW(s), L"  \x00B7  score %S", r->score);
            if (lastOptions.exclude) {
                char list[256];
                coreList(lastOptions.exclude, list);
                wsprintfW(s + lstrlenW(s), L"  \x00B7  without %S", list);
            }
            text(s, F_ROW, rc(ix + 46, y + 12, iw - 46, 20), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            float ly = y + 38;
            if (!r->pass) {
                wsprintfW(s, L"%S", r->reason);
                if (s[0] >= L'a' && s[0] <= L'z') s[0] -= 32;
                text(s, F_SMALL, rc(ix, ly, iw, 18), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
                ly += 19;
            }
            s[0] = 0;
            if (r->coolantStart >= 0)
                wsprintfW(s, L"Water %d.%d \x2192 %d.%d \x00B0" L"C", (int)r->coolantStart, (int)(r->coolantStart * 10 + 0.5) % 10, (int)r->coolantMax, (int)(r->coolantMax * 10 + 0.5) % 10);
            if (r->pkgTempMax >= 0) wsprintfW(s + lstrlenW(s), L"%sPackage %d W, %d \x00B0" L"C at most", s[0] ? L"  \x00B7  " : L"", r->loaded ? (int)(r->wattSum / r->loaded) : 0, r->pkgTempMax);
            if (s[0]) text(s, F_SMALL, rc(ix, ly, iw, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING), ly += 19;
            if (rankText[0]) {
                // One line per ranking run, newest last.
                char copy[1024], *row, *ctx = NULL;
                int lines = 0, skip;
                lstrcpynA(copy, rankText, sizeof(copy));
                for (char *c = copy; *c; c++) lines += *c == '\n';
                skip = lines > 3 ? lines - 3 : 0;
                for (row = strtok_s(copy, "\n", &ctx); row && ly < y + 108; row = strtok_s(NULL, "\n", &ctx)) {
                    if (skip-- > 0) continue;
                    wsprintfW(s, L"%S", row);
                    text(s, F_SMALL, rc(ix, ly, iw, 18), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
                    ly += 18;
                }
            } else if (r->failed) {
                // Cores in the order they first failed.
                int order[MAX_CPU], n = 0;
                for (int i = 0; i < coreCount; i++)
                    if (r->core[i].errors) order[n++] = i;
                for (int i = 1; i < n; i++)
                    for (int k = i; k > 0 && r->core[order[k]].firstError < r->core[order[k - 1]].firstError; k--) {
                        int swap = order[k];
                        order[k] = order[k - 1], order[k - 1] = swap;
                    }
                lstrcpyW(s, L"First errors: ");
                for (int i = 0; i < n && lstrlenW(s) < 200; i++)
                    wsprintfW(s + lstrlenW(s), L"%s%S at %d:%02d", i ? L", " : L"", cores[order[i]].name, r->core[order[i]].firstError / 60, r->core[order[i]].firstError % 60);
                text(s, F_SMALL, rc(ix, ly, iw, 18), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }
        y += 124 + SECT;
    }
    // Right: earlier runs, newest first.
    section(L"HISTORY", x, &y);
    if (!historyCount) text(L"Runs are kept in ledger.csv beside the program.", F_SMALL, rc(x + 2, y + 2, RIGHT_W, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
    for (int i = 0; i < historyCount; i++) {
        BOOL pass = !strcmp(history[i].result, "PASS");
        wsprintfW(s, L"%S", history[i].when);
        text(s, F_SMALL, rc(x + 2, y + 2, 86, 18), dim(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        text(pass ? L"PASS" : L"FAIL", F_SMALL, rc(x + 90, y + 2, 34, 18), pass ? good() : ACCENT, 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        wsprintfW(s, L"%S", history[i].text);
        text(s, F_SMALL, rc(x + 128, y + 2, RIGHT_W - 128, 18), ink(), 1, DWRITE_TEXT_ALIGNMENT_LEADING);
        y += 24;
    }
}

static BOOL render(void) {
    D2D1_RENDER_TARGET_PROPERTIES p = {D2D1_RENDER_TARGET_TYPE_SOFTWARE, {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE}, 96 * scale, 96 * scale, D2D1_RENDER_TARGET_USAGE_NONE,
                                       D2D1_FEATURE_LEVEL_DEFAULT};
    D2D1_COLOR_F white = {1, 1, 1, 1};
    int w = (int)(WIN_W * scale + 0.5f), h = (int)(WIN_H * scale + 0.5f);
    RECT r = {0, 0, w, h};
    if (!surfDc || w != surfW || h != surfH) {
        BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, (DWORD)(w * h * 4), 0, 0, 0, 0}, {{0}}};
        if (surfDc) DeleteDC(surfDc), DeleteObject(surfBmp);
        surfDc = CreateCompatibleDC(NULL);
        surfBmp = CreateDIBSection(surfDc, &bi, DIB_RGB_COLORS, &surfBits, NULL, 0);
        if (!surfBmp) return FALSE;
        SelectObject(surfDc, surfBmp);
        surfW = w, surfH = h;
        if (dcrt) ID2D1SolidColorBrush_Release(brush), ID2D1DCRenderTarget_Release(dcrt), dcrt = NULL;
    }
    if (!dcrt) {
        if (ID2D1Factory_CreateDCRenderTarget(d2f, &p, &dcrt) != S_OK) return FALSE;
        rt = (ID2D1RenderTarget *)dcrt;
        ID2D1RenderTarget_SetTextAntialiasMode(rt, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (ID2D1RenderTarget_CreateSolidColorBrush(rt, &white, NULL, &brush) != S_OK) return FALSE;
    }
    ID2D1DCRenderTarget_BindDC(dcrt, surfDc, &r);
    ID2D1RenderTarget_BeginDraw(rt);
    paintAll();
    return ID2D1RenderTarget_EndDraw(rt, NULL, NULL) == S_OK;
}

static void redraw(void) {
    if (!window || !render()) return;
    HDC dc = GetDC(window);
    GdiFlush();
    BitBlt(dc, 0, 0, surfW, surfH, surfDc, 0, 0, SRCCOPY);
    ReleaseDC(window, dc);
}

// ---- History (ledger.csv) ----

static int csvFields(char *line, char **field, int max) {
    int n = 0;
    for (char *c = line; n < max;) {
        if (*c == '"') {
            field[n++] = ++c;
            while (*c && *c != '"') c++;
            if (*c) *c++ = 0;
            if (*c == ',') c++;
        } else {
            field[n++] = c;
            while (*c && *c != ',') c++;
            if (!*c) break;
            *c++ = 0;
        }
        if (!*c) break;
    }
    return n;
}

static void historyLoad(void) {
    static const char *const month[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    WCHAR path[MAX_PATH];
    static char buf[16384];
    char *lines[64], *field[16];
    int count = 0;
    historyCount = 0;
    swprintf(path, MAX_PATH, L"%lsledger.csv", exeDir);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f), from = size > (long)sizeof(buf) - 1 ? size - ((long)sizeof(buf) - 1) : 0;
    fseek(f, from, SEEK_SET);
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = 0;
    for (char *row = strtok(buf, "\r\n"); row; row = strtok(NULL, "\r\n")) {
        if (count == 64) memmove(lines, lines + 1, 63 * sizeof(*lines)), count--;
        lines[count++] = row;
    }
    for (int i = count - 1; i >= 0 && historyCount < 3; i--) {
        // run,note,minutes,excluded,result,reason,seconds,score,errors,failed_cores,...
        int n = csvFields(lines[i], field, 16), m, d, hh, mm;
        if (n < 10 || (strcmp(field[4], "PASS") && strcmp(field[4], "FAIL"))) continue;
        if (sscanf(field[0], "%*4d%2d%2d-%2d%2d", &m, &d, &hh, &mm) != 4 || m < 1 || m > 12) continue;
        snprintf(history[historyCount].when, sizeof(history[0].when), "%d %s %02d:%02d", d, month[m - 1], hh, mm);
        snprintf(history[historyCount].result, sizeof(history[0].result), "%s", field[4]);
        const char *label = strlen(field[0]) > 16 ? field[0] + 16 : "", *what = field[1];
        if (!what[0]) what = !strcmp(label, "pass") ? "Single pass" : !strcmp(label, "10min") ? "10 minutes" : !strncmp(label, "rank", 4) ? "Ranking run" : label;
        if (strcmp(field[9], "-")) snprintf(history[historyCount].text, sizeof(history[0].text), "%.60s  \xB7  %.40s failed", what, field[9]);
        else snprintf(history[historyCount].text, sizeof(history[0].text), "%.100s", what);
        historyCount++;
    }
}

// ---- Worker ----

static void publish(const Live *live) {
    EnterCriticalSection(&lock);
    sharedLive = *live;
    LeaveCriticalSection(&lock);
    PostMessageW(window, WM_LIVE, 0, 0);
}

static void idleSample(int *util) {
    static Live live;
    Snapshot s;
    utilRead(util);
    sensorsRead(&s);
    memset(&live, 0, sizeof(live));
    live.phase = PH_IDLE, live.pkgWatts = s.pkgWatts, live.pkgTemp = s.pkgTemp, live.tjmax = s.tjmax, live.coolant = coolantRead();
    for (int i = 0; i < coreCount; i++) {
        LiveCore *c = &live.core[i];
        int threads = 0;
        c->temp = -1, c->firstError = -1;
        for (int cpu = 0; cpu < cpuCount; cpu++) {
            if (!(cores[i].mask >> cpu & 1)) continue;
            if (s.cpu[cpu].mhz > c->mhz) c->mhz = s.cpu[cpu].mhz;
            if (s.cpu[cpu].temp > c->temp) c->temp = s.cpu[cpu].temp;
            c->util += util[cpu], threads++;
        }
        c->util = threads ? c->util / threads : 0;
    }
    publish(&live);
}

static DWORD WINAPI worker(LPVOID unused) {
    int util[MAX_CPU] = {0};
    sensorsOpen();
    utilRead(util);
    while (!quitting) {
        int kind = InterlockedExchange(&pendingKind, K_NONE);
        if (kind == K_SENSORS) {
            source = SRC_NONE;
            sensorsOpen();
        } else if (kind) {
            Options o = pendingOptions;
            Result r;
            char name[96] = "", rank[1024] = "";
            InterlockedExchange(&stopRequested, 0);
            engineError[0] = 0;
            wheaCount = 0;
            if (kind == K_RANK) rankLoop(&o, &r, name, rank, sizeof(rank));
            else if (!runOnce(&o, &r, name)) name[0] = 0;
            EnterCriticalSection(&lock);
            sharedResult = r, sharedOptions = o;
            lstrcpynA(sharedName, name, sizeof(sharedName)), lstrcpynA(sharedRank, rank, sizeof(sharedRank)), lstrcpynA(sharedProblem, engineError, sizeof(sharedProblem));
            LeaveCriticalSection(&lock);
            PostMessageW(window, WM_DONE, 0, 0);
            continue;
        }
        idleSample(util);
        WaitForSingleObject(wake, EVERY_MS);
    }
    return 0;
}

static void start(int kind) {
    Options o = {0};
    if (running) return;
    o.minutes = kind == K_PASS ? 0 : 10;
    o.exclude = heldOut;
    WideCharToMultiByte(CP_ACP, 0, note, -1, o.note, sizeof(o.note), NULL, NULL);
    for (char *c = o.note; *c; c++)
        if (*c == '"' || *c == '\n' || *c == ',') *c = ' ';
    lstrcpyA(o.label, kind == K_PASS ? "pass" : kind == K_TEN ? "10min" : "rank");
    pendingOptions = o;
    running = TRUE, runKind = kind, noteFocus = FALSE, problem[0] = 0, rankText[0] = 0, haveResult = FALSE;
    InterlockedExchange(&pendingKind, kind);
    SetEvent(wake);
    redraw();
}

// ---- Window ----

static void tip(void) {
    WCHAR s[128];
    int hottest = -1;
    for (int i = 0; i < coreCount; i++)
        if (view.core[i].temp >= 0 && (hottest < 0 || view.core[i].temp > view.core[hottest].temp)) hottest = i;
    if (running) wsprintfW(s, L"Shakedown \x00B7 running %d:%02d \x00B7 %d error%s", view.loadSeconds / 60, view.loadSeconds % 60, view.errors, view.errors == 1 ? L"" : L"s");
    else if (haveResult) wsprintfW(s, L"Shakedown \x00B7 last run %s", lastResult.pass ? L"passed" : L"failed");
    else lstrcpyW(s, L"Shakedown \x00B7 ready");
    if (hottest >= 0) wsprintfW(s + lstrlenW(s), L" \x00B7 %S %d \x00B0" L"C", cores[hottest].name, view.core[hottest].temp);
    if (lstrcmpW(s, nid.szTip)) {
        lstrcpynW(nid.szTip, s, 128);
        nid.uFlags = NIF_TIP | NIF_SHOWTIP;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

static int hitAt(LPARAM lp) {
    float x = (short)LOWORD(lp) / scale, y = (short)HIWORD(lp) / scale;
    for (int i = 1; i < H_COUNT; i++)
        if (hit[i].right > hit[i].left && inside(hit[i], x, y)) return i;
    return H_NONE;
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (render()) GdiFlush(), BitBlt(dc, 0, 0, surfW, surfH, surfDc, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LIVE:
        EnterCriticalSection(&lock);
        view = sharedLive;
        LeaveCriticalSection(&lock);
        tip();
        redraw();
        return 0;
    case WM_DONE: {
        EnterCriticalSection(&lock);
        lastResult = sharedResult, lastOptions = sharedOptions;
        lstrcpyA(lastName, sharedName), lstrcpyA(rankText, sharedRank), lstrcpyA(problem, sharedProblem);
        LeaveCriticalSection(&lock);
        running = FALSE;
        haveResult = lastName[0] != 0;
        if (runKind == K_RANK && haveResult) heldOut = lastOptions.exclude; // the cores that had to be held out stay marked
        historyLoad();
        if (haveResult && GetForegroundWindow() != hwnd) {
            char list[256];
            coreList(lastResult.failed, list);
            nid.uFlags = NIF_INFO;
            nid.dwInfoFlags = lastResult.pass ? NIIF_INFO : NIIF_WARNING;
            lstrcpyW(nid.szInfoTitle, lastResult.pass ? L"Shakedown: passed" : L"Shakedown: failed");
            if (lastResult.pass) wsprintfW(nid.szInfo, L"The load ran %d:%02d with no errors.", lastResult.seconds / 60, lastResult.seconds % 60);
            else wsprintfW(nid.szInfo, L"%S%s%S", lastResult.reason, lastResult.failed ? L". Errors on " : L"", lastResult.failed ? list : "");
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        tip();
        redraw();
        return 0;
    }
    case WM_MOUSEMOVE: {
        int h = hitAt(lp);
        if (h != hover) {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            hover = h;
            redraw();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hover) hover = H_NONE, redraw();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, hover == H_NOTE ? (LPCWSTR)IDC_IBEAM : hover ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        int h = hitAt(lp);
        noteFocus = h == H_NOTE;
        if (h == H_PASS) start(K_PASS);
        else if (h == H_TEN) start(K_TEN);
        else if (h == H_RANK) start(K_RANK);
        else if (h == H_STOP) InterlockedExchange(&stopRequested, 1);
        else if (h == H_INSTALL) {
            if (cmdInstall(FALSE) == 0) InterlockedExchange(&pendingKind, K_SENSORS), SetEvent(wake);
        } else if (h >= H_CORE) {
            heldOut ^= (ULONG64)1 << (h - H_CORE);
            // Something has to run the load.
            ULONG64 everyCore = coreCount >= 64 ? ~0ull : ((ULONG64)1 << coreCount) - 1;
            if ((heldOut & everyCore) == everyCore) heldOut = 0;
        }
        redraw();
        return 0;
    }
    case WM_CHAR:
        if (!noteFocus) break;
        if (wp == VK_BACK) {
            int n = lstrlenW(note);
            if (n) note[n - 1] = 0;
        } else if (wp == VK_RETURN || wp == VK_ESCAPE) {
            noteFocus = FALSE;
        } else if (wp >= 32 && wp < 127 && lstrlenW(note) < 100) {
            int n = lstrlenW(note);
            note[n] = (WCHAR)wp, note[n + 1] = 0;
        }
        redraw();
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT || LOWORD(lp) == WM_LBUTTONUP) {
            ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(hwnd);
        }
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        scale = HIWORD(wp) / 96.0f;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_CLOSE:
        if (running && MessageBoxW(hwnd, L"A test is running. Stop it and close?", L"Shakedown", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&stopRequested, 1);
        InterlockedExchange(&quitting, 1);
        SetEvent(wake);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL systemLight(void) {
    DWORD v = 0, n = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &n);
    return v != 0;
}

// ---- Developer mode: draw one state to a .bmp with made-up data, no window, no hardware ----
//   shakedown.exe --shot <idle|running|fail|pass|rank|nohelper> [light] [x2] out=<file.bmp>

static int shot(LPCWSTR cmd) {
    LPCWSTR out = wcsstr(cmd, L"out=");
    BOOL isRunning = wcsstr(cmd, L" running") != NULL, isFail = wcsstr(cmd, L" fail") != NULL, isPass = wcsstr(cmd, L" pass") != NULL, isRank = wcsstr(cmd, L" rank") != NULL;
    BOOL bare = wcsstr(cmd, L" nohelper") != NULL, loaded = isRunning || isRank;
    DWORD wrote;
    if (!out) return 2;
    shotMode = TRUE;
    light = wcsstr(cmd, L" light") != NULL;
    scale = wcsstr(cmd, L" x2") ? 2 : 1;
    coreCount = 24, pCount = 8, cpuCount = 32;
    for (int i = 0; i < 24; i++) cores[i].perf = i < 8, sprintf(cores[i].name, i < 8 ? "P%d" : "E%d", i < 8 ? i : i - 8);
    source = bare ? SRC_COUNTERS : SRC_HELPER;
    view.tjmax = 100, view.coolant = loaded ? 31.4 : 26.6, view.pkgWatts = bare ? 0 : loaded ? 388 : 94, view.pkgTemp = bare ? -1 : loaded ? 91 : 42;
    for (int i = 0; i < 24; i++) {
        LiveCore *c = &view.core[i];
        c->firstError = -1;
        c->mhz = i < 8 ? (loaded ? 5880 + i % 3 : 5600 + i * 31) : (loaded ? 4480 : 2100 + i * 57);
        c->temp = bare ? -1 : i < 8 ? (loaded ? 84 + (i * 5) % 11 : 33 + i % 6) : (loaded ? 70 + i % 7 : 31 + i % 3);
        c->util = loaded ? 99 : i == 5 ? 12 : 0;
    }
    if (isRunning) {
        running = TRUE, runKind = K_TEN, view.phase = PH_LOAD, view.loadSeconds = 252, view.wanted = 600, view.errors = 3;
        view.core[6].errors = 2, view.core[6].firstError = 225, view.core[3].errors = 1, view.core[3].firstError = 238;
        lstrcpyW(note, L"P2 P4 58x, rest 59x");
    }
    if (isRank) {
        running = TRUE, runKind = K_RANK, view.phase = PH_LOAD, view.loadSeconds = 97, view.wanted = 600, view.round = 2, view.held = 0x14;
        view.core[2].util = view.core[4].util = 1, view.core[2].mhz = view.core[4].mhz = 0, view.core[2].temp = view.core[4].temp = 58;
    }
    if (isFail || isPass) {
        Result *r = &lastResult;
        haveResult = TRUE;
        r->pass = isPass, r->seconds = isPass ? 600 : 318, r->wanted = 600, r->coolantStart = 28.7, r->coolantMax = 31.9, r->pkgTempMax = 96, r->loaded = 100, r->wattSum = 38800;
        lstrcpyW(note, isPass ? L"P0 P1 P7 59x, rest 58x" : L"59x all cores");
        if (isPass) lstrcpyA(r->score, "9871");
        else {
            lstrcpyA(r->reason, "the load crashed after 5:18");
            r->errors = 4, r->failed = 0x68;
            r->core[6].errors = 2, r->core[6].firstError = 225, r->core[3].errors = 1, r->core[3].firstError = 238, r->core[5].errors = 1, r->core[5].firstError = 279;
            lastOptions.exclude = 0x14, heldOut = 0x14;
        }
    }
    static const struct { const char *when, *result, *text; } past[3] = {
        {"11 Nov 20:03", "FAIL", "59x all cores  \xB7  P3 P5 P6 failed"}, {"11 Nov 20:01", "FAIL", "59x all cores  \xB7  P2 P4 failed"}, {"11 Nov 19:57", "FAIL", "59x all cores  \xB7  P2 P4 failed"}};
    for (historyCount = 0; historyCount < 3; historyCount++)
        lstrcpyA(history[historyCount].when, past[historyCount].when), lstrcpyA(history[historyCount].result, past[historyCount].result), lstrcpyA(history[historyCount].text, past[historyCount].text);
    if (!drawLoad() || !render()) return 1;
    BITMAPINFOHEADER info = {sizeof(info), surfW, -surfH, 1, 32, BI_RGB, (DWORD)(surfW * surfH * 4), 0, 0, 0, 0};
    BITMAPFILEHEADER head = {0x4D42, sizeof(head) + sizeof(info) + surfW * surfH * 4, 0, 0, sizeof(head) + sizeof(info)};
    WCHAR path[MAX_PATH];
    lstrcpynW(path, out + 4, MAX_PATH);
    for (WCHAR *c = path; *c; c++)
        if (*c == L' ') *c = 0;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 1;
    GdiFlush();
    WriteFile(f, &head, sizeof(head), &wrote, NULL);
    WriteFile(f, &info, sizeof(info), &wrote, NULL);
    WriteFile(f, surfBits, surfW * surfH * 4, &wrote, NULL);
    CloseHandle(f);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show) {
    quiet = TRUE;
    paths();
    if (wcsstr(cmd, L"--helper")) return helperEntry();
    if (!wcscmp(cmd, L"install")) return cmdInstall(FALSE);
    if (!wcscmp(cmd, L"uninstall")) return cmdInstall(TRUE);
    if (wcsstr(cmd, L"--shot")) return shot(cmd);

    HANDLE once = CreateMutexW(NULL, TRUE, L"Local\\ShakedownWindow");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(WIN_CLASS, NULL);
        if (other) ShowWindow(other, SW_RESTORE), SetForegroundWindow(other);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!topology() || !drawLoad()) {
        MessageBoxW(NULL, L"Shakedown could not start: the CPU layout or Direct2D is not available.", L"Shakedown", MB_ICONERROR);
        return 1;
    }
    light = systemLight();
    historyLoad();
    InitializeCriticalSection(&lock);
    wake = CreateEventW(NULL, FALSE, FALSE, NULL);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = proc, wc.hInstance = inst, wc.lpszClassName = WIN_CLASS;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    POINT origin = {0, 0};
    GetCursorPos(&origin);
    UINT dpi = 96, dpiY;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    typedef HRESULT(WINAPI * GetDpi)(HMONITOR, int, UINT *, UINT *);
    GetDpi getDpi = shcore ? (GetDpi)GetProcAddress(shcore, "GetDpiForMonitor") : NULL;
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    if (getDpi) getDpi(monitor, 0, &dpi, &dpiY);
    scale = dpi / 96.0f;
    RECT frame = {0, 0, (int)(WIN_W * scale + 0.5f), (int)(WIN_H * scale + 0.5f)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
    int w = frame.right - frame.left, h = frame.bottom - frame.top;
    window = CreateWindowExW(0, WIN_CLASS, L"Shakedown", style, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h, NULL,
                             NULL, inst, NULL);
    if (!window) return 1;
    BOOL dark = !light;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark)); // dark title bar to match

    nid.cbSize = sizeof(nid), nid.hWnd = window, nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY, nid.uVersion = NOTIFYICON_VERSION_4;
    int px = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    nid.hIcon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, 0);
    lstrcpyW(nid.szTip, L"Shakedown");
    Shell_NotifyIconW(NIM_ADD, &nid);
    Shell_NotifyIconW(NIM_SETVERSION, &nid);

    workerThread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    ShowWindow(window, show);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // The worker ends its load (the job is closed with it) before the process goes.
    WaitForSingleObject(workerThread, 15000);
    if (aliveEvent) CloseHandle(aliveEvent);
    CloseHandle(once);
    return 0;
}
