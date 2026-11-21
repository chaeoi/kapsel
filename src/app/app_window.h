#pragma once

#include "settings.h"
#include "terminal/terminal_view.h"

#include <memory>
#include <optional>

class AppWindow {
public:
    explicit AppWindow(std::wstring directory);
    bool create(HINSTANCE instance, int show);

private:
    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM param, LPARAM data);
    LRESULT handle(UINT message, WPARAM param, LPARAM data);
    void layout();
    void paint();
    bool draw_button(const DRAWITEMSTRUCT& button);
    void update_status(const wchar_t* text);
    void choose_iso();
    void launch(VmOptions options);
    void start(VmOptions options);
    void session_message(WPARAM param, LPARAM data);
    void show_menu();
    void set_dpi(unsigned dpi);

    std::wstring directory_;
    Settings settings_;
    HWND window_{};
    HWND iso_button_{};
    HWND menu_button_{};
    HFONT label_font_{};
    HFONT title_font_{};
    unsigned dpi_ = 96;
    std::unique_ptr<VmSession> session_;
    std::unique_ptr<TerminalView> terminal_;
    std::optional<VmOptions> pending_;
    VmMode running_ = VmMode::Builtin;
    bool closing_ = false;
    bool graphics_visible_ = false;
    std::wstring status_;
};
