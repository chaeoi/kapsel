#include "terminal_view.h"
#include "virtualization/vm_session.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace {
constexpr int columns = 80;
constexpr int rows = 24;
constexpr size_t history_limit = 1000;
constexpr COLORREF background = RGB(13, 27, 34);
constexpr COLORREF foreground = RGB(218, 235, 234);

void append_utf16(std::wstring& string, uint32_t codepoint)
{
    if (codepoint <= 0xFFFF) {
        string.push_back(static_cast<wchar_t>(codepoint));
    } else if (codepoint <= 0x10FFFF) {
        const auto offset = codepoint - 0x10000;
        string.push_back(static_cast<wchar_t>(0xD800 + (offset >> 10)));
        string.push_back(static_cast<wchar_t>(0xDC00 + (offset & 0x3FF)));
    }
}
}

TerminalView::TerminalView(HWND parent, VmSession& session, unsigned dpi) : session_(session)
{
    WNDCLASSW cls{};
    cls.lpfnWndProc = window_proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    cls.lpszClassName = L"Kapsel.Terminal";
    RegisterClassW(&cls);
    window_ = CreateWindowExW(0, cls.lpszClassName, L"Debian terminal",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, parent, nullptr, cls.hInstance, this);
    set_dpi(dpi);
    reset();
}

TerminalView::~TerminalView()
{
    if (terminal_) vterm_free(terminal_);
    if (font_) DeleteObject(font_);
}

SIZE TerminalView::minimum_size() const
{
    return {columns * cell_width_ + 2 * padding_, rows * cell_height_ + 2 * padding_};
}

void TerminalView::reset()
{
    if (terminal_) vterm_free(terminal_);
    terminal_ = vterm_new(rows, columns);
    if (!terminal_) {
        screen_ = nullptr;
        return;
    }
    vterm_set_utf8(terminal_, 1);
    vterm_output_set_callback(terminal_, output, this);
    screen_ = vterm_obtain_screen(terminal_);
    VTermColor fg{}, bg{};
    vterm_color_rgb(&fg, GetRValue(foreground), GetGValue(foreground), GetBValue(foreground));
    vterm_color_rgb(&bg, GetRValue(background), GetGValue(background), GetBValue(background));
    vterm_state_set_default_colors(vterm_obtain_state(terminal_), &fg, &bg);

    static const VTermScreenCallbacks callbacks = [] {
        VTermScreenCallbacks result{};
        result.damage = damage;
        result.movecursor = move_cursor;
        result.sb_pushline = scroll_line;
        result.sb_popline = pop_line;
        return result;
    }();
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_ROW);
    history_.clear();
    cursor_ = {};
    scroll_ = 0;
    surrogate_ = 0;
    vterm_screen_reset(screen_, 1);
    InvalidateRect(window_, nullptr, FALSE);
}

void TerminalView::append(const char* bytes, size_t size)
{
    if (!terminal_) return;
    vterm_input_write(terminal_, bytes, size);
    vterm_screen_flush_damage(screen_);
}

void TerminalView::set_dpi(unsigned dpi)
{
    if (font_) DeleteObject(font_);
    font_ = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    padding_ = MulDiv(22, dpi, 96);
    HDC dc = GetDC(window_);
    HGDIOBJ previous = SelectObject(dc, font_);
    SIZE glyph{};
    TEXTMETRICW metrics{};
    GetTextExtentPoint32W(dc, L"M", 1, &glyph);
    GetTextMetricsW(dc, &metrics);
    cell_width_ = std::max(1L, glyph.cx);
    cell_height_ = std::max(1L, metrics.tmHeight) + MulDiv(3, dpi, 96);
    SelectObject(dc, previous);
    ReleaseDC(window_, dc);
    InvalidateRect(window_, nullptr, FALSE);
}

