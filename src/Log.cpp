#include "Log.h"

#include <Windows.h>

#include <cstdio>

namespace Log
{
namespace
{
HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;

std::string Line(std::string_view a_level, std::string_view a_text)
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char head[64];
    std::snprintf(head, sizeof(head), "[%02u:%02u:%02u.%03u] [%lu] [%.*s] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        GetCurrentThreadId(), static_cast<int>(a_level.size()), a_level.data());
    std::string line(head);
    line.append(a_text);
    line.append("\r\n");
    return line;
}

void Put(const std::string& a_line) noexcept
{
    DWORD written = 0;
    WriteFile(g_file, a_line.data(), static_cast<DWORD>(a_line.size()), &written, nullptr);
}
}  // namespace

void Open(const std::filesystem::path& a_path)
{
    std::error_code ec;
    std::filesystem::create_directories(a_path.parent_path(), ec);
    const auto previous = a_path.parent_path() / (a_path.stem().wstring() + L"-previous" + a_path.extension().wstring());
    if (std::filesystem::exists(a_path, ec))
        std::filesystem::rename(a_path, previous, ec);
    g_file = CreateFileW(a_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Write(std::string_view a_level, std::string_view a_text) noexcept
{
    if (g_file == INVALID_HANDLE_VALUE)
        return;
    try {
        const auto line = Line(a_level, a_text);
        AcquireSRWLockExclusive(&g_lock);
        Put(line);
        ReleaseSRWLockExclusive(&g_lock);
    } catch (...) {
    }
}

void WriteIfFree(std::string_view a_level, std::string_view a_text) noexcept
{
    if (g_file == INVALID_HANDLE_VALUE)
        return;
    try {
        const auto line = Line(a_level, a_text);
        if (!TryAcquireSRWLockExclusive(&g_lock))
            return;
        Put(line);
        ReleaseSRWLockExclusive(&g_lock);
    } catch (...) {
    }
}
}  // namespace Log
