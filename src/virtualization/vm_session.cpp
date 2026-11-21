#include "vm_session.h"
#include "qmp_client.h"

#include <bcrypt.h>
#include <cwchar>
#include <utility>
#include <vector>

namespace {
class Handle {
public:
    Handle(HANDLE handle = nullptr) : handle_(handle) {}
    ~Handle() { if (valid()) CloseHandle(handle_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    operator HANDLE() const { return handle_; }
private:
    HANDLE handle_;
};

std::wstring quote(const std::wstring& argument)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        result.append(character == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        result.push_back(character);
        slashes = 0;
    }
    result.append(2 * slashes, L'\\');
    return result + L'"';
}

std::wstring drive_path(const std::wstring& path)
{
    std::wstring escaped;
    for (wchar_t character : path) {
        escaped.push_back(character);
        if (character == L',')
            escaped.push_back(character);
    }
    return escaped;
}

HANDLE run_process(const std::wstring& directory, const std::wstring& executable,
                   const std::vector<std::wstring>& arguments, HANDLE output, HANDLE input)
{
    std::wstring command = quote(executable);
    for (const auto& argument : arguments)
        command += L' ' + quote(argument);

    STARTUPINFOW info{};
    info.cb = sizeof(info);
    info.dwFlags = STARTF_USESTDHANDLES;
    info.hStdInput = input;
    info.hStdOutput = output;
    info.hStdError = output;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW((directory + L'\\' + executable).c_str(), command.data(),
                        nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, directory.c_str(), &info, &process))
        return nullptr;
    CloseHandle(process.hThread);
    return process.hProcess;
}

bool file_exists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

DWORD CALLBACK copy_progress(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER,
                             DWORD, DWORD, HANDLE, HANDLE, LPVOID cancel)
{
    return WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0 ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

std::wstring pipe_prefix()
{
    unsigned char entropy[16];
    if (BCryptGenRandom(nullptr, entropy, sizeof(entropy), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return {};
    const wchar_t hex[] = L"0123456789abcdef";
    std::wstring prefix = L"kapsel-";
    for (unsigned char byte : entropy) {
        prefix += hex[byte >> 4];
        prefix += hex[byte & 15];
    }
    return prefix;
}

struct GraphicsWindow {
    DWORD process_id;
    HWND window = nullptr;
};

BOOL CALLBACK find_graphics(HWND window, LPARAM argument)
{
    auto& data = *reinterpret_cast<GraphicsWindow*>(argument);
    DWORD pid = 0;
    wchar_t title[64];
    GetWindowThreadProcessId(window, &pid);
    if (pid == data.process_id && IsWindowVisible(window) &&
        GetWindowTextW(window, title, 64) && wcsncmp(title, L"Kapsel", 6) == 0) {
        data.window = window;
        return FALSE;
    }
    return TRUE;
}
}

VmSession::VmSession(HWND window, const std::wstring& directory)
    : window_(window), directory_(directory) {}

VmSession::~VmSession()
{
    if (thread_) {
        SetEvent(abort_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
    }
    if (command_) CloseHandle(command_);
    if (abort_) CloseHandle(abort_);
}

bool VmSession::active() const
{
    return thread_ && WaitForSingleObject(thread_, 0) == WAIT_TIMEOUT;
}

bool VmSession::start(VmOptions options)
{
    if (active()) return false;
    if (thread_) CloseHandle(thread_);
    thread_ = nullptr;
    if (!command_) command_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!abort_) abort_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!command_ || !abort_) return false;
    ResetEvent(command_);
    ResetEvent(abort_);
    options_ = std::move(options);
    AcquireSRWLockExclusive(&lock_);
    incoming_.clear();
    outgoing_.clear();
    output_posted_ = false;
    poweroff_requested_ = false;
    ReleaseSRWLockExclusive(&lock_);
    thread_ = CreateThread(nullptr, 0, worker_entry, this, 0, nullptr);
    return thread_ != nullptr;
}

void VmSession::send(const char* bytes, size_t size)
{
    if (!active()) return;
    AcquireSRWLockExclusive(&lock_);
    if (outgoing_.size() + size <= 256 * 1024)
        outgoing_.append(bytes, size);
    ReleaseSRWLockExclusive(&lock_);
    SetEvent(command_);
}

void VmSession::shutdown(bool force)
{
    if (force) {
        if (abort_) SetEvent(abort_);
        return;
    }
    AcquireSRWLockExclusive(&lock_);
    poweroff_requested_ = true;
    ReleaseSRWLockExclusive(&lock_);
    if (command_) SetEvent(command_);
}

std::string VmSession::receive_output()
{
    std::string output;
    AcquireSRWLockExclusive(&lock_);
    output.swap(incoming_);
    output_posted_ = false;
    ReleaseSRWLockExclusive(&lock_);
    if (command_) SetEvent(command_);
    return output;
}

std::wstring VmSession::log_path() const
{
    return directory_ + L"\\data\\qemu.log";
}

void VmSession::post_output(const char* bytes, DWORD size)
{
    AcquireSRWLockExclusive(&lock_);
    incoming_.append(bytes, size);
    if (!output_posted_) {
        output_posted_ = true;
        PostMessageW(window_, VM_MESSAGE, 0, 0);
    }
    ReleaseSRWLockExclusive(&lock_);
}

void VmSession::state(VmState value, LPARAM detail)
{
    PostMessageW(window_, VM_MESSAGE, static_cast<WPARAM>(value) + 1, detail);
}

DWORD WINAPI VmSession::worker_entry(void* context)
{
    return static_cast<VmSession*>(context)->worker();
}

DWORD VmSession::worker()
{
    const auto execute = [&]() -> DWORD {
        const std::wstring folder = directory_ + L"\\data";
        if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return GetLastError();

        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        Handle lock(CreateFileW((folder + L"\\session.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, &inherit, OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        if (!lock.valid()) return GetLastError();
        Handle log(CreateFileW(log_path().c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!log.valid()) return GetLastError();
        Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!input.valid()) return GetLastError();

        const bool builtin = options_.mode == VmMode::Builtin;
        const std::wstring relative_disk = builtin ? L"data\\debian.qcow2" : L"data\\iso.qcow2";
        const std::wstring disk = directory_ + L'\\' + relative_disk;
        const std::wstring temporary = disk + L".partial";
        if (!file_exists(disk)) {
            if (options_.mode == VmMode::Disk)
                return ERROR_FILE_NOT_FOUND;
            state(VmState::Preparing);
            DeleteFileW(temporary.c_str());
            if (builtin) {
                if (!CopyFileExW((directory_ + L"\\guest\\debian.qcow2").c_str(), temporary.c_str(),
                                 copy_progress, abort_, nullptr, COPY_FILE_FAIL_IF_EXISTS)) {
                    DWORD failure = GetLastError();
                    DeleteFileW(temporary.c_str());
                    return failure;
                }
            } else {
                const std::wstring relative_temporary = relative_disk + L".partial";
                Handle creator(run_process(directory_, L"runtime\\qemu-img.exe",
                    {L"create", L"-f", L"qcow2", relative_temporary,
                     std::to_wstring(options_.disk_gib) + L"G"}, log, input));
                if (!creator.valid()) return GetLastError();
                const HANDLE waits[] = {creator, abort_};
                const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 20000);
                DWORD exit_code = 1;
                if (waited == WAIT_OBJECT_0) GetExitCodeProcess(creator, &exit_code);
                else {
                    TerminateProcess(creator, 1);
                    WaitForSingleObject(creator, 2000);
                }
                if (exit_code != 0) {
                    DeleteFileW(temporary.c_str());
                    return waited == WAIT_OBJECT_0 + 1 ? ERROR_CANCELLED : ERROR_WRITE_FAULT;
                }
            }
            if (!MoveFileW(temporary.c_str(), disk.c_str())) {
                const DWORD error = GetLastError();
                DeleteFileW(temporary.c_str());
                return error;
            }
        }
        if (WaitForSingleObject(abort_, 0) == WAIT_OBJECT_0) return ERROR_CANCELLED;
        const std::wstring pipe = pipe_prefix();
        if (pipe.empty()) return ERROR_GEN_FAILURE;
        const std::wstring console_name = pipe + L"-terminal";
        const std::wstring qmp_name = pipe + L"-qmp";
        std::vector<std::wstring> args = {L"-L", L"runtime\\firmware", L"-machine", L"q35",
            L"-smp", std::to_wstring(options_.cpus), L"-m", std::to_wstring(options_.memory_mib),
            L"-no-user-config", L"-monitor", L"none", L"-parallel", L"none",
            L"-netdev", L"user,id=nat",
            L"-device", L"virtio-net-pci,netdev=nat,addr=0x3,romfile=",
            L"-drive", L"file=" + relative_disk + L",format=qcow2,if=virtio"};
        if (options_.whpx)
            args.insert(args.end(), {L"-accel", L"whpx"});
        args.insert(args.end(), {L"-accel", L"tcg"});
        if (builtin) {
            args.insert(args.end(), {L"-display", L"none", L"-vga", L"none",
                L"-chardev", L"pipe,id=terminal,path=" + console_name, L"-serial", L"chardev:terminal"});
        } else {
            args.insert(args.end(), {L"-display", L"sdl", L"-vga", L"std",
                L"-device", L"qemu-xhci", L"-device", L"usb-tablet"});
        }
        args.insert(args.end(), {L"-chardev", L"pipe,id=management,path=" + qmp_name,
                                L"-object", L"monitor-qmp,id=qmp,chardev=management"});
        if (options_.mode == VmMode::Install) {
            args.insert(args.end(), {L"-boot", L"order=c,once=d,menu=on", L"-drive",
                L"file=" + drive_path(options_.iso) + L",format=raw,if=ide,index=2,media=cdrom,readonly=on"});
        } else {
            args.insert(args.end(), {L"-boot", L"order=c"});
        }

        state(VmState::Starting);
        Handle process(run_process(directory_, L"runtime\\qemu-system-x86_64.exe", args, log, input));
        if (!process.valid()) return GetLastError();
        const auto finish_process = [&](DWORD code) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 3000);
            }
            return code;
        };
        Handle terminal(builtin ? connect_vm_pipe(console_name, process, abort_) : nullptr);
        Handle qmp((!builtin || terminal.valid()) ? connect_vm_pipe(qmp_name, process, abort_) : nullptr);
        if (!qmp.valid() || !initialize_qmp(qmp, abort_))
            return finish_process(WaitForSingleObject(abort_, 0) == WAIT_OBJECT_0 ?
                                  ERROR_CANCELLED : ERROR_CONNECTION_ABORTED);
        if (builtin) {
            state(VmState::Connected);
        } else {
            GraphicsWindow graphics{GetProcessId(process)};
            EnumWindows(find_graphics, reinterpret_cast<LPARAM>(&graphics));
            state(VmState::Graphics, reinterpret_cast<LPARAM>(graphics.window));
        }

        Handle read_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!read_event.valid()) return finish_process(GetLastError());
        OVERLAPPED reader{};
        reader.hEvent = read_event;
        char text[8192];
        bool reading = false;
        bool stopping = false;
        ULONGLONG stop_deadline = 0;
        const HANDLE waits[] = {process, abort_, command_, read_event};
        while (true) {
            if (terminal.valid() && !reading) {
                AcquireSRWLockShared(&lock_);
                const bool room = incoming_.size() < 256 * 1024;
                ReleaseSRWLockShared(&lock_);
                if (room) {
                    DWORD received = 0;
                    ResetEvent(read_event);
                    if (ReadFile(terminal, text, sizeof(text), &received, &reader)) {
                        if (received) post_output(text, received);
                        ResetEvent(read_event);
                        if (received) continue;
                    } else {
                        reading = GetLastError() == ERROR_IO_PENDING;
                    }
                }
            }

            const auto now = GetTickCount64();
            DWORD timeout = INFINITE;
            if (stop_deadline) {
                const auto remaining = stop_deadline > now ? static_cast<DWORD>(stop_deadline - now) : 0;
                timeout = std::min(timeout, remaining);
            }
            const DWORD event = WaitForMultipleObjects(4, waits, FALSE, timeout);
            if (event == WAIT_OBJECT_0 + 3 && reading) {
                DWORD received = 0;
                if (GetOverlappedResult(terminal, &reader, &received, FALSE) && received)
                    post_output(text, received);
                reading = false;
                ResetEvent(read_event);
                continue;
            }
            if (event == WAIT_OBJECT_0 + 2) {
                std::string outgoing;
                AcquireSRWLockExclusive(&lock_);
                outgoing.swap(outgoing_);
                const bool poweroff = std::exchange(poweroff_requested_, false);
                ReleaseSRWLockExclusive(&lock_);
                if (!outgoing.empty())
                    write_vm_pipe(terminal, abort_, outgoing.data(), static_cast<DWORD>(outgoing.size()));
                if (poweroff && !stopping) {
                    if (request_powerdown(qmp, abort_)) {
                        state(VmState::Stopping);
                        stopping = true;
                        stop_deadline = GetTickCount64() + 15000;
                    } else {
                        state(VmState::Stopping, ERROR_TIMEOUT);
                    }
                }
                continue;
            }
            if (event == WAIT_TIMEOUT) {
                if (stop_deadline && GetTickCount64() >= stop_deadline) {
                    state(VmState::Stopping, ERROR_TIMEOUT);
                    stop_deadline = 0;
                }
                continue;
            }

            if (reading) {
                CancelIoEx(terminal, &reader);
                DWORD discarded;
                GetOverlappedResult(terminal, &reader, &discarded, TRUE);
            }
            if (event == WAIT_OBJECT_0 + 1) return finish_process(ERROR_CANCELLED);
            DWORD code = 1;
            GetExitCodeProcess(process, &code);
            return code ? ERROR_GEN_FAILURE : ERROR_SUCCESS;
        }
    };
    const DWORD result = execute();
    state(VmState::Stopped, result);
    return 0;
}