int TerminalView::damage(VTermRect area, void* context)
{
    auto& view = *static_cast<TerminalView*>(context);
    if (view.scroll_ != 0) {
        InvalidateRect(view.window_, nullptr, FALSE);
    } else {
        RECT rect{view.padding_ + area.start_col * view.cell_width_,
                  view.padding_ + area.start_row * view.cell_height_,
                  view.padding_ + area.end_col * view.cell_width_,
                  view.padding_ + area.end_row * view.cell_height_};
        InvalidateRect(view.window_, &rect, FALSE);
    }
    return 1;
}

void TerminalView::invalidate_cell(VTermPos position)
{
    damage({position.row, position.row + 1, position.col, position.col + 1}, this);
}

int TerminalView::move_cursor(VTermPos position, VTermPos old, int visible, void* context)
{
    auto& view = *static_cast<TerminalView*>(context);
    view.cursor_ = position;
    view.cursor_visible_ = visible != 0;
    view.invalidate_cell(old);
    view.invalidate_cell(position);
    return 1;
}

int TerminalView::scroll_line(int count, const VTermScreenCell* cells, void* context)
{
    auto& view = *static_cast<TerminalView*>(context);
    if (view.history_.size() == history_limit) view.history_.pop_front();
    view.history_.emplace_back(cells, cells + count);
    if (view.scroll_ > 0)
        view.scroll_ = std::min(view.scroll_ + 1, static_cast<int>(view.history_.size()));
    return 1;
}

int TerminalView::pop_line(int count, VTermScreenCell* cells, void* context)
{
    auto& view = *static_cast<TerminalView*>(context);
    if (view.history_.empty()) return 0;
    for (int column = 0; column < count; ++column) {
        const auto& line = view.history_.back();
        cells[column] = column < static_cast<int>(line.size()) ? line[column] : VTermScreenCell{};
    }
    view.history_.pop_back();
    view.scroll_ = std::min(view.scroll_, static_cast<int>(view.history_.size()));
    return 1;
}

void TerminalView::output(const char* bytes, size_t size, void* context)
{
    static_cast<TerminalView*>(context)->session_.send(bytes, size);
}

VTermScreenCell TerminalView::cell_at(int row, int column) const
{
    VTermScreenCell cell{};
    const int source = row - scroll_;
    if (source < 0) {
        const auto& line = history_.at(static_cast<int>(history_.size()) + source);
        if (column < static_cast<int>(line.size())) cell = line[column];
    } else if (screen_) {
        vterm_screen_get_cell(screen_, {source, column}, &cell);
    }
    return cell;
}

