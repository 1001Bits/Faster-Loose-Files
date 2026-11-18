#include "LooseReads.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>

namespace LooseReads
{
namespace
{
constexpr std::uint64_t kPiece = 64 * 1024;
constexpr DWORD kMaxRead = 4u << 20;  // one read at most: a larger file takes several
std::atomic<DWORD> g_maxRead{ kMaxRead };  // [Debug] iMaxReadKB: a smaller cap, whole pieces
constexpr std::size_t kMaxPending = 4096;

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using GetOverlappedResultFn = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
using CloseHandleFn = BOOL(WINAPI*)(HANDLE);

// What each import led to before: Windows, or another plugin's patch, which then still sees every call.
CreateFileAFn g_createFileA = &::CreateFileA;
CreateFileWFn g_createFileW = &::CreateFileW;
ReadFileFn g_readFile = &::ReadFile;
GetOverlappedResultFn g_getOverlappedResult = &::GetOverlappedResult;
CloseHandleFn g_closeHandle = &::CloseHandle;

std::atomic<bool> g_whole{ true };
std::atomic<bool> g_cache{ true };
std::atomic<bool> g_verify{ false };
std::vector<std::string> g_extensions{ "dds" };

using Counter = std::atomic<std::uint64_t>;
Counter g_opens{ 0 }, g_cachedOpens{ 0 }, g_reads{ 0 }, g_readBytes{ 0 }, g_readTicks{ 0 }, g_longest{ 0 }, g_pieces{ 0 }, g_served{ 0 },
    g_diskReads{ 0 }, g_diskBytes{ 0 }, g_diskTicks{ 0 }, g_unused{ 0 }, g_passed{ 0 }, g_errors{ 0 }, g_verified{ 0 }, g_mismatches{ 0 },
    g_untimed{ 0 }, g_otherReads{ 0 }, g_timedOut{ 0 };

void Add(Counter& a_counter, std::uint64_t a_value) noexcept { a_counter.fetch_add(a_value, std::memory_order_relaxed); }

void Max(Counter& a_counter, std::uint64_t a_value) noexcept
{
    auto old = a_counter.load(std::memory_order_relaxed);
    while (a_value > old && !a_counter.compare_exchange_weak(old, a_value, std::memory_order_relaxed)) {
    }
}

std::mutex g_flagsMtx;
std::vector<FlagCount> g_flags;
std::atomic<FileSink> g_fileSink{ nullptr };

LONGLONG Now() noexcept
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// SetFileSink: one file's times, shared by every copy of its Loose.
struct Track
{
    std::string name;
    std::uint64_t openTick{ 0 };
    LONGLONG openQpc{ 0 };
    std::atomic<LONGLONG> firstStart{ 0 }, lastDone{ 0 };
    std::atomic<std::uint64_t> bytes{ 0 };
    std::atomic<std::uint32_t> reads{ 0 };

