#include "app_window.h"

#include "../../packaging/windows/resources.h"
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <utility>

namespace {
constexpr wchar_t window_class[] = L"Kapsel.Main";
constexpr COLORREF window_surface = RGB(235, 245, 244);
constexpr COLORREF soft_line = RGB(191, 215, 212);
constexpr COLORREF ink = RGB(22, 55, 60);
constexpr COLORREF muted = RGB(63, 97, 100);
constexpr COLORREF teal = RGB(139, 222, 209);
enum : UINT {
    action_iso = 201, action_menu, action_builtin, action_disk, action_settings,
    action_shutdown, action_force, action_copy, action_paste, action_log,
};
constexpr UINT start_on_open = WM_APP + 2;
constexpr UINT start_pending = WM_APP + 3;

void fill(HDC dc, const RECT& area, COLORREF color)
{
    SetDCBrushColor(dc, color);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

HFONT make_font(unsigned dpi, int points, int weight, const wchar_t* face)
{
    return CreateFontW(-MulDiv(points, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

void draw_label(HDC dc, const wchar_t* text, RECT bounds, HFONT font, COLORREF color,
                UINT align = DT_LEFT)
{
    HGDIOBJ previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &bounds, align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, previous);
}
}

AppWindow::AppWindow(std::wstring directory) : directory_(std::move(directory)),
    settings_(read_settings(directory_)) {}

bool AppWindow::create(HINSTANCE instance, int show)
{
    WNDCLASSEXW window{};
    window.cbSize = sizeof(window);
    window.lpfnWndProc = dispatch;
    window.hInstance = instance;
    window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window.lpszClassName = window_class;
    window.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_KAPSEL));
    window.hIconSm = window.hIcon;
    if (!RegisterClassExW(&window)) return false;
    dpi_ = GetDpiForSystem();
    if (!dpi_) dpi_ = 96;
    const int width = MulDiv(856, dpi_, 96);
    const int height = MulDiv(690, dpi_, 96);
    if (!CreateWindowExW(0, window_class, L"Kapsel", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                         CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                         nullptr, nullptr, instance, this))
        return false;
    ShowWindow(window_, show);
    UpdateWindow(window_);
    PostMessageW(window_, start_on_open, 0, 0);
    return true;
}

LRESULT CALLBACK AppWindow::dispatch(HWND window, UINT message, WPARAM param, LPARAM data)
{
    if (message == WM_NCCREATE) {
        auto* app = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return app ? app->handle(message, param, data) : DefWindowProcW(window, message, param, data);
}

void AppWindow::set_dpi(unsigned dpi)
{
    dpi_ = dpi ? dpi : 96;
    if (label_font_) DeleteObject(label_font_);
    if (title_font_) DeleteObject(title_font_);
    label_font_ = make_font(dpi_, 9, FW_NORMAL, L"Microsoft YaHei UI");
    title_font_ = make_font(dpi_, 15, FW_SEMIBOLD, L"Segoe UI");
    if (terminal_) terminal_->set_dpi(dpi_);
}

void AppWindow::layout()
{
    RECT bounds{};
    GetClientRect(window_, &bounds);
    const auto px = [this](int value) { return MulDiv(value, dpi_, 96); };
    if (iso_button_) {
        MoveWindow(iso_button_, std::max(px(125), int(bounds.right) - px(182)), px(12), px(122), px(33), TRUE);
        MoveWindow(menu_button_, std::max(px(254), int(bounds.right) - px(52)), px(12), px(32), px(33), TRUE);
    }
    if (terminal_)
        MoveWindow(terminal_->window(), 0, px(58), bounds.right,
                   std::max(0, int(bounds.bottom) - px(87)), TRUE);
}

void AppWindow::paint()
{
    PAINTSTRUCT paint_info{};
    HDC dc = BeginPaint(window_, &paint_info);
    RECT bounds{};
    GetClientRect(window_, &bounds);
    const auto px = [this](int value) { return MulDiv(value, dpi_, 96); };
    const int footer = bounds.bottom - px(29);
    fill(dc, paint_info.rcPaint, window_surface);
    fill(dc, {0, px(57), bounds.right, px(58)}, soft_line);
    fill(dc, {0, footer, bounds.right, footer + 1}, soft_line);
    RECT mark{px(20), px(17), px(44), px(41)};
    fill(dc, mark, teal);
    draw_label(dc, L"K", mark, title_font_, ink, DT_CENTER);
    draw_label(dc, L"Kapsel", {px(56), px(3), px(159), px(55)}, title_font_, ink);
    if (bounds.right >= px(490)) {
        const wchar_t* name = running_ == VmMode::Builtin ? L"Debian 13" : L"虚拟机";
        draw_label(dc, name, {px(157), px(8), bounds.right - px(189), px(51)}, label_font_, muted);
    }
    const int right = bounds.right - px(18);
    draw_label(dc, status_.c_str(), {px(19), footer, right, bounds.bottom}, label_font_, ink);
    EndPaint(window_, &paint_info);
}

bool AppWindow::draw_button(const DRAWITEMSTRUCT& button)
{
    if (button.CtlType != ODT_BUTTON) return false;
    const HDC dc = button.hDC;
    const bool primary = button.CtlID == action_iso;
    const bool pressed = (button.itemState & ODS_SELECTED) != 0;
    RECT area = button.rcItem;
    fill(dc, area, window_surface);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, primary ? (pressed ? RGB(101, 192, 177) : teal) : window_surface);
    SetDCPenColor(dc, primary ? RGB(101, 192, 177) : soft_line);
    const int radius = MulDiv(10, dpi_, 96);
    RoundRect(dc, area.left, area.top, area.right, area.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    if (primary) {
        wchar_t label[32]{};
        GetWindowTextW(button.hwndItem, label, 32);
        draw_label(dc, label, area, label_font_, ink, DT_CENTER);
    } else {
        const int center_x = (area.left + area.right) / 2;
        const int center_y = (area.top + area.bottom) / 2;
        const int spacing = MulDiv(5, dpi_, 96);
        const int radius = MulDiv(1, dpi_, 96);
        for (int offset : {-spacing, 0, spacing})
            fill(dc, {center_x + offset - radius, center_y - radius,
                      center_x + offset + radius, center_y + radius}, ink);
    }
    if (button.itemState & ODS_FOCUS) {
        InflateRect(&area, -3, -3);
        DrawFocusRect(dc, &area);
    }
    return true;
}

void AppWindow::update_status(const wchar_t* text)
{
    status_ = text;
    RECT footer{};
    GetClientRect(window_, &footer);
    footer.top = footer.bottom - MulDiv(29, dpi_, 96);
    InvalidateRect(window_, &footer, FALSE);
}

void AppWindow::choose_iso()
{
    wchar_t path[4096]{};
    OPENFILENAMEW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = window_;
    chooser.lpstrFilter = L"ISO 镜像 (*.iso)\0*.iso\0\0";
    chooser.lpstrFile = path;
    chooser.nMaxFile = 4096;
    chooser.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&chooser)) return;
    VmOptions options = settings_.external;
    options.iso = path;
    launch(std::move(options));
}

