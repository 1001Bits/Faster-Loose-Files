#pragma once

// Faster reads of loose files (textures in Data\Textures, not in an archive).
//
// The game reads a loose texture through its file streamer: one background thread asks for
// 64 KB at a time, each with an OVERLAPPED, from a file opened without Windows' file cache
// (BSSystemFile::DoOpen 0x68000000, then DoReadAt: ReadFile, GetOverlappedResult). The same on
// Fallout 4 1.10.163, 1.11.x and VR 1.2.72.
//
// Here the first piece's read asks for the rest of the file as well (up to iMaxReadKB, 1 MB, at a time), and
// the next pieces come from memory; and loose textures can go through Windows' file cache, so a
// texture read once is in memory the next time. The game sees the same bytes either way.
//
// Everything works on the game executable's imports of CreateFileA/W, ReadFile,
// GetOverlappedResult and CloseHandle. Only files with a listed extension opened for reading are
// touched, and only reads of one aligned 64 KB piece with an OVERLAPPED; any other call, and
// anything unexpected, goes on unchanged. Other plugins' own file access is never seen.
// The counters run in every mode, so "off" measures the game's own reads.
// Game-independent: the tests run the same code.

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <Windows.h>

namespace LooseReads
{
struct Config
{
    bool wholeFiles{ true };     // read the rest of the file at its first piece
    bool windowsCache{ true };   // open without FILE_FLAG_NO_BUFFERING
    bool verify{ false };        // read each piece handed out again the game's way and compare
    unsigned maxReadKB{ 1024 };  // the largest single whole-file read, 128 KB to 4 MB (1 MB: same gain, shorter longest wait)
    std::vector<std::string> extensions{ "dds", "nif", "bgsm", "bgem" };  // lower case, without the dot
};

struct FlagCount
{
    DWORD flags;
    std::uint64_t opens;
};

struct Stats
{
    std::uint64_t opens{ 0 };         // files with a listed extension opened for reading
    std::uint64_t cachedOpens{ 0 };   // ... of them switched to the Windows cache
    std::uint64_t reads{ 0 };         // reads of those files that finished
    std::uint64_t readBytes{ 0 };     // ... bytes the game got
    std::uint64_t readTicks{ 0 };     // ... time the reading thread waited (inside ReadFile and GetOverlappedResult)
    std::uint64_t longestTicks{ 0 };  // ... the longest single wait
    std::uint64_t pieces{ 0 };        // 64 KB pieces among those reads
    std::uint64_t served{ 0 };        // ... answered from a whole-file read
    std::uint64_t diskReads{ 0 };     // whole-file reads made
    std::uint64_t diskBytes{ 0 };     // ... and what they fetched
    std::uint64_t diskTicks{ 0 };     // ... and how long they took
    std::uint64_t unusedBytes{ 0 };   // fetched by them, never asked for (counted when the memory is reused)
    std::uint64_t passed{ 0 };        // pieces that went to Windows unchanged (rest of file one piece, unaligned, busy)
    std::uint64_t errors{ 0 };        // reads that failed, other than at the end of a file
    std::uint64_t verified{ 0 };      // pieces compared (bVerifyReads)
    std::uint64_t mismatches{ 0 };    // ... that differed: must stay 0
    std::uint64_t untimed{ 0 };       // reads that were pending and never seen to finish
    std::vector<FlagCount> openFlags;  // the flags the game asked for, by value
};

// Before Install. Can be called again later only to switch modes in the tests.
void Configure(const Config& a_config);

// One loose file, when the game closed it: how long from its open until its last byte had arrived. Testing only.
struct FileTiming
{
    std::string name;
    std::uint64_t openTick{ 0 };  // GetTickCount64 at the open
    double firstReadMs{ -1 };     // from the open until the first read began; -1: never read
    double lastDoneMs{ -1 };      // from the open until the last read finished
    std::uint64_t bytes{ 0 };     // what the game got
    std::uint64_t size{ 0 };
    std::uint32_t reads{ 0 };
};
using FileSink = void (*)(const FileTiming&);

// Every closed listed file is handed to a_sink (on the thread that closed it); nullptr stops it. Before Install.
void SetFileSink(FileSink a_sink);

// Patches the running executable's imports: call before the game opens loose files.
// Empty on success, else why nothing was changed.
[[nodiscard]] std::string Install();

// One line per import: whose function it replaced (chained), or that it is missing.
[[nodiscard]] std::vector<std::string> InstallReport();

// Empty while every patched import still leads to this plugin first; else which one another module took.
[[nodiscard]] std::vector<std::string> Check();

// a_atExit: never waits for a lock (another thread may have been stopped holding it); the flags may then be missing.
[[nodiscard]] Stats GetStats(bool a_atExit = false);
[[nodiscard]] double TicksToMs(std::uint64_t a_ticks) noexcept;

// Whether a file name ends with a listed extension (exposed for the tests).
[[nodiscard]] bool Listed(const char* a_name) noexcept;
[[nodiscard]] bool Listed(const wchar_t* a_name) noexcept;

// What the imports are replaced with (exposed for the tests).
HANDLE WINAPI OnCreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
HANDLE WINAPI OnCreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
BOOL WINAPI OnReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL WINAPI OnGetOverlappedResult(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
BOOL WINAPI OnCloseHandle(HANDLE);
}  // namespace LooseReads