    void Start(LONGLONG a_at) noexcept
    {
        LONGLONG none = 0;
        firstStart.compare_exchange_strong(none, a_at, std::memory_order_relaxed);
    }
    void Done(LONGLONG a_at, std::uint64_t a_bytes) noexcept
    {
        auto old = lastDone.load(std::memory_order_relaxed);
        while (a_at > old && !lastDone.compare_exchange_weak(old, a_at, std::memory_order_relaxed)) {
        }
        bytes.fetch_add(a_bytes, std::memory_order_relaxed);
        reads.fetch_add(1, std::memory_order_relaxed);
    }
};

// One open of a listed file.
struct Loose
{
    std::uint64_t id{ 0 };  // never used twice, unlike a handle's value
    std::shared_ptr<Track> track;  // only with a file sink
    std::uint64_t size{ 0 };
    bool overlapped{ false };  // the handle was opened for overlapped reads
    // Pieces are handed out only where that leaves the handle exactly as Windows would: overlapped handles, and synchronous
    // ones with the Windows cache. A synchronous handle without it leaves its file pointer differently after a short read at
    // the end (seen in the tests); the loose file streamer never opens one (BSSystemFile::DoOpen gives 0x28000000 only
    // without its async bit, and OpenFile sets that bit whenever it sets the other).
    bool serve{ false };
};

std::shared_mutex g_openMtx;
std::unordered_map<HANDLE, Loose> g_open;
std::atomic<std::size_t> g_openCount{ 0 };  // lets every other file's calls skip the lock
std::atomic<std::uint64_t> g_nextId{ 1 };

bool Find(HANDLE a_file, Loose& a_out) noexcept
{
    if (!g_openCount.load(std::memory_order_relaxed))
        return false;
    std::shared_lock lk(g_openMtx);
    const auto it = g_open.find(a_file);
    if (it == g_open.end())
        return false;
    a_out = it->second;
    return true;
}

// A read the game left pending: timed until GetOverlappedResult reports it done.
struct Pending
{
    HANDLE file;
    std::uint64_t ticks;  // waited so far: inside ReadFile, then inside GetOverlappedResult
    bool piece;
    std::shared_ptr<Track> track;
    bool onHandle;  // its OVERLAPPED has no event: whoever waits for it waits on the handle's own signal
};

std::mutex g_pendingMtx;
std::unordered_map<LPOVERLAPPED, Pending> g_pending;
std::atomic<std::size_t> g_pendingCount{ 0 };

// Whether a read is in flight on this handle (or about to be) that will be waited for on the handle itself. A read of ours on
// it would clear the handle's own signal, which GetOverlappedResult waits for when the OVERLAPPED has no event: that read could
// then finish unseen and its thread wait for ever. So no whole-file read on such a handle. Reads with their own event (Creation
// Engine Tweaks' parallel readers) are not at risk and do not hold whole-file reads back.
// Not atomic with the whole-file read that follows: a read waiting on the handle that starts on another thread in between is not
// seen. The game makes those reads from one streamer thread at a time per file (and Creation Engine Tweaks' readers use events).
bool InFlight(HANDLE a_file) noexcept
{
    if (!g_pendingCount.load(std::memory_order_relaxed))
        return false;
    std::lock_guard lk(g_pendingMtx);
    for (const auto& [overlapped, p] : g_pending)
        if (p.file == a_file && p.onHandle)
            return true;
    return false;
}

void Finished(std::uint64_t a_bytes, std::uint64_t a_ticks, bool a_failed, bool a_piece, Track* a_track) noexcept
{
    if (a_track)
        a_track->Done(Now(), a_bytes);
    Add(g_reads, 1);
    Add(g_readBytes, a_bytes);
    Add(g_readTicks, a_ticks);
    Max(g_longest, a_ticks);
    if (a_failed)
        Add(g_errors, 1);
    if (a_piece)
        Add(g_pieces, 1);
}

// Set while this thread makes a read of our own: should a plugin chained before us call back into a patched import, that call
// goes straight on (no tracking, no serving, no waiting for a slot this thread itself holds).
thread_local bool t_ours = false;

struct Ours
{
    bool was;
    Ours() noexcept : was(t_ours) { t_ours = true; }
    ~Ours() { t_ours = was; }
    Ours(const Ours&) = delete;
    Ours& operator=(const Ours&) = delete;
};

// One read from the game's handle, without the game's event or OVERLAPPED. Returns the bytes read.
DWORD ReadAt(HANDLE a_file, void* a_to, std::uint64_t a_at, DWORD a_size, HANDLE a_event) noexcept
{
    const Ours ours;
    OVERLAPPED o{};
    o.Offset = static_cast<DWORD>(a_at);
    o.OffsetHigh = static_cast<DWORD>(a_at >> 32);
    // The low bit: no completion packet, should the file ever be tied to a completion port.
    o.hEvent = reinterpret_cast<HANDLE>(reinterpret_cast<std::uintptr_t>(a_event) | 1);
    DWORD got = 0;
    if (!g_readFile(a_file, a_to, a_size, &got, &o)) {
        if (GetLastError() != ERROR_IO_PENDING)
            return 0;
        if (!g_getOverlappedResult(a_file, &o, &got, TRUE))
            return GetLastError() == ERROR_HANDLE_EOF ? got : 0;
    }
    return got;
}

struct Pages
{
    void* p{ nullptr };
    explicit Pages(std::size_t a_size) noexcept : p(VirtualAlloc(nullptr, a_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {}
    ~Pages()
    {
        if (p)
            VirtualFree(p, 0, MEM_RELEASE);
    }
    Pages(const Pages&) = delete;
    Pages& operator=(const Pages&) = delete;
};

// The last whole-file reads. Several, so that files read in turns do not take each other's away.
struct Slot
{
    std::timed_mutex mtx;
    std::uint8_t* data{ nullptr };
    HANDLE event{ nullptr };
    std::uint64_t file{ 0 };  // Loose::id; 0 = empty
    std::uint64_t start{ 0 };
    std::uint64_t end{ 0 };
    std::uint64_t handed{ 0 };  // which pieces the game got, one bit each (64 pieces of 64 KB = 4 MB)
    // What it holds or is reading in right now, readable without the lock. Set under the lock; may be seen half changed, so
    // whatever it promises is checked again under the lock.
    std::atomic<std::uint64_t> claimFile{ 0 };
    std::atomic<std::uint64_t> claimStart{ 0 };
    std::atomic<std::uint64_t> claimEnd{ 0 };
};
Slot g_slots[4];
std::atomic<unsigned> g_nextSlot{ 0 };
// Taken to decide on a new whole-file read, never while waiting for a slot: two threads asking for pieces of one file at the same
// moment (Creation Engine Tweaks' parallel readers) start one read of it, not two.
std::mutex g_claimMtx;

bool Claims(const Slot& a_slot, std::uint64_t a_file, std::uint64_t a_at) noexcept
{
    return a_slot.claimFile.load(std::memory_order_acquire) == a_file && a_at >= a_slot.claimStart.load(std::memory_order_relaxed) &&
           a_at < a_slot.claimEnd.load(std::memory_order_relaxed);
}
static_assert(kMaxRead / kPiece <= 64);

// What a slot fetched that the game never asked for, before the slot is used again.
void Retire(Slot& a_slot) noexcept
{
    if (!a_slot.file)
        return;
    std::uint64_t unused = 0;
    for (std::uint64_t at = a_slot.start, i = 0; at < a_slot.end; at += kPiece, ++i)
        if (!(a_slot.handed & (1ull << i)))
            unused += (std::min)(kPiece, a_slot.end - at);
    Add(g_unused, unused);
    a_slot.file = 0;
    a_slot.claimFile.store(0, std::memory_order_release);
}

BOOL Served(LPDWORD a_read, LPOVERLAPPED a_overlapped, DWORD a_bytes) noexcept
{
    // As Windows leaves them after a read that finished at once.
    a_overlapped->Internal = 0;
    a_overlapped->InternalHigh = a_bytes;
    if (a_overlapped->hEvent)
        SetEvent(reinterpret_cast<HANDLE>(reinterpret_cast<std::uintptr_t>(a_overlapped->hEvent) & ~std::uintptr_t{ 1 }));
    *a_read = a_bytes;
    Add(g_served, 1);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

// The longest wait for a whole-file read another thread is making. Longer than such a read takes even on a slow disk; should
// that thread itself wait for this one (another plugin's worker threads chained in the imports), the caller reads its own piece.
constexpr auto kSlotWait = std::chrono::milliseconds(250);

enum class Got
{
    no,    // no slot has it
    yes,   // copied
    busy,  // a slot is reading it in, and did not finish in kSlotWait
};

// The piece at a_at from a slot that holds it; a slot still reading it in is waited for, up to kSlotWait.
Got FromSlot(const Loose& a_file, std::uint64_t a_at, void* a_to, DWORD& a_bytes) noexcept
{
    for (auto& slot : g_slots) {
        if (!Claims(slot, a_file.id, a_at))
            continue;
        std::unique_lock lk(slot.mtx, std::defer_lock);
        try {
            if (!lk.try_lock_for(kSlotWait)) {
                Add(g_timedOut, 1);
                return Got::busy;
            }
        } catch (...) {
            return Got::busy;
        }
        if (slot.file != a_file.id || a_at < slot.start || a_at >= slot.end)
            continue;
        const auto bytes = (std::min)(kPiece, slot.end - a_at);
        if (bytes < kPiece && slot.end < a_file.size)
            continue;  // the slot stops inside this piece: a short read
        std::memcpy(a_to, slot.data + (a_at - slot.start), bytes);
        slot.handed |= 1ull << ((a_at - slot.start) / kPiece);
        a_bytes = static_cast<DWORD>(bytes);
        return Got::yes;
    }
    return Got::no;
}

// The piece at a_at, from a slot or from a new read of the rest of the file. False: the game's own read goes ahead.
bool Serve(const Loose& a_file, HANDLE a_handle, void* a_to, std::uint64_t a_at, DWORD& a_bytes) noexcept
{
    if (a_at % kPiece || a_at >= a_file.size) {
        Add(g_passed, 1);  // at or past the end, the game's own read reports it
        return false;
    }
    if (const auto got = FromSlot(a_file, a_at, a_to, a_bytes); got != Got::no) {
        if (got == Got::busy)
            Add(g_passed, 1);
        return got == Got::yes;
    }
    const auto left = a_file.size - a_at;
    if (left <= kPiece || InFlight(a_handle)) {
        Add(g_passed, 1);  // the last piece: the same read as the game's own
        return false;
    }
    const auto want = static_cast<DWORD>((std::min)((left + kPiece - 1) / kPiece * kPiece, std::uint64_t{ g_maxRead.load(std::memory_order_relaxed) }));
    Slot* mine = nullptr;
    std::unique_lock<std::timed_mutex> lk;
    bool claimed = false;  // by another thread since the look above
    try {
        std::lock_guard claim(g_claimMtx);
        for (const auto& slot : g_slots)
            claimed = claimed || Claims(slot, a_file.id, a_at);
        const auto first = claimed ? 0 : g_nextSlot.fetch_add(1, std::memory_order_relaxed);
        for (unsigned i = 0; !claimed && i < std::size(g_slots); ++i) {
            auto& slot = g_slots[(first + i) % std::size(g_slots)];
            std::unique_lock l(slot.mtx, std::try_to_lock);
            if (!l)
                continue;
            Retire(slot);
            if (!slot.data) {
                // Page-aligned, as reads without the Windows cache need.
                slot.data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kMaxRead, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (!slot.event)
                    slot.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!slot.data || !slot.event)
                    break;
            }
            slot.claimStart.store(a_at, std::memory_order_relaxed);
            slot.claimEnd.store(a_at + want, std::memory_order_relaxed);
            slot.claimFile.store(a_file.id, std::memory_order_release);
            mine = &slot;
            lk = std::move(l);
            break;
        }
    } catch (...) {
        mine = nullptr;  // a lock that could not be taken: the game's own read
    }
    if (!mine) {
        if (claimed && FromSlot(a_file, a_at, a_to, a_bytes) == Got::yes)
            return true;
        Add(g_passed, 1);
        return false;
    }
    auto& slot = *mine;
    const auto t0 = Now();
    const DWORD got = ReadAt(a_handle, slot.data, a_at, want, slot.event);
    Add(g_diskTicks, static_cast<std::uint64_t>(Now() - t0));
    if (got < (std::min)(kPiece, left)) {
        slot.claimFile.store(0, std::memory_order_release);
        Add(g_passed, 1);
        return false;  // the game's own read meets the same problem and reports it
    }
    slot.file = a_file.id;
    slot.start = a_at;
    slot.end = a_at + got;
    slot.handed = 1;
    slot.claimEnd.store(a_at + got, std::memory_order_relaxed);
    Add(g_diskReads, 1);
    Add(g_diskBytes, got);
    const auto bytes = (std::min)(kPiece, std::uint64_t{ got });
    std::memcpy(a_to, slot.data, bytes);
    a_bytes = static_cast<DWORD>(bytes);
    return true;
}

// bVerifyReads: the same piece read again the game's way. Never changes what the game got.
void Verify(HANDLE a_handle, const void* a_got, std::uint64_t a_at, DWORD a_bytes) noexcept
{
    thread_local Pages buffer(kPiece);
    thread_local HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!buffer.p || !event || InFlight(a_handle))
        return;
    const DWORD again = ReadAt(a_handle, buffer.p, a_at, static_cast<DWORD>(kPiece), event);
    Add(g_verified, 1);
    if (again != a_bytes || std::memcmp(buffer.p, a_got, a_bytes) != 0)
        Add(g_mismatches, 1);
}

bool Ends(std::string_view a_name, const std::string& a_ext) noexcept
{
    if (a_name.size() < a_ext.size() + 1 || a_name[a_name.size() - a_ext.size() - 1] != '.')
        return false;
    const auto tail = a_name.substr(a_name.size() - a_ext.size());
    for (std::size_t i = 0; i < tail.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(tail[i])) != a_ext[i])
            return false;
    return true;
}

bool Ends(std::wstring_view a_name, const std::string& a_ext) noexcept
{
    if (a_name.size() < a_ext.size() + 1 || a_name[a_name.size() - a_ext.size() - 1] != L'.')
        return false;
    const auto tail = a_name.substr(a_name.size() - a_ext.size());
    for (std::size_t i = 0; i < tail.size(); ++i)
        if (std::towlower(tail[i]) != static_cast<wchar_t>(static_cast<unsigned char>(a_ext[i])))
            return false;
    return true;
}

// Only a file opened to be read as it is.
// Only a file opened to be read as it is, that nobody may write while it is open (the game shares its loose files for reading only).
bool Tracked(DWORD a_access, DWORD a_share, DWORD a_disposition) noexcept
{
    return (a_access & GENERIC_READ) && !(a_access & (GENERIC_WRITE | DELETE | FILE_WRITE_DATA | FILE_APPEND_DATA)) &&
           !(a_share & FILE_SHARE_WRITE) && a_disposition == OPEN_EXISTING;
}

// No buffering is the game's setting for loose files its streamer reads. Sequential mode would drop pages it
// has just read: random access keeps them, and a whole-file read needs no read-ahead.
DWORD CacheFlags(DWORD a_flags) noexcept
{
    if (!g_cache.load(std::memory_order_relaxed) || !(a_flags & FILE_FLAG_NO_BUFFERING))
        return a_flags;
    return (a_flags & ~(FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN)) | FILE_FLAG_RANDOM_ACCESS;
}

template <class Char>
void Register(HANDLE a_file, DWORD a_asked, DWORD a_used, const Char* a_name) noexcept
{
    const auto error = GetLastError();  // a successful open can still say something (ERROR_ALREADY_EXISTS)
    try {
        Loose loose;
        loose.id = g_nextId.fetch_add(1, std::memory_order_relaxed);
        if (g_fileSink.load(std::memory_order_relaxed)) {
            loose.track = std::make_shared<Track>();
            for (const Char* c = a_name; *c; ++c)
                loose.track->name += *c >= 32 && *c < 127 ? static_cast<char>(*c) : '?';
            loose.track->openTick = GetTickCount64();
            loose.track->openQpc = Now();
        }
        LARGE_INTEGER size{};
        loose.size = GetFileSizeEx(a_file, &size) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
        loose.overlapped = (a_used & FILE_FLAG_OVERLAPPED) != 0;
        loose.serve = loose.overlapped || !(a_used & FILE_FLAG_NO_BUFFERING);
        {
            std::unique_lock lk(g_openMtx);
            g_open[a_file] = loose;
            g_openCount.store(g_open.size(), std::memory_order_relaxed);
        }
        Add(g_opens, 1);
        if (a_used != a_asked)
            Add(g_cachedOpens, 1);
        std::lock_guard lk(g_flagsMtx);
        const auto it = std::find_if(g_flags.begin(), g_flags.end(), [&](const FlagCount& a_f) { return a_f.flags == a_asked; });
        if (it != g_flags.end())
            ++it->opens;
        else if (g_flags.size() < 32)
            g_flags.push_back({ a_asked, 1 });
    } catch (...) {
    }
    SetLastError(error);
}

// All the imports or none: without CloseHandle a reused handle could be taken for the old file.
struct Import
{
    const char* name;
    void* hook;
    void** original;
    bool required;
    ULONGLONG* slot{ nullptr };
    std::string before;  // whose function the slot held
};

// Patched in this order: a file is only tracked (CreateFile) once its reads and its close are seen.
Import g_imports[] = {
    { "CloseHandle", reinterpret_cast<void*>(&OnCloseHandle), reinterpret_cast<void**>(&g_closeHandle), true },
    { "GetOverlappedResult", reinterpret_cast<void*>(&OnGetOverlappedResult), reinterpret_cast<void**>(&g_getOverlappedResult), false },
    { "ReadFile", reinterpret_cast<void*>(&OnReadFile), reinterpret_cast<void**>(&g_readFile), true },
    { "CreateFileW", reinterpret_cast<void*>(&OnCreateFileW), reinterpret_cast<void**>(&g_createFileW), false },
    { "CreateFileA", reinterpret_cast<void*>(&OnCreateFileA), reinterpret_cast<void**>(&g_createFileA), true },
};

// One import slot changed, keeping the page's execute right should it have one. False: left as it was.
bool WriteSlot(ULONGLONG* a_slot, ULONGLONG a_value) noexcept
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(a_slot, &mbi, sizeof(mbi)))
        return false;
    const bool exec = mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    DWORD old = 0;
    if (!VirtualProtect(a_slot, sizeof(*a_slot), exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old))
        return false;
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(a_slot), static_cast<LONG64>(a_value));
    VirtualProtect(a_slot, sizeof(*a_slot), old, &old);
    return true;
}
bool g_installed = false;

