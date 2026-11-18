// Another plugin reading the game's loose files on its own threads through its own imports, as Creation Engine Tweaks' parallel
// streamer reads do (cf_readstats::RealRead): an OVERLAPPED with a per-thread event, ReadFile, and GetOverlappedResult when pending.

#include <cstdint>

#include <Windows.h>

// The bytes read, or -1 on an error other than the end of the file.
extern "C" __declspec(dllexport) long long OtherReadAt(HANDLE a_file, void* a_to, DWORD a_size, std::uint64_t a_at)
{
    thread_local const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED o{};
    o.Offset = static_cast<DWORD>(a_at);
    o.OffsetHigh = static_cast<DWORD>(a_at >> 32);
    o.hEvent = event;
    DWORD got = 0;
    if (!ReadFile(a_file, a_to, a_size, &got, &o)) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            if (GetOverlappedResult(a_file, &o, &got, TRUE))
                return got;
            error = GetLastError();
        }
        return error == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return got;
}

// The same imports with the caller's own OVERLAPPED, to see what a read leaves in it.
extern "C" __declspec(dllexport) BOOL OtherReadRaw(HANDLE a_file, void* a_to, DWORD a_size, DWORD* a_read, OVERLAPPED* a_overlapped)
{
    return ReadFile(a_file, a_to, a_size, a_read, a_overlapped);
}

extern "C" __declspec(dllexport) BOOL OtherWait(HANDLE a_file, OVERLAPPED* a_overlapped, DWORD* a_read)
{
    return GetOverlappedResult(a_file, a_overlapped, a_read, TRUE);
}
