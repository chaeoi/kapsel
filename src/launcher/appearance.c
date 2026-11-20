#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <wchar.h>
#include "appearance.h"
#include "resource.h"

static const COLORREF paper = RGB(246, 247, 251);
static const COLORREF rail = RGB(24, 34, 52);
static const COLORREF ink = RGB(23, 32, 53);
static const COLORREF muted = RGB(80, 95, 119);
static const COLORREF line = RGB(207, 215, 230);
static const COLORREF accent = RGB(60, 76, 188);
static const COLORREF rail_text = RGB(191, 205, 230);
static const COLORREF white = RGB(255, 255, 255);
static HBRUSH paper_brush, rail_brush;
static HFONT brand_font, capacity_font, heading_font;
static BOOL high_contrast;

static RECT dialog_rect(HWND dialog, LONG left, LONG top, LONG right, LONG bottom)
{
    RECT rect = { left, top, right, bottom };
    MapDialogRect(dialog, &rect);
    return rect;
}

static HFONT sized_font(HWND dialog, int height, int weight, const wchar_t *face)
{
    const RECT scale = dialog_rect(dialog, 0, 0, 0, height);
    return CreateFontW(-scale.bottom, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, face);
}

static LRESULT CALLBACK hot_button(HWND button, UINT message, WPARAM param, LPARAM data,
                                   UINT_PTR id, DWORD_PTR unused)
{
    (void)unused;
    if (message == WM_MOUSEMOVE && !GetPropW(button, L"Kapsel.Hot")) {
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, button, 0 };
        if (TrackMouseEvent(&track)) {
            SetPropW(button, L"Kapsel.Hot", (HANDLE)1);
            InvalidateRect(button, NULL, FALSE);
        }
    }
    if (message == WM_MOUSELEAVE || message == WM_ENABLE || message == WM_NCDESTROY) {
        RemovePropW(button, L"Kapsel.Hot");
        InvalidateRect(button, NULL, FALSE);
        if (message == WM_NCDESTROY) RemoveWindowSubclass(button, hot_button, id);
    }
    return DefSubclassProc(button, message, param, data);
}

void init_appearance(HWND dialog)
{
    HIGHCONTRASTW contrast = { .cbSize = sizeof(contrast) };
    high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                    (contrast.dwFlags & HCF_HIGHCONTRASTON);
    paper_brush = CreateSolidBrush(paper);
    rail_brush = CreateSolidBrush(rail);
    brand_font = sized_font(dialog, 18, FW_SEMIBOLD, L"Segoe UI");
    capacity_font = sized_font(dialog, 15, FW_SEMIBOLD, L"Segoe UI");
    heading_font = sized_font(dialog, 13, FW_BOLD, L"Microsoft YaHei UI");

    SendDlgItemMessageW(dialog, IDC_BRAND, WM_SETFONT, (WPARAM)brand_font, FALSE);
    SendDlgItemMessageW(dialog, IDC_DISK_VALUE, WM_SETFONT, (WPARAM)capacity_font, FALSE);
    SendDlgItemMessageW(dialog, IDC_BOOT_HEADING, WM_SETFONT, (WPARAM)heading_font, FALSE);
    SendDlgItemMessageW(dialog, IDC_HARDWARE_HEADING, WM_SETFONT, (WPARAM)heading_font, FALSE);
    const int buttons[] = { IDC_BROWSE, IDC_BOOT_DISK, IDC_BOOT_ISO };
    for (size_t i = 0; i < ARRAYSIZE(buttons); ++i)
        SetWindowSubclass(GetDlgItem(dialog, buttons[i]), hot_button, 1, 0);
}

void release_appearance(void)
{
    DeleteObject(paper_brush);
    DeleteObject(rail_brush);
    DeleteObject(brand_font);
    DeleteObject(capacity_font);
    DeleteObject(heading_font);
}

static void draw_box(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF edge)
{
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, edge);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
}