std::string ModuleOf(const void* a_address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(a_address), &module) || !module)
        return std::format("unknown code at {}", a_address);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring_view name(path);
    if (const auto slash = name.find_last_of(L"\\/"); slash != std::wstring_view::npos)
        name.remove_prefix(slash + 1);
    std::string out;
    for (const auto c : name)
        out += c < 128 ? static_cast<char>(c) : '?';
    return out;
}

bool FromWindows(const std::string& a_module)
{
    std::string m;
    for (const auto c : a_module)
        m += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return m == "kernel32.dll" || m == "kernelbase.dll" || m.starts_with("api-ms-win");
}

// The import slots of a_names in a_module (each left null when not imported).
void FindSlots(HMODULE a_module, const char* const* a_names, ULONGLONG** a_slots, std::size_t a_count)
{
    auto* base = reinterpret_cast<std::uint8_t*>(a_module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return;
    const HMODULE modules[2] = { GetModuleHandleW(L"kernel32.dll"), GetModuleHandleW(L"kernelbase.dll") };
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        auto* slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        const auto* name = imp->OriginalFirstThunk ? reinterpret_cast<const IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk) : nullptr;
        for (; slot->u1.Function; ++slot, name = name ? name + 1 : nullptr) {
            for (std::size_t i = 0; i < a_count; ++i) {
                if (a_slots[i])
                    continue;
                bool same = false;
                if (name && !IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal))
                    same = std::strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name->u1.AddressOfData)->Name, a_names[i]) == 0;
                else if (!name) {
                    // No names kept: recognised by address. Someone else's patch in this slot then goes unseen.
                    for (const auto module : modules)
                        same = same || (module && reinterpret_cast<ULONGLONG>(GetProcAddress(module, a_names[i])) == slot->u1.Function);
                }
                if (same)
                    a_slots[i] = &slot->u1.Function;
            }
        }
    }
}

