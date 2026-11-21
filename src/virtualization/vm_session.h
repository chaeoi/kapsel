#pragma once

#include <windows.h>
#include <string>

enum class VmMode { Builtin, Install, Disk };
enum class VmState { Preparing, Starting, Connected, Graphics, Stopping, Stopped };

struct VmOptions {
    VmMode mode = VmMode::Builtin;
    unsigned cpus = 1;
    unsigned memory_mib = 512;
    unsigned disk_gib = 32;
    bool whpx = false;
    std::wstring iso;
};

inline constexpr UINT VM_MESSAGE = WM_APP + 1;

class VmSession {
public:
    VmSession(HWND window, const std::wstring& directory);
    ~VmSession();
    bool start(VmOptions options);
    void send(const char* bytes, size_t size);
    void shutdown(bool force);
    std::string receive_output();
    bool active() const;
    std::wstring log_path() const;

    VmSession(const VmSession&) = delete;
    VmSession& operator=(const VmSession&) = delete;

private:
    static DWORD WINAPI worker_entry(void* context);
    DWORD worker();
    void state(VmState value, LPARAM detail = 0);
    void post_output(const char* bytes, DWORD size);

    const HWND window_;
    const std::wstring directory_;
    VmOptions options_;
    HANDLE thread_ = nullptr;
    HANDLE command_ = nullptr;
    HANDLE abort_ = nullptr;
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::string outgoing_;
    std::string incoming_;
    bool poweroff_requested_ = false;
    bool output_posted_ = false;
};
