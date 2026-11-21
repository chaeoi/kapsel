#pragma once

#include <windows.h>
#include <vterm.h>
#include <deque>
#include <vector>

class VmSession;

class TerminalView {
public:
    TerminalView(HWND parent, VmSession& session, unsigned dpi);
    ~TerminalView();
    HWND window() const { return window_; }
    SIZE minimum_size() const;
    void reset();
    void append(const char* bytes, size_t size);
    void set_dpi(unsigned dpi);
    void copy();
    void paste();

private:
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    static int damage(VTermRect rect, void* context);
    static int move_cursor(VTermPos position, VTermPos old, int visible, void* context);
    static int scroll_line(int columns, const VTermScreenCell* cells, void* context);
    static int pop_line(int columns, VTermScreenCell* cells, void* context);
    static void output(const char* bytes, size_t size, void* context);
    static VTermModifier modifiers();
    bool key_down(WPARAM key);
    void paint();
    void paint_row(HDC dc, int row, int min_x, int max_x);
    void invalidate_cell(VTermPos position);
    VTermScreenCell cell_at(int row, int column) const;

    HWND window_{};
    VmSession& session_;
    VTerm* terminal_{};
    VTermScreen* screen_{};
    HFONT font_{};
    int cell_width_ = 9;
    int cell_height_ = 20;
    int padding_ = 20;
    int scroll_ = 0;
    unsigned short surrogate_ = 0;
    bool cursor_visible_ = true;
    VTermPos cursor_{};
    std::deque<std::vector<VTermScreenCell>> history_;
};