void FindImports()
{
    const char* names[std::size(g_imports)];
    ULONGLONG* slots[std::size(g_imports)] = {};
    for (std::size_t i = 0; i < std::size(g_imports); ++i)
        names[i] = g_imports[i].name;
    FindSlots(GetModuleHandleW(nullptr), names, slots, std::size(g_imports));
    for (std::size_t i = 0; i < std::size(g_imports); ++i)
        g_imports[i].slot = slots[i];
}
}  // namespace

void SetFileSink(FileSink a_sink)
{
    g_fileSink.store(a_sink, std::memory_order_relaxed);
}

void Configure(const Config& a_config)
{
    g_whole.store(a_config.wholeFiles, std::memory_order_relaxed);
    g_cache.store(a_config.windowsCache, std::memory_order_relaxed);
    g_verify.store(a_config.verify, std::memory_order_relaxed);
    g_maxRead.store(static_cast<DWORD>((std::clamp)(a_config.maxReadKB, 128u, 4096u) / 64 * 64 * 1024), std::memory_order_relaxed);
    g_extensions.clear();
    for (auto ext : a_config.extensions) {
        std::string clean;
        for (const auto c : ext)
            if (c != '.' && c != ' ' && c != '\t')
                clean += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!clean.empty())
            g_extensions.push_back(clean);
    }
}

