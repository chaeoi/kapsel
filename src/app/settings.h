#pragma once

#include "virtualization/vm_session.h"

struct Settings {
    VmOptions external{VmMode::Install, 2, 4096, 32, false, {}};
    bool whpx = false;
};

Settings read_settings(const std::wstring& directory);
bool edit_settings(HWND owner, const std::wstring& directory, Settings& settings);
