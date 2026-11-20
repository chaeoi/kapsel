#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "appearance.h"
#include "resource.h"

#define DISK L"vm.qcow2"
#define NEW_DISK L"vm.qcow2.creating"
#define LOG_FILE L"qemu.log"
#define QEMU L"qemu\\qemu-system-x86_64.exe"
#define QEMU_IMG L"qemu\\qemu-img.exe"

enum { ISO_PATH_CAP = 4096, COMMAND_CAP = ISO_PATH_CAP * 2 + 1024, VM_TIMER = 1 };
enum { MAX_CPUS = 64, MAX_MEMORY_GIB = 256, MAX_DISK_GIB = 2048 };
static wchar_t iso_path[ISO_PATH_CAP];
static wchar_t ini_path[32768];
static HANDLE vm, disk_lock;
static DWORD window_visible_since;
static const wchar_t handoff_note[] = L"虚拟机窗口打开后，启动器会自动关闭。";

typedef struct {
    UINT cpus;
    UINT memory_gib;
    BOOL whpx;
} VmSettings;

static void show_error(HWND dialog, const wchar_t *message)
{
    MessageBoxW(dialog, message, L"Kapsel", MB_OK | MB_ICONERROR);
}

static BOOL is_file(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL read_number(HWND dialog, int id, UINT max, UINT *value)
{
    wchar_t text[8];
    if (!GetDlgItemTextW(dialog, id, text, ARRAYSIZE(text)) ||
        GetWindowTextLengthW(GetDlgItem(dialog, id)) >= (int)ARRAYSIZE(text))
        return FALSE;
    for (const wchar_t *p = text; *p; ++p) {
        if (*p < L'0' || *p > L'9') return FALSE;
    }
    const unsigned long number = wcstoul(text, NULL, 10);
    if (number < 1 || number > max) return FALSE;
    *value = (UINT)number;
    return TRUE;
}

static UINT saved_number(const wchar_t *name, UINT fallback, UINT max)
{
    UINT value = GetPrivateProfileIntW(L"vm", name, fallback, ini_path);
    return value && value <= max ? value : fallback;
}

static void save_number(const wchar_t *name, UINT value)
{
    wchar_t text[12];
    swprintf(text, ARRAYSIZE(text), L"%u", value);
    WritePrivateProfileStringW(L"vm", name, text, ini_path);
}

static ULONGLONG disk_size_bytes(void)
{
    HANDLE file = CreateFileW(DISK, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                              FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;

    BYTE header[32];
    DWORD length = 0;
    BOOL read = ReadFile(file, header, sizeof(header), &length, NULL);
    CloseHandle(file);
    if (!read || length != sizeof(header) || memcmp(header, "QFI\xfb", 4) != 0)
        return 0;

    // The QCOW2 header stores the virtual capacity as a big-endian integer.
    ULONGLONG bytes = 0;
    for (size_t i = 24; i < sizeof(header); ++i) bytes = (bytes << 8) | header[i];
    return bytes;
}

static void refresh_disk(HWND dialog)
{
    const BOOL exists = is_file(DISK);
    wchar_t value[32] = L"";
    wchar_t summary[38] = L"—";
    if (exists) {
        const ULONGLONG bytes = disk_size_bytes();
        const ULONGLONG gib = 1ULL << 30;
        if (bytes && bytes % gib == 0)
            swprintf(value, ARRAYSIZE(value), L"%llu", bytes / gib);
        else if (bytes)
            swprintf(value, ARRAYSIZE(value), L"%.2f", (double)bytes / gib);
        else
            wcscpy(value, L"—");
        SetDlgItemTextW(dialog, IDC_DISK_SIZE, value);
    } else {
        UINT disk_gib;
        if (read_number(dialog, IDC_DISK_SIZE, MAX_DISK_GIB, &disk_gib))
            swprintf(value, ARRAYSIZE(value), L"%u", disk_gib);
    }
    if (value[0] && value[0] != L'—')
        swprintf(summary, ARRAYSIZE(summary), L"%ls GiB", value);
    SendDlgItemMessageW(dialog, IDC_DISK_SIZE, EM_SETREADONLY, exists, 0);
    SetDlgItemTextW(dialog, IDC_DISK_VALUE, summary);
    SetDlgItemTextW(dialog, IDC_DISK_NOTE, exists ? L"已创建 · 按需增长" : L"首次启动时创建");
    SetDlgItemTextW(dialog, IDC_DISK_SIZE_LABEL, exists ? L"硬盘容量" : L"新盘容量");
}

static void refresh_dialog(HWND dialog)
{
    BOOL has_disk = is_file(DISK);
    EnableWindow(GetDlgItem(dialog, IDC_BROWSE), !vm);
    EnableWindow(GetDlgItem(dialog, IDC_WHPX), !vm);
    EnableWindow(GetDlgItem(dialog, IDC_CPU), !vm);
    EnableWindow(GetDlgItem(dialog, IDC_MEMORY), !vm);
    EnableWindow(GetDlgItem(dialog, IDC_DISK_SIZE), !vm);
    EnableWindow(GetDlgItem(dialog, IDC_BOOT_ISO), !vm && iso_path[0]);
    EnableWindow(GetDlgItem(dialog, IDC_BOOT_DISK), !vm && has_disk);
    SendMessageW(dialog, DM_SETDEFID,
                 iso_path[0] ? IDC_BOOT_ISO : has_disk ? IDC_BOOT_DISK : IDC_BROWSE, 0);
    InvalidateRect(GetDlgItem(dialog, IDC_BROWSE), NULL, FALSE);
    InvalidateRect(GetDlgItem(dialog, IDC_BOOT_ISO), NULL, FALSE);
    InvalidateRect(GetDlgItem(dialog, IDC_BOOT_DISK), NULL, FALSE);
    SetDlgItemTextW(dialog, IDC_STATUS, vm ? L"正在打开虚拟机窗口…" : handoff_note);
}

static void select_iso(HWND dialog)
{
    wchar_t chosen[ISO_PATH_CAP] = L"";
    OPENFILENAMEW picker = {
        .lStructSize = sizeof(picker),
        .hwndOwner = dialog,
        .lpstrFilter = L"ISO 镜像 (*.iso)\0*.iso\0所有文件 (*.*)\0*.*\0",
        .lpstrFile = chosen,
        .nMaxFile = ARRAYSIZE(chosen),
        .Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR,
    };
    if (GetOpenFileNameW(&picker)) {
        wcscpy(iso_path, chosen);
        SetDlgItemTextW(dialog, IDC_ISO, iso_path);
        refresh_dialog(dialog);
    } else if (CommDlgExtendedError()) {
        show_error(dialog, L"无法打开 ISO 选择窗口或所选路径过长。");
    }
}

static HANDLE run_qemu(const wchar_t *exe, wchar_t *command,
                       HANDLE input, HANDLE log)
{
    STARTUPINFOW startup = {
        .cb = sizeof(startup),
        .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = input,
        .hStdOutput = log,
        .hStdError = log,
    };
    PROCESS_INFORMATION process;
    if (!CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process)) {
        return NULL;
    }
    CloseHandle(process.hThread);
    return process.hProcess;
}

static BOOL create_disk(HANDLE input, HANDLE log, UINT size_gib)
{
    // Publish the image only after qemu-img succeeds. The folder lock covers both paths.
    DeleteFileW(NEW_DISK);
    wchar_t command[96];
    swprintf(command, ARRAYSIZE(command), L"qemu-img.exe create -f qcow2 " NEW_DISK L" %uG",
             size_gib);
    HANDLE creator = run_qemu(QEMU_IMG, command, input, log);
    if (!creator) return FALSE;

    DWORD code = 1;
    if (WaitForSingleObject(creator, 20000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(creator, &code);
    } else {
        // The child keeps the folder lock if it is still finishing the temporary image.
        TerminateProcess(creator, 1);
    }
    CloseHandle(creator);
    if (code == 0 && MoveFileW(NEW_DISK, DISK)) return TRUE;

    DeleteFileW(NEW_DISK);
    return FALSE;
}

static void build_command(wchar_t *command, BOOL from_iso, const VmSettings *settings)
{
    swprintf(command, COMMAND_CAP, L"qemu-system-x86_64.exe "
             L"-L qemu\\share -machine q35 %ls-accel tcg "
             L"-smp %u -m %u -display sdl -vga std -device qemu-xhci -device usb-tablet "
             L"-netdev user,id=nat -device virtio-net-pci,netdev=nat,romfile= -monitor none "
             L"-drive file=" DISK L",format=qcow2,if=virtio -boot %ls",
             settings->whpx ? L"-accel whpx " : L"",
             settings->cpus, settings->memory_gib * 1024,
             from_iso ? L"order=c,once=d,menu=on" : L"order=c");
    if (!from_iso) return;

    // QEMU uses UTF-8 argv with the process code page set by its manifest.
    wchar_t *argument_end = command + wcslen(command);
    wcscpy(argument_end, L" -drive \"file=");
    argument_end += wcslen(argument_end);
    for (const wchar_t *p = iso_path; *p; ++p) {
        *argument_end++ = *p;
        if (*p == L',') *argument_end++ = L',';
    }
    wcscpy(argument_end, L",format=raw,if=ide,index=2,media=cdrom,readonly=on\"");
}

static void number_error(HWND dialog, int id, const wchar_t *label, UINT max)
{
    wchar_t message[96];
    swprintf(message, ARRAYSIZE(message), L"%ls请输入 1–%u 的整数。", label, max);
    SetDlgItemTextW(dialog, IDC_STATUS, message);
    SetFocus(GetDlgItem(dialog, id));
    SendDlgItemMessageW(dialog, id, EM_SETSEL, 0, -1);
}

static void start_vm(HWND dialog, BOOL from_iso)
{
    if (vm) return;
    if (from_iso && !is_file(iso_path)) {
        show_error(dialog, L"所选 ISO 已移动或无法读取，请重新选择。");
        return;
    }
    if (!is_file(QEMU) || !is_file(QEMU_IMG)) {
        show_error(dialog, L"找不到 QEMU，请将便携包完整解压到同一个文件夹。");
        return;
    }
    VmSettings settings = { .whpx = IsDlgButtonChecked(dialog, IDC_WHPX) == BST_CHECKED };
    if (!read_number(dialog, IDC_CPU, MAX_CPUS, &settings.cpus)) {
        number_error(dialog, IDC_CPU, L"虚拟 CPU：", MAX_CPUS);
        return;
    }
    if (!read_number(dialog, IDC_MEMORY, MAX_MEMORY_GIB, &settings.memory_gib)) {
        number_error(dialog, IDC_MEMORY, L"内存（GiB）：", MAX_MEMORY_GIB);
        return;
    }

    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    // The child inherits the lock, so closing the launcher cannot allow a second writer.
    disk_lock = CreateFileW(L"kapsel.lock", GENERIC_READ | GENERIC_WRITE, 0, &inherit,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                            NULL);
    if (disk_lock == INVALID_HANDLE_VALUE) {
        show_error(dialog, L"虚拟机正在使用此文件夹，或当前文件夹不可写。");
        return;
    }

    HANDLE log = INVALID_HANDLE_VALUE;
    HANDLE input = INVALID_HANDLE_VALUE;
    const wchar_t *error = NULL;
    const BOOL has_disk = is_file(DISK);
    if (!from_iso && !has_disk) {
        error = L"找不到 vm.qcow2，请先从 ISO 启动。";
        goto finish;
    }
    UINT disk_gib = 0;
    if (!has_disk && !read_number(dialog, IDC_DISK_SIZE, MAX_DISK_GIB, &disk_gib)) {
        CloseHandle(disk_lock);
        number_error(dialog, IDC_DISK_SIZE, L"新盘容量（GiB）：", MAX_DISK_GIB);
        return;
    }

    log = CreateFileW(LOG_FILE, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        error = L"无法打开 qemu.log，请检查当前文件夹是否可写。";
        goto finish;
    }
    if (!has_disk) {
        SetDlgItemTextW(dialog, IDC_STATUS, L"正在创建虚拟硬盘…");
        UpdateWindow(GetDlgItem(dialog, IDC_STATUS));
        if (!create_disk(input, log, disk_gib)) {
            error = L"无法创建 vm.qcow2，详情见同目录的 qemu.log。";
            goto finish;
        }
    }

    wchar_t command[COMMAND_CAP];
    build_command(command, from_iso, &settings);
    vm = run_qemu(QEMU, command, input, log);
    if (vm) {
        save_number(L"cpus", settings.cpus);
        save_number(L"memory_gib", settings.memory_gib);
        save_number(L"whpx", settings.whpx);
    } else {
        error = L"无法运行 QEMU，请检查便携包中的运行文件。";
    }

finish:
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (vm) {
        window_visible_since = 0;
        SetTimer(dialog, VM_TIMER, 150, NULL);
    } else {
        CloseHandle(disk_lock);
    }
    refresh_disk(dialog);
    refresh_dialog(dialog);
    if (error) show_error(dialog, error);
}

typedef struct {
    DWORD process_id;
    HWND window;
} VmWindow;

static BOOL CALLBACK find_vm_window(HWND window, LPARAM param)
{
    VmWindow *candidate = (VmWindow *)param;
    DWORD process_id = 0;
    if (!GetWindowThreadProcessId(window, &process_id) ||
        process_id != candidate->process_id || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER))
        return TRUE;

    wchar_t class_name[32];
    if (!GetClassNameW(window, class_name, ARRAYSIZE(class_name)) ||
        wcscmp(class_name, L"SDL_app") != 0)
        return TRUE;
    candidate->window = window;
    return FALSE;
}

static void check_vm(HWND dialog)
{
    if (!vm) return;
    if (WaitForSingleObject(vm, 0) == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(vm, &code);
        CloseHandle(vm);
        CloseHandle(disk_lock);
        vm = NULL;
        KillTimer(dialog, VM_TIMER);
        refresh_disk(dialog);
        refresh_dialog(dialog);
        if (code) show_error(dialog, L"虚拟机异常退出，详情见同目录的 qemu.log。");
        return;
    }

    VmWindow window = { .process_id = GetProcessId(vm) };
    EnumWindows(find_vm_window, (LPARAM)&window);
    if (!window.window) {
        window_visible_since = 0;
        return;
    }
    // Let startup errors reach the launcher before handing off to the guest window.
    const DWORD now = GetTickCount();
    if (!window_visible_since) window_visible_since = now;
    if (now - window_visible_since >= 400) {
        SetForegroundWindow(window.window);
        EndDialog(dialog, 0);
    }
}

static INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM param, LPARAM data)
{
    switch (message) {
    case WM_INITDIALOG:
        init_appearance(dialog);
        SetDlgItemTextW(dialog, IDC_ISO, L"选择用于启动的 ISO 镜像");
        SendDlgItemMessageW(dialog, IDC_CPU, EM_SETLIMITTEXT, 2, 0);
        SendDlgItemMessageW(dialog, IDC_MEMORY, EM_SETLIMITTEXT, 3, 0);
        SendDlgItemMessageW(dialog, IDC_DISK_SIZE, EM_SETLIMITTEXT, 4, 0);
        SetDlgItemInt(dialog, IDC_CPU, saved_number(L"cpus", 2, MAX_CPUS), FALSE);
        SetDlgItemInt(dialog, IDC_MEMORY, saved_number(L"memory_gib", 4, MAX_MEMORY_GIB), FALSE);
        SetDlgItemInt(dialog, IDC_DISK_SIZE, 64, FALSE);
        CheckDlgButton(dialog, IDC_WHPX, GetPrivateProfileIntW(L"vm", L"whpx", 0, ini_path)
                       ? BST_CHECKED : BST_UNCHECKED);
        refresh_disk(dialog);
        refresh_dialog(dialog);
        SetFocus(GetDlgItem(dialog, is_file(DISK) ? IDC_BOOT_DISK : IDC_BROWSE));
        return FALSE;

    case WM_COMMAND:
        if (HIWORD(param) == EN_SETFOCUS || HIWORD(param) == EN_KILLFOCUS)
            InvalidateRect(dialog, NULL, TRUE);
        if (HIWORD(param) == EN_CHANGE && LOWORD(param) >= IDC_CPU &&
            LOWORD(param) <= IDC_DISK_SIZE && !vm) {
            SetDlgItemTextW(dialog, IDC_STATUS, handoff_note);
            if (LOWORD(param) == IDC_DISK_SIZE && !is_file(DISK)) refresh_disk(dialog);
        }
        switch (LOWORD(param)) {
        case IDC_BROWSE: select_iso(dialog); return TRUE;
        case IDC_BOOT_ISO: start_vm(dialog, TRUE); return TRUE;
        case IDC_BOOT_DISK: start_vm(dialog, FALSE); return TRUE;
        case IDCANCEL: EndDialog(dialog, 0); return TRUE;
        }
        break;

    case WM_TIMER:
        if (param == VM_TIMER) check_vm(dialog);
        return TRUE;

    case WM_ERASEBKGND:
        return paint_dialog_background(dialog, (HDC)param);
    case WM_DRAWITEM:
        return paint_dialog_button(dialog, (const DRAWITEMSTRUCT *)data);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return color_dialog_control(message, (HDC)param, (HWND)data);

    case WM_CLOSE:
        EndDialog(dialog, 0);
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show)
{
    (void)previous;
    (void)args;
    (void)show;

    DWORD length = GetModuleFileNameW(NULL, ini_path, ARRAYSIZE(ini_path));
    wchar_t *separator = length && length < ARRAYSIZE(ini_path) ? wcsrchr(ini_path, L'\\') : NULL;
    if (!separator) return 1;
    wchar_t *exe_name = separator + 1;
    if ((size_t)(exe_name - ini_path) + ARRAYSIZE(L"kapsel.ini") > ARRAYSIZE(ini_path)) return 1;
    *exe_name = L'\0';
    if (!SetCurrentDirectoryW(ini_path)) {
        show_error(NULL, L"无法访问 Kapsel 所在的文件夹。");
        return 1;
    }
    wcscpy(exe_name, L"kapsel.ini");

    INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_KAPSEL), NULL, dialog_proc, 0);
    release_appearance();
    if (vm) {
        CloseHandle(vm);
        CloseHandle(disk_lock);
    }
    return result == -1;
}