std::string Install()
{
    if (g_installed)
        return {};
    FindImports();
    for (const auto& i : g_imports) {
        if (i.required && !i.slot)
            return std::format("the game does not import {}", i.name);
    }
    // The originals first: the first call can come as soon as a slot changes.
    for (auto& i : g_imports) {
        if (!i.slot)
            continue;
        *i.original = reinterpret_cast<void*>(*i.slot);
        i.before = ModuleOf(reinterpret_cast<const void*>(*i.slot));
    }
    for (std::size_t n = 0; n < std::size(g_imports); ++n) {
        auto& i = g_imports[n];
        if (!i.slot)
            continue;
        if (!WriteSlot(i.slot, reinterpret_cast<ULONGLONG>(i.hook))) {
            // Those already changed get their old function back.
            for (std::size_t k = 0; k < n; ++k)
                if (g_imports[k].slot)
                    WriteSlot(g_imports[k].slot, reinterpret_cast<ULONGLONG>(*g_imports[k].original));
            return std::format("the import of {} could not be changed", i.name);
        }
    }
    g_installed = true;
    return {};
}

std::vector<std::string> InstallReport()
{
    std::vector<std::string> out;
    for (const auto& i : g_imports) {
        if (!i.slot)
            out.push_back(std::format("{}: not imported by the game", i.name));
        else if (FromWindows(i.before))
            out.push_back(std::format("{}: patched (was {})", i.name, i.before));
        else
            out.push_back(std::format("{}: patched, chained after {}", i.name, i.before));
    }
    return out;
}

