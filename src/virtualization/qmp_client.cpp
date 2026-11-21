#include "qmp_client.h"

#include <string_view>

HANDLE connect_vm_pipe(const std::wstring& name, HANDLE process, HANDLE cancel)
{
    const HANDLE waits[] = {process, cancel};
    const auto deadline = GetTickCount64() + 12000;
    const std::wstring path = L"\\\\.\\pipe\\" + name;
    while (GetTickCount64() < deadline) {
        HANDLE pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            return pipe;
        if (WaitForMultipleObjects(2, waits, FALSE, 16) != WAIT_TIMEOUT)
            break;
    }
    return nullptr;
}

bool write_vm_pipe(HANDLE pipe, HANDLE cancel, const char* bytes, DWORD length)
{
    if (!pipe || !length)
        return false;
    OVERLAPPED io{};
    io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!io.hEvent)
        return false;
    DWORD written = 0;
    bool success = WriteFile(pipe, bytes, length, &written, &io);
    if (!success && GetLastError() == ERROR_IO_PENDING) {
        const HANDLE waits[] = {io.hEvent, cancel};
        if (WaitForMultipleObjects(2, waits, FALSE, 1500) == WAIT_OBJECT_0) {
            success = GetOverlappedResult(pipe, &io, &written, FALSE);
        } else {
            CancelIoEx(pipe, &io);
            GetOverlappedResult(pipe, &io, &written, TRUE);
        }
    }
    CloseHandle(io.hEvent);
    return success && written == length;
}

namespace {
bool qmp_command(HANDLE pipe, HANDLE cancel, const char* request, DWORD length)
{
    if (!write_vm_pipe(pipe, cancel, request, length)) return false;
    OVERLAPPED io{};
    io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!io.hEvent) return false;

    const auto deadline = GetTickCount64() + 5000;
    std::string received;
    bool accepted = false;
    while (GetTickCount64() < deadline && received.size() < 16 * 1024) {
        char packet[1024];
        DWORD count = 0;
        ResetEvent(io.hEvent);
        bool success = ReadFile(pipe, packet, sizeof(packet), &count, &io);
        if (!success && GetLastError() == ERROR_IO_PENDING) {
            const HANDLE waits[] = {io.hEvent, cancel};
            const auto now = GetTickCount64();
            const DWORD timeout = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
            if (WaitForMultipleObjects(2, waits, FALSE, timeout) == WAIT_OBJECT_0) {
                success = GetOverlappedResult(pipe, &io, &count, FALSE);
            } else {
                CancelIoEx(pipe, &io);
                GetOverlappedResult(pipe, &io, &count, TRUE);
            }
        }
        if (!success || !count) break;
        received.append(packet, count);
        size_t end;
        while ((end = received.find('\n')) != std::string::npos) {
            const std::string_view line(received.data(), end);
            if (line.find("\"id\"") != std::string_view::npos) {
                accepted = line.find("\"return\"") != std::string_view::npos;
                CloseHandle(io.hEvent);
                return accepted;
            }
            received.erase(0, end + 1);
        }
    }
    CloseHandle(io.hEvent);
    return false;
}
}

bool initialize_qmp(HANDLE pipe, HANDLE cancel)
{
    const char request[] = "{\"execute\":\"qmp_capabilities\",\"id\":1}\r\n";
    return qmp_command(pipe, cancel, request, sizeof(request) - 1);
}

bool request_powerdown(HANDLE pipe, HANDLE cancel)
{
    const char request[] = "{\"execute\":\"system_powerdown\",\"id\":2}\r\n";
    return qmp_command(pipe, cancel, request, sizeof(request) - 1);
}
