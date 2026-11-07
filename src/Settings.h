#pragma once

// FasterLooseFiles.ini, next to the plugin DLL.

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace Settings
{
// [General]
inline bool bWholeFileReads{ true };
inline bool bWindowsFileCache{ true };
inline std::string sExtensions{ "dds,nif,bgsm,bgem" };  // textures, meshes, materials; never sound (played piece by piece)
inline bool bEnableLogging{ false };
// [Debug]
inline bool bVerifyReads{ false };
inline int iStatsIntervalMs{ 2000 };  // how often bEnableLogging writes the totals
inline bool bFileTimes{ false };     // FasterLooseFiles-files.csv: each loose file's open -> last byte, for testing
inline int iMaxReadKB{ 1024 };        // the largest single whole-file read (4 MB in one go held the reading thread up to 364 ms)

[[nodiscard]] inline std::filesystem::path IniPath()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&IniPath), &self);
    wchar_t dll[MAX_PATH]{};
    GetModuleFileNameW(self, dll, MAX_PATH);
    return std::filesystem::path(dll).replace_extension(L".ini");
}

namespace detail
{
inline std::wstring Read(const std::filesystem::path& a_ini, const wchar_t* a_section, const wchar_t* a_key)
{
    wchar_t buf[512]{};
    GetPrivateProfileStringW(a_section, a_key, L"", buf, static_cast<DWORD>(std::size(buf)), a_ini.c_str());
    std::wstring v = buf;
    for (auto& c : v)
        c = static_cast<wchar_t>(std::towlower(c));
    return v;
}

inline int Int(const std::filesystem::path& a_ini, const wchar_t* a_section, const wchar_t* a_key, int a_default)
{
    const auto v = Read(a_ini, a_section, a_key);
    try {
        return v.empty() ? a_default : std::stoi(v);
    } catch (...) {
        return a_default;
    }
}

inline bool Bool(const std::filesystem::path& a_ini, const wchar_t* a_section, const wchar_t* a_key, bool a_default)
{
    const auto v = Read(a_ini, a_section, a_key);
    if (v == L"1" || v == L"true" || v == L"yes" || v == L"on")
        return true;
    if (v == L"0" || v == L"false" || v == L"no" || v == L"off")
        return false;
    return a_default;
}
}  // namespace detail

inline void Load()
{
    const auto ini = IniPath();
    bWholeFileReads = detail::Bool(ini, L"General", L"bWholeFileReads", bWholeFileReads);
    bWindowsFileCache = detail::Bool(ini, L"General", L"bWindowsFileCache", bWindowsFileCache);
    bEnableLogging = detail::Bool(ini, L"General", L"bEnableLogging", bEnableLogging);
    bVerifyReads = detail::Bool(ini, L"Debug", L"bVerifyReads", bVerifyReads);
    iStatsIntervalMs = (std::clamp)(detail::Int(ini, L"Debug", L"iStatsIntervalMs", iStatsIntervalMs), 100, 60000);
    bFileTimes = detail::Bool(ini, L"Debug", L"bFileTimes", bFileTimes);
    iMaxReadKB = (std::clamp)(detail::Int(ini, L"Debug", L"iMaxReadKB", iMaxReadKB), 128, 4096);
    const auto ext = detail::Read(ini, L"General", L"sExtensions");
    if (!ext.empty()) {
        std::string ascii;
        for (const auto c : ext)
            if (c < 128)
                ascii += static_cast<char>(c);
        sExtensions = ascii;
    }
}

// "dds, png" -> { "dds", "png" }
[[nodiscard]] inline std::vector<std::string> Extensions()
{
    std::vector<std::string> out;
    std::string part;
    for (const auto c : sExtensions + ",") {
        if (c == ',') {
            if (!part.empty())
                out.push_back(part);
            part.clear();
        } else if (c != ' ' && c != '\t' && c != '.') {
            part += c;
        }
    }
    return out;
}

[[nodiscard]] inline std::string Describe()
{
    std::error_code ec;
    return std::format("{}: bWholeFileReads={} bWindowsFileCache={} sExtensions={} bEnableLogging={} bVerifyReads={} iStatsIntervalMs={} iMaxReadKB={} bFileTimes={}",
        std::filesystem::exists(IniPath(), ec) ? "FasterLooseFiles.ini" : "FasterLooseFiles.ini missing, defaults",
        int(bWholeFileReads), int(bWindowsFileCache), sExtensions, int(bEnableLogging), int(bVerifyReads), iStatsIntervalMs, iMaxReadKB, int(bFileTimes));
}
}  // namespace Settings