Stats GetStats(bool a_atExit)
{
    Stats s;
    const auto get = [](const Counter& a_c) { return a_c.load(std::memory_order_relaxed); };
    s.opens = get(g_opens);
    s.cachedOpens = get(g_cachedOpens);
    s.reads = get(g_reads);
    s.readBytes = get(g_readBytes);
    s.readTicks = get(g_readTicks);
    s.longestTicks = get(g_longest);
    s.pieces = get(g_pieces);
    s.served = get(g_served);
    s.diskReads = get(g_diskReads);
    s.diskBytes = get(g_diskBytes);
    s.diskTicks = get(g_diskTicks);
    s.unusedBytes = get(g_unused);
    s.passed = get(g_passed);
    s.errors = get(g_errors);
    s.verified = get(g_verified);
    s.mismatches = get(g_mismatches);
    s.untimed = get(g_untimed);
    s.otherReads = get(g_otherReads);
    s.timedOut = get(g_timedOut);
    {
        std::unique_lock lk(g_flagsMtx, std::defer_lock);
        if (a_atExit ? lk.try_lock() : (lk.lock(), true))
            s.openFlags = g_flags;
    }
    return s;
}

double TicksToMs(std::uint64_t a_ticks) noexcept
{
    static const double perMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    return static_cast<double>(a_ticks) / perMs;
}

bool Listed(const char* a_name) noexcept
{
    if (!a_name)
        return false;
    const std::string_view name(a_name);
    for (const auto& ext : g_extensions)
        if (Ends(name, ext))
            return true;
    return false;
}

bool Listed(const wchar_t* a_name) noexcept
{
    if (!a_name)
        return false;
    const std::wstring_view name(a_name);
    for (const auto& ext : g_extensions)
        if (Ends(name, ext))
            return true;
    return false;
}

HANDLE WINAPI OnCreateFileA(LPCSTR a_name, DWORD a_access, DWORD a_share, LPSECURITY_ATTRIBUTES a_security, DWORD a_disposition, DWORD a_flags, HANDLE a_template)
{
    if (!Tracked(a_access, a_share, a_disposition) || !Listed(a_name))
        return g_createFileA(a_name, a_access, a_share, a_security, a_disposition, a_flags, a_template);
    const DWORD flags = CacheFlags(a_flags);
    const HANDLE file = g_createFileA(a_name, a_access, a_share, a_security, a_disposition, flags, a_template);
    if (file != INVALID_HANDLE_VALUE)
        Register(file, a_flags, flags, a_name);
    return file;
}

HANDLE WINAPI OnCreateFileW(LPCWSTR a_name, DWORD a_access, DWORD a_share, LPSECURITY_ATTRIBUTES a_security, DWORD a_disposition, DWORD a_flags, HANDLE a_template)
{
    if (!Tracked(a_access, a_share, a_disposition) || !Listed(a_name))
        return g_createFileW(a_name, a_access, a_share, a_security, a_disposition, a_flags, a_template);
    const DWORD flags = CacheFlags(a_flags);
    const HANDLE file = g_createFileW(a_name, a_access, a_share, a_security, a_disposition, flags, a_template);
    if (file != INVALID_HANDLE_VALUE)
        Register(file, a_flags, flags, a_name);
    return file;
}

