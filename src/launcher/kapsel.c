#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <wchar.h>
#include "resource.h"

#define DISK L"vm.qcow2"
#define NEW_DISK L"vm.qcow2.creating"
#define LOG_FILE L"qemu.log"
#define QEMU L"qemu\\qemu-system-x86_64.exe"
#define QEMU_IMG L"qemu\\qemu-img.exe"

enum { ISO_PATH_CAP = 4096, COMMAND_CAP = ISO_PATH_CAP * 2 + 1024, VM_TIMER = 1 };
static wchar_t iso_path[ISO_PATH_CAP];
static wchar_t ini_path[32768];
static HANDLE vm, disk_lock;

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

static void refresh_dialog(HWND dialog)
{
    BOOL has_disk = is_file(DISK);
    EnableWindow(GetDlgItem(dialog, IDC_BROWSE), !vm);
    EnableWindow(GetDlgItem(dialog, IDC_WHPX), !vm);
    EnableWindow(GetDlgItem(dialog, IDC_BOOT_ISO), !vm && iso_path[0]);
    EnableWindow(GetDlgItem(dialog, IDC_BOOT_DISK), !vm && has_disk);
    SetDlgItemTextW(dialog, IDC_STATUS, vm ? L"虚拟机运行中" :
                   (has_disk ? L"虚拟硬盘　vm.qcow2" : L"首次从 ISO 启动时创建 64 GB 虚拟硬盘"));
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

static BOOL create_disk(HANDLE input, HANDLE log)
{
    // Publish the image only after qemu-img succeeds. The folder lock covers both paths.
    DeleteFileW(NEW_DISK);
    wchar_t command[] = L"qemu-img.exe create -f qcow2 " NEW_DISK L" 64G";
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

static void build_command(wchar_t *command, BOOL from_iso, BOOL whpx)
{
    swprintf(command, COMMAND_CAP, L"qemu-system-x86_64.exe "
             L"-L qemu\\share -machine q35 %ls-accel tcg "
             L"-smp 2 -m 4096 -display sdl -vga std -device qemu-xhci -device usb-tablet "
             L"-netdev user,id=nat -device virtio-net-pci,netdev=nat,romfile= -monitor none "
             L"-drive file=" DISK L",format=qcow2,if=virtio -boot %ls",
             whpx ? L"-accel whpx " : L"",
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
    if (!from_iso && !is_file(DISK)) {
        error = L"找不到 vm.qcow2，请先从 ISO 启动。";
        goto finish;
    }

    log = CreateFileW(LOG_FILE, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        error = L"无法打开 qemu.log，请检查当前文件夹是否可写。";
        goto finish;
    }
    if (!is_file(DISK) && !create_disk(input, log)) {
        error = L"无法创建 vm.qcow2，详情见同目录的 qemu.log。";
        goto finish;
    }

    BOOL whpx = IsDlgButtonChecked(dialog, IDC_WHPX) == BST_CHECKED;
    WritePrivateProfileStringW(L"vm", L"whpx", whpx ? L"1" : L"0", ini_path);
    wchar_t command[COMMAND_CAP];
    build_command(command, from_iso, whpx);
    vm = run_qemu(QEMU, command, input, log);
    if (!vm) error = L"无法运行 QEMU，请检查便携包中的运行文件。";

finish:
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (vm) {
        SetTimer(dialog, VM_TIMER, 250, NULL);
    } else {
        CloseHandle(disk_lock);
    }
    refresh_dialog(dialog);
    if (error) show_error(dialog, error);
}

static INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM param, LPARAM data)
{
    (void)data;
    switch (message) {
    case WM_INITDIALOG:
        CheckDlgButton(dialog, IDC_WHPX, GetPrivateProfileIntW(L"vm", L"whpx", 0, ini_path)
                       ? BST_CHECKED : BST_UNCHECKED);
        refresh_dialog(dialog);
        SetFocus(GetDlgItem(dialog, is_file(DISK) ? IDC_BOOT_DISK : IDC_BROWSE));
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(param)) {
        case IDC_BROWSE: select_iso(dialog); return TRUE;
        case IDC_BOOT_ISO: start_vm(dialog, TRUE); return TRUE;
        case IDC_BOOT_DISK: start_vm(dialog, FALSE); return TRUE;
        case IDCANCEL: EndDialog(dialog, 0); return TRUE;
        }
        break;

    case WM_TIMER:
        if (param == VM_TIMER && WaitForSingleObject(vm, 0) == WAIT_OBJECT_0) {
            DWORD code = 1;
            GetExitCodeProcess(vm, &code);
            CloseHandle(vm);
            CloseHandle(disk_lock);
            vm = NULL;
            KillTimer(dialog, VM_TIMER);
            refresh_dialog(dialog);
            if (code) show_error(dialog, L"虚拟机异常退出，详情见同目录的 qemu.log。");
        }
        return TRUE;

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
    if (vm) {
        CloseHandle(vm);
        CloseHandle(disk_lock);
    }
    return result == -1;
}
