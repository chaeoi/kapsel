#pragma once

#include <windows.h>
#include <string>

HANDLE connect_vm_pipe(const std::wstring& name, HANDLE process, HANDLE cancel);
bool write_vm_pipe(HANDLE pipe, HANDLE cancel, const char* bytes, DWORD length);
bool initialize_qmp(HANDLE pipe, HANDLE cancel);
bool request_powerdown(HANDLE pipe, HANDLE cancel);
