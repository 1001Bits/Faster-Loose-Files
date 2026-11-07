// Offline tests of the loose file reads. Reads files only; the synthetic files live in a temporary folder.
//   LooseReadsTest [trace files of the game's real loose reads (tools\bench\loose-reads-*.txt)]
// Every read is made as the game's BSSystemFile::DoReadAt makes it (ReadFile with an OVERLAPPED whose event is 0, then
// GetOverlappedResult when pending), through the plugin's import replacements, and its bytes are compared with a plain read.
#include "LooseReads.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace LooseReads;

namespace
{
int g_failures = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            ++g_failures;                                   \
            std::printf("FAIL %s:%d: %s ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                       \
            std::printf("\n");                              \
        }                                                   \
    } while (0)

constexpr DWORD kPiece = 64 * 1024;
constexpr DWORD kGameAsync = 0x68000000;  // overlapped, no buffering, sequential: the streamer's loose opens
constexpr DWORD kGameSync = 0x08000000;   // sequential: streamer off
constexpr DWORD kGameSyncUnbuffered = 0x28000000;

struct Aligned
{
    std::uint8_t* p;
    explicit Aligned(std::size_t a_size) : p(static_cast<std::uint8_t*>(VirtualAlloc(nullptr, a_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) {}
    ~Aligned() { VirtualFree(p, 0, MEM_RELEASE); }
};

// As BSSystemFile::DoReadAt: the bytes the game ends up with, or -1 on an error other than the end of the file.
long long GameReadAt(HANDLE a_file, void* a_to, std::uint64_t a_at)
{
    OVERLAPPED o{};
    o.Offset = static_cast<DWORD>(a_at);
    o.OffsetHigh = static_cast<DWORD>(a_at >> 32);
    DWORD got = 0;
    if (!OnReadFile(a_file, a_to, kPiece, &got, &o)) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            if (OnGetOverlappedResult(a_file, &o, &got, TRUE))
                return got;
            error = GetLastError();
        }
        return error == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return got;
}

std::vector<std::uint8_t> Content(std::uint64_t a_size)
{
    std::vector<std::uint8_t> v(a_size);
    std::mt19937_64 r(a_size * 7919 + 1);
    for (auto& b : v)
        b = static_cast<std::uint8_t>(r() >> 56);
    return v;
}

void Write(const fs::path& a_path, const std::vector<std::uint8_t>& a_data)
{
    std::ofstream(a_path, std::ios::binary).write(reinterpret_cast<const char*>(a_data.data()), static_cast<std::streamsize>(a_data.size()));
}

Config Mode(bool a_whole, bool a_cache, bool a_verify = false)
{
    Config c;
    c.wholeFiles = a_whole;
    c.windowsCache = a_cache;
    c.verify = a_verify;
    c.extensions = { "dds" };
    return c;
}

void TestNames()
{
    Configure(Mode(true, true));
    CHECK(Listed("Data\\Textures\\a.dds"), "");
    CHECK(Listed("Data\\Textures\\A.DDS"), "");
    CHECK(!Listed("Data\\Textures\\a.dds.bak"), "");
    CHECK(!Listed("dds"), "");
    CHECK(!Listed(".ddsx"), "");
    CHECK(!Listed("Fallout4 - Textures1.ba2"), "");
    CHECK(Listed(L"c:\\x\\b.Dds"), "");
    CHECK(!Listed(L"c:\\x\\b.nif"), "");
    Config two = Mode(true, true);
    two.extensions = { ".DDS", " png" };
    Configure(two);
    CHECK(Listed("a.png") && Listed("a.dds") && !Listed("a.nif"), "list with dots, spaces, capitals");
    Configure(Mode(true, true));
}

// Synthetic files of awkward sizes, read in the game's pattern and others, files interleaved, in every mode and open kind.
void TestSynthetic(const fs::path& a_dir)
{
    const std::uint64_t sizes[] = { 0, 1, 4095, 65535, 65536, 65537, 131072, 200000, 1048576 + 512, 4194304, 4194304 + 3, 9 * 1048576 + 12345 };
    std::vector<std::vector<std::uint8_t>> contents;
    std::vector<fs::path> paths;
    for (const auto size : sizes) {
        paths.push_back(a_dir / ("t" + std::to_string(size) + ".dds"));
        contents.push_back(Content(size));
        Write(paths.back(), contents.back());
    }
    Aligned buffer(kPiece), reference(kPiece);
    for (const bool verify : { false, true }) {
        for (const DWORD flags : { kGameAsync, kGameSync, kGameSyncUnbuffered }) {
            for (const bool whole : { false, true }) {
                for (const bool cache : { false, true }) {
                    Configure(Mode(whole, cache, verify));
                    const auto before = GetStats();
                    std::vector<HANDLE> handles, references;
                    const DWORD used = cache && (flags & FILE_FLAG_NO_BUFFERING)
                                           ? (flags & ~(FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN)) | FILE_FLAG_RANDOM_ACCESS
                                           : flags;
                    for (const auto& p : paths) {
                        handles.push_back(OnCreateFileA(p.string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr));
                        CHECK(handles.back() != INVALID_HANDLE_VALUE, "open %s", p.string().c_str());
                        references.push_back(::CreateFileA(p.string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, used, nullptr));
                    }
                    // Round robin over all files, piece after piece and one past the end: more files than slots.
                    std::uint64_t pass = 0;
                    for (bool any = true; any; ++pass) {
                        any = false;
                        for (std::size_t f = 0; f < paths.size(); ++f) {
                            const auto at = pass * kPiece;
                            if (at > sizes[f] + kPiece)
                                continue;
                            any = true;
                            std::memset(buffer.p, 0xCD, kPiece);
                            LARGE_INTEGER mark;
                            mark.QuadPart = 12345;
                            const bool sync = !(flags & FILE_FLAG_OVERLAPPED);
                            if (sync) {
                                SetFilePointerEx(handles[f], mark, nullptr, FILE_BEGIN);
                                SetFilePointerEx(references[f], mark, nullptr, FILE_BEGIN);
                            }
                            const auto got = GameReadAt(handles[f], buffer.p, at);
                            const auto want = at < sizes[f] ? (std::min)(std::uint64_t{ kPiece }, sizes[f] - at) : 0;
                            CHECK(got == static_cast<long long>(want), "flags %08lX whole %d cache %d size %llu at %llu: got %lld want %llu", flags,
                                whole, cache, sizes[f], at, got, want);
                            if (got > 0)
                                CHECK(std::memcmp(buffer.p, contents[f].data() + at, static_cast<std::size_t>(got)) == 0, "bytes, size %llu at %llu",
                                    sizes[f], at);
                            if (sync) {
                                // The file pointer as Windows leaves it after the same read on a handle opened the same way.
                                OVERLAPPED o{};
                                o.Offset = static_cast<DWORD>(at);
                                DWORD n = 0;
                                ::ReadFile(references[f], reference.p, kPiece, &n, &o);
                                LARGE_INTEGER zero{}, pos{}, windows{};
                                SetFilePointerEx(handles[f], zero, &pos, FILE_CURRENT);
                                SetFilePointerEx(references[f], zero, &windows, FILE_CURRENT);
                                CHECK(pos.QuadPart == windows.QuadPart, "flags %08lX whole %d cache %d: file pointer %lld after a read at %llu of %llu, Windows %lld",
                                    flags, whole, cache, pos.QuadPart, at, want, windows.QuadPart);
                            }
                        }
                    }
                    // Re-reads backwards and a jump into the middle of a big file.
                    for (std::size_t f = 0; f < paths.size(); ++f) {
                        for (std::uint64_t at = sizes[f] / kPiece * kPiece + kPiece; at >= kPiece; at -= kPiece) {
                            const auto a = at - kPiece;
                            const auto got = GameReadAt(handles[f], buffer.p, a);
                            const auto want = a < sizes[f] ? (std::min)(std::uint64_t{ kPiece }, sizes[f] - a) : 0;
                            CHECK(got == static_cast<long long>(want), "backwards size %llu at %llu", sizes[f], a);
                            if (got > 0)
                                CHECK(std::memcmp(buffer.p, contents[f].data() + a, static_cast<std::size_t>(got)) == 0, "backwards bytes");
                        }
                    }
                    // Buffered handles accept any offset: those reads pass through unchanged.
                    if (cache || !(flags & FILE_FLAG_NO_BUFFERING)) {
                        const auto f = paths.size() - 1;
                        const std::uint64_t at = 3 * kPiece + 777;
                        const auto got = GameReadAt(handles[f], buffer.p, at);
                        CHECK(got == kPiece && std::memcmp(buffer.p, contents[f].data() + at, kPiece) == 0, "unaligned read");
                    }
                    for (const auto h : handles)
                        CHECK(OnCloseHandle(h), "close");
                    for (const auto h : references)
                        ::CloseHandle(h);
                    const auto after = GetStats();
                    CHECK(after.opens - before.opens == paths.size(), "opens counted");
                    CHECK(after.errors == before.errors, "errors %llu", after.errors - before.errors);
                    CHECK(after.mismatches == 0, "verify mismatches");
                    if (whole && ((flags & FILE_FLAG_OVERLAPPED) || !(used & FILE_FLAG_NO_BUFFERING)))
                        CHECK(after.served > before.served && after.diskReads > before.diskReads, "whole mode serves");
                    else
                        CHECK(after.served == before.served && after.diskReads == before.diskReads, "off mode, or a synchronous unbuffered handle, never serves");
                    if (cache && (flags & FILE_FLAG_NO_BUFFERING))
                        CHECK(after.cachedOpens - before.cachedOpens == paths.size(), "cached opens");
                    else
                        CHECK(after.cachedOpens == before.cachedOpens, "no cache change");
                    if (verify && after.served > before.served)
                        CHECK(after.verified > before.verified, "verify ran");
                }
            }
        }
    }
    // A file opened for writing, or created, is never tracked.
    Configure(Mode(true, true));
    const auto before = GetStats();
    HANDLE w = OnCreateFileA(paths.back().string().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, kGameAsync, nullptr);
    CHECK(w != INVALID_HANDLE_VALUE, "open for writing");
    OnCloseHandle(w);
    HANDLE n = OnCreateFileA((a_dir / "new.dds").string().c_str(), GENERIC_READ, 0, nullptr, CREATE_NEW, 0, nullptr);
    OnCloseHandle(n);
    HANDLE other = OnCreateFileA((a_dir / "t65536.dds").string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, kGameAsync, nullptr);
    OnCloseHandle(other);
    CHECK(GetStats().opens - before.opens == 1, "only the read-only open is tracked");
    std::printf("synthetic: %zu files x 3 open kinds x 4 modes x verify off/on\n", paths.size());
}

struct TraceRead
{
    std::uint32_t file;
    std::uint64_t at;
    std::uint32_t size;
    bool texture;
};

// The game's recorded loose reads, replayed in order in each mode: every piece must match a plain read of the file.
void TestTrace(const fs::path& a_trace)
{
    std::ifstream in(a_trace);
    std::size_t count = 0;
    in >> count;
    std::string line;
    std::getline(in, line);
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < count; ++i) {
        std::getline(in, line);
        paths.push_back(line);
    }
    std::size_t reads = 0;
    in >> reads;
    std::vector<TraceRead> list;
    for (std::size_t i = 0; i < reads; ++i) {
        TraceRead r{};
        int t = 0;
        in >> r.file >> r.at >> r.size >> t;
        r.texture = t != 0;
        if (r.texture && r.size == kPiece)
            list.push_back(r);
    }
    std::vector<bool> exists(paths.size());
    std::size_t present = 0;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        std::error_code ec;
        exists[i] = fs::exists(fs::path(reinterpret_cast<const char8_t*>(paths[i].c_str())), ec);
    }
    std::vector<std::vector<std::uint8_t>> data(paths.size());
    for (const auto& r : list) {
        if (exists[r.file] && data[r.file].empty()) {
            std::ifstream f(fs::path(reinterpret_cast<const char8_t*>(paths[r.file].c_str())), std::ios::binary);
            data[r.file].assign(std::istreambuf_iterator<char>(f), {});
            ++present;
        }
    }
    Aligned buffer(kPiece);
    struct Run
    {
        bool whole, cache;
        unsigned cap;
    };
    for (const auto [whole, cache, cap] : { Run{ false, false, 4096 }, Run{ false, true, 4096 }, Run{ true, false, 4096 }, Run{ true, true, 4096 }, Run{ true, true, 1024 } }) {
        {
            auto config = Mode(whole, cache);
            config.maxReadKB = cap;
            Configure(config);
            const auto before = GetStats();
            std::vector<HANDLE> handles(paths.size(), INVALID_HANDLE_VALUE);
            std::size_t done = 0, skipped = 0, bad = 0;
            for (const auto& r : list) {
                if (!exists[r.file]) {
                    ++skipped;
                    continue;
                }
                auto& h = handles[r.file];
                if (h == INVALID_HANDLE_VALUE)
                    h = OnCreateFileA(paths[r.file].c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, kGameAsync, nullptr);
                const auto& d = data[r.file];
                const auto got = GameReadAt(h, buffer.p, r.at);
                const auto want = r.at < d.size() ? (std::min)(std::uint64_t{ kPiece }, d.size() - r.at) : 0;
                if (got != static_cast<long long>(want) || (got > 0 && std::memcmp(buffer.p, d.data() + r.at, static_cast<std::size_t>(got)) != 0))
                    ++bad;
                ++done;
            }
            for (const auto h : handles)
                if (h != INVALID_HANDLE_VALUE)
                    OnCloseHandle(h);
            const auto after = GetStats();
            CHECK(bad == 0, "%s whole %d cache %d cap %u: %zu reads differ", a_trace.filename().string().c_str(), whole, cache, cap, bad);
            std::printf("trace %s whole %d cache %d cap %u KB: %zu reads of %zu files compared (%zu of missing files skipped), %zu differ; served %llu, "
                        "whole-file reads %llu (%.1f MB), unused %.1f MB, wait %.1f ms\n",
                a_trace.filename().string().c_str(), whole, cache, cap, done, present, skipped, bad, after.served - before.served,
                after.diskReads - before.diskReads, (after.diskBytes - before.diskBytes) / 1048576.0, (after.unusedBytes - before.unusedBytes) / 1048576.0,
                TicksToMs(after.readTicks - before.readTicks));
        }
    }
}

// The test's own imports patched as in the game, with another "plugin" already in the ReadFile slot.
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
ReadFileFn g_otherOriginal = nullptr;
int g_otherCalls = 0;

BOOL WINAPI OtherReadFile(HANDLE a_file, LPVOID a_buffer, DWORD a_size, LPDWORD a_read, LPOVERLAPPED a_overlapped)
{
    ++g_otherCalls;
    return g_otherOriginal(a_file, a_buffer, a_size, a_read, a_overlapped);
}

ULONGLONG* OwnSlot(const char* a_name)
{
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        auto* slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        auto* name = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        for (; slot->u1.Function; ++slot, ++name)
            if (!IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal) &&
                std::strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name->u1.AddressOfData)->Name, a_name) == 0)
                return &slot->u1.Function;
    }
    return nullptr;
}