void AppWindow::launch(VmOptions options)
{
    if (!session_->active()) {
        start(std::move(options));
        return;
    }
    if (MessageBoxW(window_, L"请保存当前虚拟机里的工作，再关机并切换系统。",
                    L"切换系统", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    pending_ = std::move(options);
    session_->shutdown(false);
    update_status(L"正在关闭当前系统…");
}

void AppWindow::start(VmOptions options)
{
    options.whpx = settings_.whpx;
    running_ = options.mode;
    graphics_visible_ = false;
    terminal_->reset();
    InvalidateRect(window_, nullptr, FALSE);
    update_status(L"正在启动…");
    if (!session_->start(std::move(options)))
        update_status(L"启动失败");
    SetFocus(terminal_->window());
}

void AppWindow::session_message(WPARAM param, LPARAM data)
{
    if (!param) {
        const std::string output = session_->receive_output();
        terminal_->append(output.data(), output.size());
        return;
    }
    switch (static_cast<VmState>(param - 1)) {
    case VmState::Preparing: update_status(L"正在准备虚拟硬盘…"); break;
    case VmState::Starting: update_status(L"正在启动…"); break;
    case VmState::Connected: update_status(L"Debian 终端已连接"); break;
    case VmState::Graphics:
        graphics_visible_ = true;
        update_status(L"图形虚拟机运行中");
        ShowWindow(window_, SW_MINIMIZE);
        if (data) SetForegroundWindow(reinterpret_cast<HWND>(data));
        break;
    case VmState::Stopping:
        update_status(data == ERROR_TIMEOUT ?
            L"客户机仍在运行，可在菜单中选择强制关机" : L"正在关机…");
        if (data == ERROR_TIMEOUT && graphics_visible_) {
            ShowWindow(window_, SW_RESTORE);
            SetForegroundWindow(window_);
        }
        break;
    case VmState::Stopped:
        if (closing_) { DestroyWindow(window_); return; }
        if (graphics_visible_) ShowWindow(window_, SW_RESTORE);
        if (data && data != ERROR_CANCELLED) {
            pending_.reset();
            update_status(L"虚拟机已退出。可从菜单打开日志");
            const auto message = L"虚拟机启动或运行失败。\n错误代码 " + std::to_wstring(data) +
                                 L"\n日志 " + session_->log_path();
            MessageBoxW(window_, message.c_str(), L"Kapsel", MB_OK | MB_ICONERROR);
        } else if (pending_) {
            PostMessageW(window_, start_pending, 0, 0);
        } else {
            update_status(L"已关机");
        }
        break;
    }
}

void AppWindow::show_menu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const auto shutdown_state = session_->active() ? MF_ENABLED : MF_GRAYED;
    const auto disk = directory_ + L"\\data\\iso.qcow2";
    const auto available = GetFileAttributesW(disk.c_str()) != INVALID_FILE_ATTRIBUTES;
    AppendMenuW(menu, MF_STRING, action_builtin, L"启动内置 Debian");
    AppendMenuW(menu, MF_STRING, action_iso, L"从 ISO 启动…");
    AppendMenuW(menu, MF_STRING | (available ? MF_ENABLED : MF_GRAYED), action_disk, L"从 ISO 虚拟硬盘启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, action_settings, L"虚拟机设置…");
    AppendMenuW(menu, MF_STRING | shutdown_state, action_shutdown, L"关机");
    AppendMenuW(menu, MF_STRING | shutdown_state, action_force, L"强制关机…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, action_copy, L"复制终端内容");
    AppendMenuW(menu, MF_STRING, action_paste, L"粘贴");
    AppendMenuW(menu, MF_STRING, action_log, L"查看 QEMU 日志");
    RECT anchor{};
    GetWindowRect(menu_button_, &anchor);
    const auto chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN, anchor.right,
                                       anchor.bottom, 0, window_, nullptr);
    DestroyMenu(menu);
    if (chosen) SendMessageW(window_, WM_COMMAND, chosen, 0);
}

LRESULT AppWindow::handle(UINT message, WPARAM param, LPARAM data)
{
    switch (message) {
    case WM_CREATE: {
        set_dpi(GetDpiForWindow(window_));
        const COLORREF caption = window_surface;
        DwmSetWindowAttribute(window_, 35, &caption, sizeof(caption));
        session_ = std::make_unique<VmSession>(window_, directory_);
        terminal_ = std::make_unique<TerminalView>(window_, *session_, dpi_);
        const auto instance = GetModuleHandleW(nullptr);
        iso_button_ = CreateWindowW(L"BUTTON", L"从 ISO 启动", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(action_iso), instance, nullptr);
        menu_button_ = CreateWindowW(L"BUTTON", L"更多操作", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(action_menu), instance, nullptr);
        layout();
        return 0;
    }
    case start_on_open: start(VmOptions{}); return 0;
    case start_pending:
        if (pending_ && !session_->active()) {
            auto options = std::move(*pending_);
            pending_.reset();
            start(std::move(options));
        } else if (pending_) {
            SetTimer(window_, 1, 20, nullptr);
        }
        return 0;
    case WM_TIMER:
        KillTimer(window_, 1);
        PostMessageW(window_, start_pending, 0, 0);
        return 0;
    case VM_MESSAGE: session_message(param, data); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(); return 0;
    case WM_DRAWITEM: return draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(data));
    case WM_SIZE: layout(); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(data);
        if (terminal_) {
            const auto size = terminal_->minimum_size();
            RECT frame{0, 0, size.cx, size.cy + MulDiv(87, dpi_, 96)};
            AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
            info->ptMinTrackSize = {frame.right - frame.left, frame.bottom - frame.top};
        }
        return 0;
    }
    case WM_DPICHANGED: {
        set_dpi(HIWORD(param));
        const auto& rect = *reinterpret_cast<RECT*>(data);
        SetWindowPos(window_, nullptr, rect.left, rect.top, rect.right - rect.left,
                     rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_SETFOCUS:
        if (terminal_) SetFocus(terminal_->window());
        return 0;
    case WM_COMMAND:
        switch (LOWORD(param)) {
        case action_iso: choose_iso(); return 0;
        case action_menu: show_menu(); return 0;
        case action_builtin: launch(VmOptions{}); return 0;
        case action_disk: {
            VmOptions options = settings_.external;
            options.mode = VmMode::Disk;
            launch(std::move(options));
            return 0;
        }
        case action_settings:
            if (edit_settings(window_, directory_, settings_))
                update_status(L"设置已保存，将在下次启动时生效");
            return 0;
        case action_shutdown: session_->shutdown(false); return 0;
        case action_force:
            if (MessageBoxW(window_, L"强制关机可能丢失客户机中未保存的内容。",
                            L"强制关机", MB_OKCANCEL | MB_ICONWARNING) == IDOK)
                session_->shutdown(true);
            return 0;
        case action_copy: terminal_->copy(); return 0;
        case action_paste: terminal_->paste(); return 0;
        case action_log: ShellExecuteW(window_, L"open", session_->log_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0;
        }
        break;
    case WM_CLOSE:
        if (session_ && session_->active()) {
            closing_ = true;
            pending_.reset();
            session_->shutdown(false);
            update_status(L"正在关机…");
        } else {
            DestroyWindow(window_);
        }
        return 0;
    case WM_DESTROY:
        terminal_.reset();
        session_.reset();
        if (title_font_) DeleteObject(title_font_);
        if (label_font_) DeleteObject(label_font_);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, param, data);
}