void TerminalView::paint_row(HDC dc, int row, int min_x, int max_x)
{
    if (!screen_) return;
    const int y = padding_ + row * cell_height_;
    const int first = std::max(0, (min_x - padding_) / cell_width_ - 1);
    const int last = std::min(columns, (max_x - padding_ + cell_width_ - 1) / cell_width_);
    for (int column = first; column < last; ++column) {
        const auto cell = cell_at(row, column);
        if (cell.chars[0] == UINT32_MAX ||
            (!cell.chars[0] && !cell.attrs.reverse && VTERM_COLOR_IS_DEFAULT_BG(&cell.bg)))
            continue;
        VTermColor bg = cell.attrs.reverse ? cell.fg : cell.bg;
        VTermColor fg = cell.attrs.reverse ? cell.bg : cell.fg;
        if (VTERM_COLOR_IS_DEFAULT_BG(&bg)) vterm_color_rgb(&bg, 13, 27, 34);
        if (VTERM_COLOR_IS_DEFAULT_FG(&fg)) vterm_color_rgb(&fg, 218, 235, 234);
        vterm_screen_convert_color_to_rgb(screen_, &bg);
        vterm_screen_convert_color_to_rgb(screen_, &fg);
        const int x = padding_ + column * cell_width_;
        RECT bounds{x, y, x + std::max(1, int(cell.width)) * cell_width_, y + cell_height_};
        wchar_t text[VTERM_MAX_CHARS_PER_CELL * 2];
        unsigned count = 0;
        for (uint32_t codepoint : cell.chars) {
            if (!codepoint || codepoint == UINT32_MAX) break;
            if (codepoint <= 0xFFFF) text[count++] = static_cast<wchar_t>(codepoint);
            else if (codepoint <= 0x10FFFF) {
                const auto offset = codepoint - 0x10000;
                text[count++] = static_cast<wchar_t>(0xD800 + (offset >> 10));
                text[count++] = static_cast<wchar_t>(0xDC00 + (offset & 0x3FF));
            }
        }
        SetBkColor(dc, RGB(bg.rgb.red, bg.rgb.green, bg.rgb.blue));
        SetTextColor(dc, RGB(fg.rgb.red, fg.rgb.green, fg.rgb.blue));
        ExtTextOutW(dc, x, y + 1, ETO_OPAQUE | ETO_CLIPPED, &bounds, text, count, nullptr);
    }
    if (scroll_ == 0 && cursor_visible_ && cursor_.row == row && GetFocus() == window_) {
        RECT caret{padding_ + cursor_.col * cell_width_, y + cell_height_ - 2,
                   padding_ + (cursor_.col + 1) * cell_width_, y + cell_height_};
        SetDCBrushColor(dc, RGB(139, 222, 209));
        FillRect(dc, &caret, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
}

void TerminalView::paint()
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    SetDCBrushColor(dc, background);
    FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    HGDIOBJ previous = SelectObject(dc, font_);
    const int first = std::max(0L, (paint.rcPaint.top - padding_) / cell_height_);
    const int last = std::min(static_cast<long>(rows), (paint.rcPaint.bottom - padding_ + cell_height_ - 1) / cell_height_);
    for (int row = first; row < last; ++row)
        paint_row(dc, row, paint.rcPaint.left, paint.rcPaint.right);
    SelectObject(dc, previous);
    EndPaint(window_, &paint);
}

VTermModifier TerminalView::modifiers()
{
    return static_cast<VTermModifier>(
        ((GetKeyState(VK_SHIFT) & 0x8000) ? VTERM_MOD_SHIFT : 0) |
        ((GetKeyState(VK_CONTROL) & 0x8000) ? VTERM_MOD_CTRL : 0) |
        ((GetKeyState(VK_MENU) & 0x8000) ? VTERM_MOD_ALT : 0));
}

bool TerminalView::key_down(WPARAM key)
{
    if (!terminal_) return false;
    const auto mod = modifiers();
    if ((mod & (VTERM_MOD_CTRL | VTERM_MOD_SHIFT)) == (VTERM_MOD_CTRL | VTERM_MOD_SHIFT)) {
        if (key == 'C') { copy(); return true; }
        if (key == 'V') { paste(); return true; }
    }
    VTermKey translated;
    switch (key) {
    case VK_UP: translated = VTERM_KEY_UP; break;
    case VK_DOWN: translated = VTERM_KEY_DOWN; break;
    case VK_LEFT: translated = VTERM_KEY_LEFT; break;
    case VK_RIGHT: translated = VTERM_KEY_RIGHT; break;
    case VK_HOME: translated = VTERM_KEY_HOME; break;
    case VK_END: translated = VTERM_KEY_END; break;
    case VK_PRIOR: translated = VTERM_KEY_PAGEUP; break;
    case VK_NEXT: translated = VTERM_KEY_PAGEDOWN; break;
    case VK_INSERT: translated = VTERM_KEY_INS; break;
    case VK_DELETE: translated = VTERM_KEY_DEL; break;
    case VK_BACK: translated = VTERM_KEY_BACKSPACE; break;
    default:
        if (key < VK_F1 || key > VK_F12) return false;
        translated = static_cast<VTermKey>(VTERM_KEY_FUNCTION(key - VK_F1 + 1));
    }
    vterm_keyboard_key(terminal_, translated, mod);
    return true;
}

void TerminalView::copy()
{
    std::wstring text;
    for (int row = 0; row < rows; ++row) {
        const size_t start = text.size();
        for (int column = 0; column < columns; ++column) {
            const auto cell = cell_at(row, column);
            if (cell.chars[0] == UINT32_MAX) continue;
            if (!cell.chars[0]) text.push_back(L' ');
            else for (uint32_t cp : cell.chars) {
                if (!cp) break;
                append_utf16(text, cp);
            }
        }
        while (text.size() > start && text.back() == L' ') text.pop_back();
        text += L"\r\n";
    }
    if (!OpenClipboard(window_)) return;
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* writable = GlobalLock(memory);
        if (writable) {
            memcpy(writable, text.c_str(), bytes);
            GlobalUnlock(memory);
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, memory)) memory = nullptr;
        }
        if (memory) GlobalFree(memory);
    }
    CloseClipboard();
}