namespace
{
// Every patched ReadFile import comes here, each with the function its own slot held before.
BOOL ReadImpl(ReadFileFn a_original, bool a_other, HANDLE a_file, LPVOID a_buffer, DWORD a_size, LPDWORD a_read, LPOVERLAPPED a_overlapped)
{
    Loose loose;
    if (t_ours || !Find(a_file, loose))
        return a_original(a_file, a_buffer, a_size, a_read, a_overlapped);
    if (a_other)
        Add(g_otherReads, 1);
    const auto t0 = Now();
    if (loose.track)
        loose.track->Start(t0);
    const bool piece = a_size == kPiece;
    if (piece && a_overlapped && a_read && a_buffer && loose.serve && g_whole.load(std::memory_order_relaxed)) {
        const std::uint64_t at = (static_cast<std::uint64_t>(a_overlapped->OffsetHigh) << 32) | a_overlapped->Offset;
        DWORD bytes = 0;
        if (Serve(loose, a_file, a_buffer, at, bytes)) {
            if (g_verify.load(std::memory_order_relaxed))
                Verify(a_file, a_buffer, at, bytes);
            if (!loose.overlapped) {
                // A read with an OVERLAPPED moves a synchronous handle's file pointer to its end.
                LARGE_INTEGER to;
                to.QuadPart = static_cast<LONGLONG>(at + bytes);
                SetFilePointerEx(a_file, to, nullptr, FILE_BEGIN);
            }
            Finished(bytes, static_cast<std::uint64_t>(Now() - t0), false, true, loose.track.get());
            return Served(a_read, a_overlapped, bytes);
        }
    }
    // Marked in flight before it starts: it can finish before ReadFile even returns.
    const bool marked = a_overlapped && loose.overlapped;
    if (marked) {
        try {
            std::lock_guard lk(g_pendingMtx);
            if (g_pending.size() >= kMaxPending) {
                // Reads the game finished some other way: never timed.
                Add(g_untimed, g_pending.size());
                g_pending.clear();
            }
            g_pending[a_overlapped] = { a_file, 0, piece, loose.track, a_overlapped->hEvent == nullptr };
            g_pendingCount.store(g_pending.size(), std::memory_order_relaxed);
        } catch (...) {
        }
    }
    const BOOL ok = a_original(a_file, a_buffer, a_size, a_read, a_overlapped);
    const DWORD error = GetLastError();
    const auto ticks = static_cast<std::uint64_t>(Now() - t0);
    if (!ok && error == ERROR_IO_PENDING && a_overlapped) {
        try {
            std::lock_guard lk(g_pendingMtx);
            if (const auto it = g_pending.find(a_overlapped); it != g_pending.end())
                it->second.ticks += ticks;
            else
                g_pending[a_overlapped] = { a_file, ticks, piece, loose.track, a_overlapped->hEvent == nullptr };
            g_pendingCount.store(g_pending.size(), std::memory_order_relaxed);
        } catch (...) {
        }
        SetLastError(error);
        return ok;
    }
    if (marked) {
        std::lock_guard lk(g_pendingMtx);
        g_pending.erase(a_overlapped);
        g_pendingCount.store(g_pending.size(), std::memory_order_relaxed);
    }
    Finished(ok && a_read ? *a_read : 0, ticks, !ok && error != ERROR_HANDLE_EOF, piece, loose.track.get());
    SetLastError(error);
    return ok;
}

BOOL GorImpl(GetOverlappedResultFn a_original, HANDLE a_file, LPOVERLAPPED a_overlapped, LPDWORD a_read, BOOL a_wait)
{
    if (t_ours || !a_overlapped || !g_pendingCount.load(std::memory_order_relaxed))
        return a_original(a_file, a_overlapped, a_read, a_wait);
    bool mine = false;
    {
        std::lock_guard lk(g_pendingMtx);
        mine = g_pending.contains(a_overlapped);
    }
    if (!mine)
        return a_original(a_file, a_overlapped, a_read, a_wait);
    const auto t0 = Now();
    const BOOL ok = a_original(a_file, a_overlapped, a_read, a_wait);
    const DWORD error = GetLastError();
    const auto ticks = static_cast<std::uint64_t>(Now() - t0);
    Pending done{};
    bool finished = false;
    {
        std::lock_guard lk(g_pendingMtx);
        if (const auto it = g_pending.find(a_overlapped); it != g_pending.end()) {
            it->second.ticks += ticks;
            if (ok || error != ERROR_IO_INCOMPLETE) {
                done = it->second;
                finished = true;
                g_pending.erase(it);
                g_pendingCount.store(g_pending.size(), std::memory_order_relaxed);
            }
        }
    }
    if (finished)
        Finished(ok && a_read ? *a_read : 0, done.ticks, !ok && error != ERROR_HANDLE_EOF, done.piece, done.track.get());
    SetLastError(error);
    return ok;
}

// Another module's own imports of ReadFile and GetOverlappedResult (Creation Engine Tweaks reads the game's loose files on its own
// threads through them). Each slot keeps the function it held before.
struct Other
{
    HMODULE module{ nullptr };
    std::string name;
    ReadFileFn readFile{ nullptr };
    GetOverlappedResultFn gor{ nullptr };
    ULONGLONG* readSlot{ nullptr };
    ULONGLONG* gorSlot{ nullptr };
    std::string readBefore, gorBefore;
};
constexpr std::size_t kOthers = 4;
Other g_others[kOthers];
std::mutex g_othersMtx;

template <std::size_t I>
BOOL WINAPI OnReadFileOther(HANDLE a_file, LPVOID a_buffer, DWORD a_size, LPDWORD a_read, LPOVERLAPPED a_overlapped)
{
    return ReadImpl(g_others[I].readFile, true, a_file, a_buffer, a_size, a_read, a_overlapped);
}

template <std::size_t I>
BOOL WINAPI OnGetOverlappedResultOther(HANDLE a_file, LPOVERLAPPED a_overlapped, LPDWORD a_read, BOOL a_wait)
{
    return GorImpl(g_others[I].gor, a_file, a_overlapped, a_read, a_wait);
}

constexpr ReadFileFn kOtherRead[kOthers] = { &OnReadFileOther<0>, &OnReadFileOther<1>, &OnReadFileOther<2>, &OnReadFileOther<3> };
constexpr GetOverlappedResultFn kOtherGor[kOthers] = { &OnGetOverlappedResultOther<0>, &OnGetOverlappedResultOther<1>,
    &OnGetOverlappedResultOther<2>, &OnGetOverlappedResultOther<3> };
}  // namespace

BOOL WINAPI OnReadFile(HANDLE a_file, LPVOID a_buffer, DWORD a_size, LPDWORD a_read, LPOVERLAPPED a_overlapped)
{
    return ReadImpl(g_readFile, false, a_file, a_buffer, a_size, a_read, a_overlapped);
}

BOOL WINAPI OnGetOverlappedResult(HANDLE a_file, LPOVERLAPPED a_overlapped, LPDWORD a_read, BOOL a_wait)
{
    return GorImpl(g_getOverlappedResult, a_file, a_overlapped, a_read, a_wait);
}

