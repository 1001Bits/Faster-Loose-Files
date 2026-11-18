#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <cstdlib>
#include <format>
#include <string>
#include <thread>

#include "F4se.h"
#include "Game.h"
#include "Log.h"
#include "LooseReads.h"
#include "Settings.h"

namespace
{
constexpr std::uint32_t kPluginVersion = F4se::Pack(FLF_VERSION_MAJOR, FLF_VERSION_MINOR, FLF_VERSION_PATCH);
constexpr const char* kPluginName = "FasterLooseFiles";

std::string LocalTime()
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// The real version (GetVersionEx answers with the manifest's), and Wine's under Proton.
std::string WindowsVersion()
{
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return "unknown";
    std::string v = "unknown";
    using GetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{ sizeof(info) };
    if (const auto get = reinterpret_cast<GetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")); get && get(&info) == 0)
        v = std::format("{}.{}.{}", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
    using WineFn = const char*(__cdecl*)();
    if (const auto wine = reinterpret_cast<WineFn>(GetProcAddress(ntdll, "wine_get_version")))
        v += std::format(" (Wine {})", wine());
    return v;
}

double Mb(std::uint64_t a_bytes) { return static_cast<double>(a_bytes) / 1048576.0; }

// Running totals on one line, for the lab and for players' logs alike.
std::string Totals(const LooseReads::Stats& a_s)
{
    return std::format("opens={} cached={} reads={} readMB={:.1f} waitMs={:.1f} longestMs={:.2f} pieces={} served={} diskReads={} "
                       "diskMB={:.1f} diskMs={:.1f} unusedMB={:.1f} passed={} errors={} untimed={} verified={} mismatches={} viaPlugins={} waitTimeouts={}",
        a_s.opens, a_s.cachedOpens, a_s.reads, Mb(a_s.readBytes), LooseReads::TicksToMs(a_s.readTicks),
        LooseReads::TicksToMs(a_s.longestTicks), a_s.pieces, a_s.served, a_s.diskReads, Mb(a_s.diskBytes),
        LooseReads::TicksToMs(a_s.diskTicks), Mb(a_s.unusedBytes), a_s.passed, a_s.errors, a_s.untimed, a_s.verified, a_s.mismatches,
        a_s.otherReads, a_s.timedOut);
}

std::string Summary(const char* a_when, bool a_atExit = false)
{
    const auto s = LooseReads::GetStats(a_atExit);
    std::string flags;
    for (const auto& f : s.openFlags)
        flags += std::format("{}0x{:08X}={}", flags.empty() ? "" : ",", f.flags, f.opens);
    const double ms = LooseReads::TicksToMs(s.readTicks);
    return std::format("SUMMARY ({}): {} loose files read {:.1f} MB in {:.1f} ms waiting ({:.2f} ms per MB); opened with flags {}; {}",
        a_when, s.opens, Mb(s.readBytes), ms, s.readBytes ? ms / Mb(s.readBytes) : 0.0, flags.empty() ? "none yet" : flags, Totals(s));
}

void Report(const char* a_when)
{
    Log::Info("{}", Summary(a_when));
}

// Other plugins that touch file reads, named once.
void LogNeighbours()
{
    struct Known
    {
        const wchar_t* dll;
        const char* note;
    };
    for (const auto& k : { Known{ L"FasterArchives.dll", "Faster Archives found: it patches the same imports for .ba2 files; both chain" },
             Known{ L"Addictol.dll", "Addictol found: its IO Cacher may already open loose files with the Windows cache (see the flags)" },
             Known{ L"FasterStartup.dll", "Faster Startups found: it caches folder listings at startup; it does not read files" },
             Known{ L"cetweaks.dll", "Creation Engine Tweaks found: its streamer threads read loose files through its own imports, patched too" },
             Known{ L"usvfs_x64.dll", "Mod Organizer's virtual file system found: loose files are opened through it" } }) {
        if (GetModuleHandleW(k.dll))
            Log::Info("{}", k.note);
    }
}

// Plugins that read the game's loose files on their own threads through their own imports.
void HookOtherReaders(const char* a_when)
{
    static std::size_t reported = 0;
    for (const auto* dll : { L"cetweaks.dll" }) {
        const auto module = GetModuleHandleW(dll);
        if (!module)
            continue;
        if (const auto problem = LooseReads::HookModuleReads(module); !problem.empty())
            Log::Warn("Creation Engine Tweaks found ({}), its reads of loose files go on unchanged: {}", a_when, problem);
    }
    const auto lines = LooseReads::OtherModulesReport();
    for (; reported < lines.size(); ++reported)
        Log::Info("import of {}", lines[reported]);
}

void CheckImports(const char* a_when)
{
    for (const auto& line : LooseReads::Check())
        Log::Warn("{} ({}): another plugin patched the import after Faster Loose Files; calls still reach it only if that plugin passes them on",
            line, a_when);
}

std::atomic<bool> g_statsStarted{ false };

// [Debug] bFileTimes: one line per closed loose file, for the lab.
HANDLE g_filesCsv = INVALID_HANDLE_VALUE;
SRWLOCK g_filesLock = SRWLOCK_INIT;

void OnFileClosed(const LooseReads::FileTiming& a_t)
{
    if (g_filesCsv == INVALID_HANDLE_VALUE)
        return;
    const auto line = std::format("{},{:.2f},{:.2f},{},{},{},{}\r\n", a_t.openTick, a_t.firstReadMs, a_t.lastDoneMs, a_t.bytes, a_t.size,
        a_t.reads, a_t.name);
    AcquireSRWLockExclusive(&g_filesLock);
    DWORD written = 0;
    WriteFile(g_filesCsv, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    ReleaseSRWLockExclusive(&g_filesLock);
}

void OpenFilesCsv(const std::filesystem::path& a_path)
{
    g_filesCsv = CreateFileW(a_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_filesCsv == INVALID_HANDLE_VALUE)
        return;
    const std::string head = "openTick,firstReadMs,lastDoneMs,bytes,size,reads,name\r\n";
    DWORD written = 0;
    WriteFile(g_filesCsv, head.data(), static_cast<DWORD>(head.size()), &written, nullptr);
    LooseReads::SetFileSink(&OnFileClosed);
}

// bEnableLogging: totals every iStatsIntervalMs (2 s) when anything changed, at least every 30 s, and a summary every minute.
void StartStats()
{
    if (g_statsStarted.exchange(true))
        return;
    std::thread([] {
        std::uint64_t lastReads = ~0ull, lastOpens = ~0ull;
        ULONGLONG lastLine = 0, lastSummary = GetTickCount64();
        for (;;) {
            Sleep(static_cast<DWORD>(Settings::iStatsIntervalMs));
            try {
            const auto s = LooseReads::GetStats();
            const auto now = GetTickCount64();
            if (s.reads != lastReads || s.opens != lastOpens || now - lastLine >= 30000) {
                Log::Info("STAT tick={} {}", now, Totals(s));
                lastReads = s.reads;
                lastOpens = s.opens;
                lastLine = now;
            }
            if (now - lastSummary >= 60000) {
                Report("running");
                lastSummary = now;
            }
            } catch (...) {
                // Never end the game over a log line (out of memory while formatting).
            }
        }
    }).detach();
}

void MessageHandler(F4se::Message* a_message)
{
    if (!a_message)
        return;
    switch (a_message->type) {
    case F4se::MessagingInterface::kPostLoad:
        LogNeighbours();
        HookOtherReaders("after plugins loaded");
        CheckImports("after plugins loaded");
        break;
    case F4se::MessagingInterface::kPostPostLoad:
        CheckImports("after every plugin loaded");
        break;
    case F4se::MessagingInterface::kGameDataReady:
        CheckImports("game data loaded");
        Report("game data loaded");
        break;
    case F4se::MessagingInterface::kPostLoadGame:
        Report("save loaded");
        break;
    default:
        break;
    }
}
}  // namespace

// F4SE 0.7 (1.10.980 and later) reads this to decide whether to load the plugin.
extern "C" __declspec(dllexport) constinit F4se::PluginVersionData F4SEPlugin_Version{
    .dataVersion = 1,
    .pluginVersion = kPluginVersion,
    .name = "FasterLooseFiles",
    .author = "",
    .addressIndependence = F4se::kAddressIndependentSignatures,
    .structureIndependence = F4se::kStructureIndependentNoStructs,
    .compatibleVersions = {},
    .f4seMinimum = 0,
    .reservedNonBreaking = 0,
    .reservedBreaking = 0,
    .reserved = {},
};

// F4SE 0.6 (1.10.163) and F4SEVR ask before loading the plugin.
extern "C" __declspec(dllexport) bool F4SEPlugin_Query(const F4se::Interface* a_f4se, F4se::PluginInfo* a_info)
{
    a_info->infoVersion = 1;
    a_info->name = kPluginName;
    a_info->version = kPluginVersion;
    return a_f4se && !a_f4se->isEditor;
}

extern "C" __declspec(dllexport) bool F4SEPlugin_Load(const F4se::Interface* a_f4se)
{
    if (!a_f4se || a_f4se->isEditor)
        return false;
    const auto f4se = F4se::Unpack(a_f4se->f4seVersion);
    std::string saveFolder;
    if (F4se::AtLeast(f4se, 0, 7, 1) && a_f4se->GetSaveFolderName)
        if (const char* name = a_f4se->GetSaveFolderName(); name && *name)
            saveFolder = name;

    Settings::Load();
    const auto logDir = Game::LogDirectory(saveFolder);
    if (logDir)
        Log::Open(*logDir / L"FasterLooseFiles.log");
    else
        Log::Open(Settings::IniPath().replace_extension(L".log"));
    if (Settings::bFileTimes)
        OpenFilesCsv((logDir ? *logDir : Settings::IniPath().parent_path()) / L"FasterLooseFiles-files.csv");

    const bool vr = Game::IsVR();
    MEMORYSTATUSEX memory{ sizeof(memory) };
    GlobalMemoryStatusEx(&memory);
    Log::Info("Faster Loose Files v{} (Fallout 4{} {}, F4SE {}.{}.{}), started {}", FLF_VERSION, vr ? " VR" : "", Game::ExeVersion().String(),
        f4se.major, f4se.minor, f4se.build, LocalTime());
    Log::Info("Windows {}; {} CPU threads; {:.1f} GB RAM, {:.1f} GB free", WindowsVersion(), std::thread::hardware_concurrency(),
        memory.ullTotalPhys / 1073741824.0, memory.ullAvailPhys / 1073741824.0);
    Log::Info("settings from {}", Settings::Describe());

    LooseReads::Config config;
    config.wholeFiles = Settings::bWholeFileReads;
    config.windowsCache = Settings::bWindowsFileCache;
    config.verify = Settings::bVerifyReads;
    config.maxReadKB = static_cast<unsigned>(Settings::iMaxReadKB);
    config.extensions = Settings::Extensions();
    LooseReads::Configure(config);
    // Before the game opens any loose file. Installed in every mode: with both ways off it only counts.
    if (const auto problem = LooseReads::Install(); !problem.empty()) {
        Log::Warn("loose files are read as the game does, nothing is counted: {}", problem);
        return true;
    }
    for (const auto& line : LooseReads::InstallReport())
        Log::Info("import {}", line);
    HookOtherReaders("at load");
    Log::Info("{}", Settings::bWholeFileReads || Settings::bWindowsFileCache
                        ? std::format("loose files: whole-file reads {}, Windows file cache {}", Settings::bWholeFileReads ? "on" : "off",
                              Settings::bWindowsFileCache ? "on" : "off")
                        : std::string("loose files: both ways off, the game's reads are only counted"));

    const auto* messaging = static_cast<const F4se::MessagingInterface*>(a_f4se->QueryInterface(F4se::kMessagingInterface));
    if (!messaging || !messaging->RegisterListener(a_f4se->GetPluginHandle(), "F4SE", MessageHandler))
        Log::Warn("messaging interface unavailable: no summaries at load");
    if (Settings::bEnableLogging)
        StartStats();

    std::atexit([]() noexcept {
        try {
            Log::WriteIfFree("info", Summary("exit", true));
        } catch (...) {
        }
    });
    return true;
}