void TerminalView::paste()
{
    if (!terminal_ || !OpenClipboard(window_)) return;
    HANDLE memory = GetClipboardData(CF_UNICODETEXT);
    if (memory) {
        const auto* text = static_cast<const wchar_t*>(GlobalLock(memory));
        if (text) {
            const auto length = std::min<size_t>(GlobalSize(memory) / sizeof(wchar_t), 16384);
            vterm_keyboard_start_paste(terminal_);
            for (size_t index = 0; index < length && text[index]; ++index) {
                uint32_t cp = text[index];
                if (cp >= 0xD800 && cp <= 0xDBFF && index + 1 < length &&
                    text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (text[++index] - 0xDC00);
                }
                if (cp == L'\r' && index + 1 < length && text[index + 1] == L'\n') ++index;
                if (cp == L'\n') cp = L'\r';
                if (cp >= L' ' || cp == L'\r' || cp == L'\t')
                    vterm_keyboard_unichar(terminal_, cp, VTERM_MOD_NONE);
            }
            vterm_keyboard_end_paste(terminal_);
            GlobalUnlock(memory);
        }
    }
    CloseClipboard();
}

LRESULT CALLBACK TerminalView::window_proc(HWND window, UINT message, WPARAM param, LPARAM data)
{
    if (message == WM_NCCREATE) {
        const auto* info = reinterpret_cast<CREATESTRUCTW*>(data);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(info->lpCreateParams));
        return TRUE;
    }
    auto* view = reinterpret_cast<TerminalView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!view) return DefWindowProcW(window, message, param, data);
    switch (message) {
    case WM_PAINT: view->paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: case WM_KILLFOCUS: view->invalidate_cell(view->cursor_); return 0;
    case WM_LBUTTONDOWN: SetFocus(window); return 0;
    case WM_MOUSEWHEEL:
        view->scroll_ = std::clamp(view->scroll_ + GET_WHEEL_DELTA_WPARAM(param) / WHEEL_DELTA * 3,
                                   0, static_cast<int>(view->history_.size()));
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (view->key_down(param)) return 0;
        break;
    case WM_CHAR:
        if (view->terminal_) {
            const auto code = static_cast<unsigned>(param);
            const auto mod = modifiers();
            if ((mod & VTERM_MOD_SHIFT) && (mod & VTERM_MOD_CTRL) && (code == 3 || code == 22))
                return 0;
            if (code == L'\b') return 0;
            if (code >= 0xD800 && code <= 0xDBFF) {
                view->surrogate_ = static_cast<unsigned short>(code);
                return 0;
            }
            uint32_t cp = code;
            if (code >= 0xDC00 && code <= 0xDFFF && view->surrogate_)
                cp = 0x10000 + ((view->surrogate_ - 0xD800) << 10) + (code - 0xDC00);
            view->surrogate_ = 0;
            vterm_keyboard_unichar(view->terminal_, cp, VTERM_MOD_NONE);
        }
        return 0;
    }
    return DefWindowProcW(window, message, param, data);
}