std::string HookModuleReads(HMODULE a_module)
{
    if (!g_installed)
        return "the game's own imports are not patched";
    if (!a_module || a_module == GetModuleHandleW(nullptr))
        return "not another module";
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&HookModuleReads), &self);
    if (a_module == self)
        return "not another module";
    std::lock_guard lk(g_othersMtx);
    std::size_t index = kOthers;
    for (std::size_t i = 0; i < kOthers; ++i) {
        if (g_others[i].module == a_module)
            return {};  // already done
        if (!g_others[i].module && index == kOthers)
            index = i;
    }
    if (index == kOthers)
        return "no free hook left";
    const char* names[2] = { "GetOverlappedResult", "ReadFile" };
    ULONGLONG* slots[2] = { nullptr, nullptr };
    FindSlots(a_module, names, slots, 2);
    if (!slots[1])
        return "it does not import ReadFile";
    auto& o = g_others[index];
    o.name = ModuleOf(reinterpret_cast<const void*>(a_module));
    o.gorSlot = slots[0];
    o.readSlot = slots[1];
    // The originals first: the first call can come as soon as a slot changes.
    o.gor = o.gorSlot ? reinterpret_cast<GetOverlappedResultFn>(*o.gorSlot) : g_getOverlappedResult;
    o.readFile = reinterpret_cast<ReadFileFn>(*o.readSlot);
    o.gorBefore = o.gorSlot ? ModuleOf(reinterpret_cast<const void*>(*o.gorSlot)) : std::string();
    o.readBefore = ModuleOf(reinterpret_cast<const void*>(*o.readSlot));
    // Completion first, then the reads: a read is never tracked whose completion would go unseen.
    if (o.gorSlot && !WriteSlot(o.gorSlot, reinterpret_cast<ULONGLONG>(kOtherGor[index])))
        return "its import of GetOverlappedResult could not be changed";
    if (!WriteSlot(o.readSlot, reinterpret_cast<ULONGLONG>(kOtherRead[index]))) {
        if (o.gorSlot)
            WriteSlot(o.gorSlot, reinterpret_cast<ULONGLONG>(o.gor));
        return "its import of ReadFile could not be changed";
    }
    o.module = a_module;
    return {};
}

std::vector<std::string> Check()
{
    std::vector<std::string> out;
    if (!g_installed)
        return out;
    for (const auto& i : g_imports) {
        if (!i.slot)
            continue;
        const auto now = *reinterpret_cast<volatile ULONGLONG*>(i.slot);
        if (now != reinterpret_cast<ULONGLONG>(i.hook))
            out.push_back(std::format("{} now goes to {} first", i.name, ModuleOf(reinterpret_cast<const void*>(now))));
    }
    std::lock_guard lk(g_othersMtx);
    for (std::size_t i = 0; i < kOthers; ++i) {
        const auto& o = g_others[i];
        if (!o.module)
            continue;
        if (const auto now = *reinterpret_cast<volatile ULONGLONG*>(o.readSlot); now != reinterpret_cast<ULONGLONG>(kOtherRead[i]))
            out.push_back(std::format("{}'s ReadFile now goes to {} first", o.name, ModuleOf(reinterpret_cast<const void*>(now))));
        if (o.gorSlot)
            if (const auto now = *reinterpret_cast<volatile ULONGLONG*>(o.gorSlot); now != reinterpret_cast<ULONGLONG>(kOtherGor[i]))
                out.push_back(std::format("{}'s GetOverlappedResult now goes to {} first", o.name, ModuleOf(reinterpret_cast<const void*>(now))));
    }
    return out;
}

std::vector<std::string> OtherModulesReport()
{
    std::vector<std::string> out;
    std::lock_guard lk(g_othersMtx);
    for (const auto& o : g_others) {
        if (!o.module)
            continue;
        out.push_back(std::format("{}: ReadFile patched ({} {}), GetOverlappedResult {}", o.name, FromWindows(o.readBefore) ? "was" : "chained after",
            o.readBefore, !o.gorSlot ? "not imported" : FromWindows(o.gorBefore) ? "patched (was " + o.gorBefore + ")" : "patched, chained after " + o.gorBefore));
    }
    return out;
}

BOOL WINAPI OnCloseHandle(HANDLE a_handle)
{
    // Forgotten first: as soon as it is closed, its value can be another file's.
    if (g_openCount.load(std::memory_order_relaxed)) {
        bool known = false;
        {
            std::shared_lock lk(g_openMtx);
            known = g_open.contains(a_handle);
        }
        if (known) {
            std::shared_ptr<Track> track;
            {
                std::unique_lock lk(g_openMtx);
                if (const auto it = g_open.find(a_handle); it != g_open.end()) {
                    track = std::move(it->second.track);
                    g_open.erase(it);
                }
                g_openCount.store(g_open.size(), std::memory_order_relaxed);
            }
            if (const auto sink = g_fileSink.load(std::memory_order_relaxed); sink && track) {
                const auto error = GetLastError();
                try {
                    FileTiming t;
                    t.name = track->name;
                    t.openTick = track->openTick;
                    const auto first = track->firstStart.load(std::memory_order_relaxed);
                    const auto last = track->lastDone.load(std::memory_order_relaxed);
                    t.firstReadMs = first ? TicksToMs(static_cast<std::uint64_t>(first - track->openQpc)) : -1.0;
                    t.lastDoneMs = last ? TicksToMs(static_cast<std::uint64_t>(last - track->openQpc)) : -1.0;
                    t.bytes = track->bytes.load(std::memory_order_relaxed);
                    t.reads = track->reads.load(std::memory_order_relaxed);
                    LARGE_INTEGER size{};
                    t.size = GetFileSizeEx(a_handle, &size) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
                    sink(t);
                } catch (...) {
                }
                SetLastError(error);
            }
            if (g_pendingCount.load(std::memory_order_relaxed)) {
                std::lock_guard lk(g_pendingMtx);
                const auto before = g_pending.size();
                std::erase_if(g_pending, [&](const auto& a_p) { return a_p.second.file == a_handle; });
                Add(g_untimed, before - g_pending.size());
                g_pendingCount.store(g_pending.size(), std::memory_order_relaxed);
            }
        }
    }
    return g_closeHandle(a_handle);
}
}  // namespace LooseReads