BOOL paint_dialog_background(HWND dialog, HDC dc)
{
    RECT client;
    GetClientRect(dialog, &client);
    FillRect(dc, &client, high_contrast ? GetSysColorBrush(COLOR_WINDOW) : paper_brush);

    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ previous_pen = SelectObject(dc, GetStockObject(DC_PEN));
    const int radius = dialog_rect(dialog, 0, 0, 6, 0).right;
    if (!high_contrast) {
        RECT side = dialog_rect(dialog, 0, 0, 116, 0);
        side.bottom = client.bottom;
        FillRect(dc, &side, rail_brush);
        RECT mark = dialog_rect(dialog, 19, 19, 48, 46);
        draw_box(dc, mark, radius, accent, accent);
        RECT stem = dialog_rect(dialog, 27, 25, 29, 40);
        SetDCBrushColor(dc, white);
        FillRect(dc, &stem, GetStockObject(DC_BRUSH));
        const POINT arms[] = {
            {29, 33}, {37, 25}, {40, 25}, {33, 33}, {41, 40}, {37, 40},
        };
        POINT scaled[ARRAYSIZE(arms)];
        for (size_t i = 0; i < ARRAYSIZE(arms); ++i) {
            RECT point = dialog_rect(dialog, arms[i].x, arms[i].y, 0, 0);
            scaled[i] = (POINT){point.left, point.top};
        }
        SetDCPenColor(dc, white);
        Polygon(dc, scaled, ARRAYSIZE(scaled));

        RECT divider = dialog_rect(dialog, 19, 96, 97, 97);
        SetDCBrushColor(dc, RGB(57, 72, 99));
        FillRect(dc, &divider, GetStockObject(DC_BRUSH));
        divider = dialog_rect(dialog, 136, 206, 402, 207);
        SetDCBrushColor(dc, line);
        FillRect(dc, &divider, GetStockObject(DC_BRUSH));
    }

    static const struct { int id; RECT rect; } fields[] = {
        { IDC_ISO, {136, 36, 316, 62} },
        { IDC_CPU, {136, 124, 219, 151} },
        { IDC_MEMORY, {227, 124, 310, 151} },
        { IDC_DISK_SIZE, {318, 124, 402, 151} },
    };
    for (size_t i = 0; i < ARRAYSIZE(fields); ++i) {
        RECT bounds = fields[i].rect;
        MapDialogRect(dialog, &bounds);
        COLORREF edge = GetFocus() == GetDlgItem(dialog, fields[i].id) ? accent : line;
        draw_box(dc, bounds, radius, high_contrast ? GetSysColor(COLOR_WINDOW) : white,
                 high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : edge);
    }
    SelectObject(dc, previous_pen);
    SelectObject(dc, previous_brush);
    return TRUE;
}

BOOL paint_dialog_button(HWND dialog, const DRAWITEMSTRUCT *item)
{
    if (item->CtlType != ODT_BUTTON) return FALSE;

    int primary_id = IDC_BROWSE;
    if (IsWindowEnabled(GetDlgItem(dialog, IDC_BOOT_ISO))) primary_id = IDC_BOOT_ISO;
    else if (IsWindowEnabled(GetDlgItem(dialog, IDC_BOOT_DISK))) primary_id = IDC_BOOT_DISK;
    const BOOL primary = item->CtlID == (UINT)primary_id;
    const BOOL disabled = item->itemState & ODS_DISABLED;
    const BOOL pressed = item->itemState & ODS_SELECTED;
    const BOOL hovered = GetPropW(item->hwndItem, L"Kapsel.Hot") != NULL;

    COLORREF fill = white;
    COLORREF edge = line;
    COLORREF text = ink;
    if (high_contrast) {
        fill = GetSysColor(primary ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
        edge = GetSysColor(COLOR_WINDOWTEXT);
        text = GetSysColor(disabled ? COLOR_GRAYTEXT : primary ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT);
    } else if (primary) {
        fill = disabled ? RGB(225, 229, 249) : pressed ? RGB(41, 53, 152) :
               hovered ? RGB(48, 62, 173) : accent;
        edge = fill;
        text = disabled ? RGB(78, 91, 144) : white;
    } else if (disabled) {
        fill = paper;
        text = muted;
    } else if (hovered || pressed) {
        fill = pressed ? RGB(229, 234, 249) : RGB(239, 242, 251);
        edge = RGB(164, 174, 216);
    }

    HDC dc = item->hDC;
    FillRect(dc, &item->rcItem, high_contrast ? GetSysColorBrush(COLOR_WINDOW) : paper_brush);
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ previous_pen = SelectObject(dc, GetStockObject(DC_PEN));
    const int radius = dialog_rect(dialog, 0, 0, 6, 0).right;
    draw_box(dc, item->rcItem, radius, fill, edge);
    if (item->itemState & ODS_FOCUS) {
        RECT focus = item->rcItem;
        InflateRect(&focus, -3, -3);
        draw_box(dc, focus, radius, fill, high_contrast || primary ? text : accent);
    }
    SelectObject(dc, previous_pen);
    SelectObject(dc, previous_brush);

    wchar_t label[80];
    GetWindowTextW(item->hwndItem, label, ARRAYSIZE(label));
    RECT bounds = item->rcItem;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    HGDIOBJ previous_font = SelectObject(dc, (HFONT)SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
    UINT flags = DT_SINGLELINE | DT_CENTER | DT_VCENTER;
    if (item->itemState & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
    DrawTextW(dc, label, -1, &bounds, flags);
    SelectObject(dc, previous_font);
    return TRUE;
}

INT_PTR color_dialog_control(UINT message, HDC dc, HWND control)
{
    if (high_contrast) return FALSE;

    int id = GetDlgCtrlID(control);
    COLORREF background = paper;
    COLORREF text = ink;
    HBRUSH brush = paper_brush;
    if (id >= IDC_BRAND && id <= IDC_DISK_FILE) {
        background = rail;
        brush = rail_brush;
        text = id == IDC_BRAND || id == IDC_DISK_VALUE ? white : rail_text;
    } else if (message == WM_CTLCOLOREDIT || id == IDC_ISO || id == IDC_DISK_SIZE ||
               id == IDC_MEMORY_UNIT || id == IDC_DISK_UNIT) {
        background = white;
        brush = GetStockObject(WHITE_BRUSH);
        if (id == IDC_MEMORY_UNIT || id == IDC_DISK_UNIT) text = muted;
    } else if (id == IDC_STATUS) {
        text = muted;
    }
    SetBkColor(dc, background);
    SetTextColor(dc, text);
    return (INT_PTR)brush;
}