// The review's case: the game waits (GetOverlappedResult, hEvent NULL) on a read of its own while a 64 KB piece of the same
// handle comes in. No whole-file read may run on that handle meanwhile: it would clear the handle's signal.
void TestInFlight(const fs::path& a_dir)
{
    Configure(Mode(true, false));  // unbuffered: the game's own read goes pending on any disk
    const auto path = a_dir / "t9449529.dds";
    const auto content = Content(9 * 1048576 + 12345);
    Aligned first(16 * kPiece), second(kPiece);
    HANDLE h = OnCreateFileA(path.string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, kGameAsync, nullptr);
    OVERLAPPED o1{};
    o1.Offset = 4 * 1048576;
    DWORD n1 = 0;
    const BOOL ok1 = OnReadFile(h, first.p, 16 * kPiece, &n1, &o1);  // not one piece: the game's own read
    const bool pending = !ok1 && GetLastError() == ERROR_IO_PENDING;
    const auto before = GetStats();
    // A first piece: would start a whole-file read. With its own event: two reads in flight on one handle cannot both
    // wait on the handle itself (Windows' rule, so the game never does it).
    OVERLAPPED o2{};
    o2.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n2 = 0;
    if (!OnReadFile(h, second.p, kPiece, &n2, &o2) && GetLastError() == ERROR_IO_PENDING)
        OnGetOverlappedResult(h, &o2, &n2, TRUE);
    CloseHandle(o2.hEvent);
    const long long got = n2;
    const auto after = GetStats();
    if (pending)
        CHECK(after.diskReads == before.diskReads && after.served == before.served, "a whole-file read ran while the game waited");
    CHECK(got == kPiece && std::memcmp(second.p, content.data(), kPiece) == 0, "the piece read meanwhile");
    if (pending)
        OnGetOverlappedResult(h, &o1, &n1, TRUE);
    CHECK(n1 == 16 * kPiece && std::memcmp(first.p, content.data() + 4 * 1048576, n1) == 0, "the game's own read");
    // With nothing in flight the same piece starts one.
    const auto again = GetStats();
    GameReadAt(h, second.p, 65536 * 2);
    CHECK(GetStats().diskReads == again.diskReads + 1, "whole-file read once nothing is in flight");
    OnCloseHandle(h);
    // Opened with write sharing: never tracked.
    const auto opens = GetStats().opens;
    OnCloseHandle(OnCreateFileA(path.string().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, kGameAsync, nullptr));
    CHECK(GetStats().opens == opens, "a file others may write is not tracked");
    std::printf("in flight: the game's read %s pending\n", pending ? "was" : "was not (finished at once, check skipped)");
}

