#include "app_window.h"

#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return 1;
    std::wstring directory(path.data(), length);
    directory.resize(directory.find_last_of(L'\\'));
    AppWindow window(std::move(directory));
    if (!window.create(instance, show)) return 1;
    MSG message{};
    BOOL next;
    while ((next = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return next < 0 ? 1 : static_cast<int>(message.wParam);
}
