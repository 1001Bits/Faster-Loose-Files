#pragma once

// The parts of F4SE's plugin interface Faster Loose Files uses, declared with the
// layout F4SE and F4SEVR pass to plugins (their PluginAPI.h).
//
// F4SE 0.7 (Fallout 4 1.10.980 and later) reads F4SEPlugin_Version and calls
// F4SEPlugin_Load; F4SE 0.6 (1.10.163) and F4SEVR call F4SEPlugin_Query
// first. One DLL exports all three.

#include <cstdint>

namespace F4se
{
using PluginHandle = std::uint32_t;

// F4SEInterface: members after GetPluginInfo exist only in later F4SE builds.
struct Interface
{
    std::uint32_t f4seVersion;     // packed, see Unpack
    std::uint32_t runtimeVersion;  // packed
    std::uint32_t editorVersion;
    std::uint32_t isEditor;
    void* (*QueryInterface)(std::uint32_t a_id);
    PluginHandle (*GetPluginHandle)();
    std::uint32_t (*GetReleaseIndex)();
    const void* (*GetPluginInfo)(const char* a_name);  // F4SE 0.6.22 and later
    const char* (*GetSaveFolderName)();                // F4SE 0.7.1 and later
};

inline constexpr std::uint32_t kMessagingInterface = 1;

struct Message
{
    const char* sender;
    std::uint32_t type;
    std::uint32_t dataLen;
    void* data;
};

// F4SEMessagingInterface: Dispatch and GetEventDispatcher follow; not used.
struct MessagingInterface
{
    enum : std::uint32_t
    {
        kPostLoad = 0,
        kPostPostLoad = 1,
        kPreLoadGame = 2,
        kPostLoadGame = 3,
        kPreSaveGame = 4,
        kPostSaveGame = 5,
        kDeleteGame = 6,
        kInputLoaded = 7,
        kNewGame = 8,
        kGameLoaded = 9,
        kGameDataReady = 10,
    };

    std::uint32_t interfaceVersion;
    bool (*RegisterListener)(PluginHandle a_listener, const char* a_sender, void (*a_handler)(Message*));
};

// Filled in by F4SEPlugin_Query (1.10.163 and VR).
struct PluginInfo
{
    std::uint32_t infoVersion;  // 1
    const char* name;
    std::uint32_t version;
};

// Exported as F4SEPlugin_Version (1.10.980 and later).
struct PluginVersionData
{
    std::uint32_t dataVersion;  // 1
    std::uint32_t pluginVersion;
    char name[256];
    char author[256];
    std::uint32_t addressIndependence;
    std::uint32_t structureIndependence;
    std::uint32_t compatibleVersions[16];  // zero-terminated; ignored with an independence flag
    std::uint32_t f4seMinimum;
    std::uint32_t reservedNonBreaking;
    std::uint32_t reservedBreaking;
    std::uint8_t reserved[512];
};
static_assert(sizeof(PluginVersionData) == 0x45C);

// addressIndependence: uses no address in the game's code. Faster Loose Files
// changes only the game's import table (CreateFile, ReadFile, ...), found by name.
inline constexpr std::uint32_t kAddressIndependentSignatures = 1u << 0;
// structureIndependence: uses none of the game's structures.
inline constexpr std::uint32_t kStructureIndependentNoStructs = 1u << 0;

[[nodiscard]] constexpr std::uint32_t Pack(std::uint32_t a_major, std::uint32_t a_minor, std::uint32_t a_build,
    std::uint32_t a_sub = 0) noexcept
{
    return (a_major << 24) | ((a_minor & 0xFF) << 16) | ((a_build & 0xFFF) << 4) | (a_sub & 0xF);
}

struct Version
{
    std::uint32_t major, minor, build, sub;
};

[[nodiscard]] constexpr Version Unpack(std::uint32_t a_packed) noexcept
{
    return { a_packed >> 24, (a_packed >> 16) & 0xFF, (a_packed >> 4) & 0xFFF, a_packed & 0xF };
}

[[nodiscard]] constexpr bool AtLeast(const Version& a_v, std::uint32_t a_major, std::uint32_t a_minor,
    std::uint32_t a_build) noexcept
{
    if (a_v.major != a_major) return a_v.major > a_major;
    if (a_v.minor != a_minor) return a_v.minor > a_minor;
    return a_v.build >= a_build;
}
}  // namespace F4se