std::vector<FileTiming> g_closed;
void Sink(const FileTiming& a_t) { g_closed.push_back(a_t); }

// [Debug] bFileTimes: each closed file's times, in every mode.
void TestFileTimes(const fs::path& a_dir)
{
    const auto path = a_dir / "t1049088.dds";  // 1 MB + 512 bytes: 17 pieces
    Aligned buffer(kPiece);
    SetFileSink(&Sink);
    for (const bool whole : { false, true }) {
        Configure(Mode(whole, true));
        g_closed.clear();
        HANDLE h = OnCreateFileA(path.string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, kGameAsync, nullptr);
        for (std::uint64_t at = 0; at < 1048576 + 512; at += kPiece)
            GameReadAt(h, buffer.p, at);
        OnCloseHandle(h);
        CHECK(g_closed.size() == 1, "one record per closed file: %zu", g_closed.size());
        if (g_closed.size() == 1) {
            const auto& t = g_closed[0];
            CHECK(t.bytes == 1048576 + 512 && t.size == 1048576 + 512 && t.reads == 17, "whole %d: bytes %llu size %llu reads %u", whole, t.bytes,
                t.size, t.reads);
            CHECK(t.firstReadMs >= 0 && t.lastDoneMs >= t.firstReadMs && t.name.ends_with("t1049088.dds") && t.openTick > 0, "times %.3f %.3f %s",
                t.firstReadMs, t.lastDoneMs, t.name.c_str());
        }
    }
    // A file opened and closed without a read.
    g_closed.clear();
    OnCloseHandle(OnCreateFileA(path.string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, kGameAsync, nullptr));
    CHECK(g_closed.size() == 1 && g_closed[0].reads == 0 && g_closed[0].firstReadMs < 0, "unread file");
    SetFileSink(nullptr);
    std::printf("file times: %zu records checked\n", std::size_t{ 3 });
}

