#include "settings.h"

#include "../../packaging/windows/resources.h"
#include <cwchar>

namespace {
const wchar_t section[] = L"virtualization";
struct Field {
    int control;
    const wchar_t* key;
    unsigned minimum;
    unsigned maximum;
    unsigned VmOptions::*member;
};
const Field fields[] = {
    {IDC_CPUS, L"cpus", 1, 64, &VmOptions::cpus},
    {IDC_MEMORY, L"memory_mib", 256, 262144, &VmOptions::memory_mib},
    {IDC_CAPACITY, L"disk_gib", 1, 2048, &VmOptions::disk_gib},
};

INT_PTR CALLBACK settings_dialog(HWND dialog, UINT message, WPARAM command, LPARAM context)
{
    if (message == WM_INITDIALOG) {
        auto* settings = reinterpret_cast<Settings*>(context);
        SetWindowLongPtrW(dialog, DWLP_USER, context);
        for (const auto& field : fields)
            SetDlgItemInt(dialog, field.control, settings->external.*field.member, FALSE);
        CheckDlgButton(dialog, IDC_WHPX, settings->whpx ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(command) == IDOK) {
        auto* settings = reinterpret_cast<Settings*>(GetWindowLongPtrW(dialog, DWLP_USER));
        VmOptions options = settings->external;
        for (const auto& field : fields) {
            wchar_t value[12]{};
            GetDlgItemTextW(dialog, field.control, value, 12);
            wchar_t* end = nullptr;
            const auto number = wcstoul(value, &end, 10);
            if (!value[0] || *end || number < field.minimum || number > field.maximum) {
                const auto text = L"请输入 " + std::to_wstring(field.minimum) +
                                  L" 至 " + std::to_wstring(field.maximum) + L" 之间的整数。";
                MessageBoxW(dialog, text.c_str(), L"虚拟机设置", MB_ICONWARNING | MB_OK);
                SetFocus(GetDlgItem(dialog, field.control));
                return TRUE;
            }
            options.*field.member = number;
        }
        settings->external = options;
        settings->whpx = IsDlgButtonChecked(dialog, IDC_WHPX) == BST_CHECKED;
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(command) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
}

Settings read_settings(const std::wstring& directory)
{
    Settings settings;
    const auto path = directory + L"\\settings.ini";
    for (const auto& field : fields) {
        const auto number = GetPrivateProfileIntW(section, field.key,
            settings.external.*field.member, path.c_str());
        if (number >= field.minimum && number <= field.maximum)
            settings.external.*field.member = number;
    }
    settings.whpx = GetPrivateProfileIntW(section, L"whpx", 0, path.c_str()) == 1;
    return settings;
}

bool edit_settings(HWND owner, const std::wstring& directory, Settings& settings)
{
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), owner,
                        settings_dialog, reinterpret_cast<LPARAM>(&settings)) != IDOK)
        return false;
    const auto path = directory + L"\\settings.ini";
    for (const auto& field : fields) {
        const auto value = std::to_wstring(settings.external.*field.member);
        if (!WritePrivateProfileStringW(section, field.key, value.c_str(), path.c_str()))
            return false;
    }
    return WritePrivateProfileStringW(section, L"whpx", settings.whpx ? L"1" : L"0", path.c_str());
}