void TestInstall(const fs::path& a_dir)
{
    auto* slot = OwnSlot("ReadFile");
    CHECK(slot != nullptr, "the test imports ReadFile");
    if (!slot)
        return;
    DWORD old = 0;
    VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old);
    g_otherOriginal = reinterpret_cast<ReadFileFn>(*slot);
    *slot = reinterpret_cast<ULONGLONG>(&OtherReadFile);
    VirtualProtect(slot, sizeof(*slot), old, &old);

    Configure(Mode(true, true));
    const auto problem = Install();
    CHECK(problem.empty(), "install: %s", problem.c_str());
    bool chained = false;
    for (const auto& line : InstallReport()) {
        std::printf("  import %s\n", line.c_str());
        chained = chained || (line.starts_with("ReadFile:") && line.find("chained after") != std::string::npos);
    }
    CHECK(chained, "ReadFile chained after the other patch");
    CHECK(Check().empty(), "nothing took the imports");

    // Through the patched imports, as the game calls them.
    const auto path = a_dir / "t4194307.dds";
    const auto content = Content(4194304 + 3);
    const auto before = GetStats();
    HANDLE h = ::CreateFileA(path.string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, kGameAsync, nullptr);
    Aligned buffer(kPiece);
    for (std::uint64_t at = 0; at < content.size(); at += kPiece) {
        OVERLAPPED o{};
        o.Offset = static_cast<DWORD>(at);
        DWORD got = 0;
        if (!::ReadFile(h, buffer.p, kPiece, &got, &o) && GetLastError() == ERROR_IO_PENDING)
            ::GetOverlappedResult(h, &o, &got, TRUE);
        const auto want = (std::min)(std::uint64_t{ kPiece }, content.size() - at);
        CHECK(got == want && std::memcmp(buffer.p, content.data() + at, want) == 0, "patched read at %llu", at);
    }
    ::CloseHandle(h);
    const auto after = GetStats();
    // 64 pieces from the first read of 4 MB; the last 3 bytes are read the game's way.
    CHECK(after.opens - before.opens == 1 && after.served - before.served == 64, "served through the imports: %llu",
        after.served - before.served);
    CHECK(g_otherCalls >= 2, "the other patch still sees our whole-file reads: %d", g_otherCalls);
    const int calls = g_otherCalls;
    HANDLE plain = ::CreateFileW((a_dir / "plain.bin").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n = 0;
    char c[4];
    ::WriteFile(plain, "abcd", 4, &n, nullptr);
    SetFilePointer(plain, 0, nullptr, FILE_BEGIN);
    CHECK(::ReadFile(plain, c, 4, &n, nullptr) && n == 4 && std::memcmp(c, "abcd", 4) == 0, "an untracked read");
    ::CloseHandle(plain);
    CHECK(g_otherCalls == calls + 1, "an untracked read reaches the other patch");
}
}  // namespace

int main(int argc, char** argv)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path dir = fs::path(tmp) / (L"flf-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(dir);
    TestNames();
    TestSynthetic(dir);
    for (int i = 1; i < argc; ++i)
        TestTrace(argv[i]);
    TestInFlight(dir);
    TestFileTimes(dir);
    TestInstall(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
