#pragma once

// The running game: its executable's version, whether it is Fallout 4 VR, and
// where F4SE plugins write their logs.

#include <array>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <format>
#include <optional>
#include <string>

#include <ShlObj.h>

namespace Game
{
[[nodiscard]] inline std::filesystem::path ExePath()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return exe;
}

[[nodiscard]] inline bool IsVR()
{
    auto name = ExePath().filename().wstring();
    for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
    return name == L"fallout4vr.exe";
}

struct Version
{
    std::uint16_t major{ 0 }, minor{ 0 }, patch{ 0 }, build{ 0 };

    // "1.11.240", the form players know.
    [[nodiscard]] std::string String() const
    {
        return build ? std::format("{}.{}.{}.{}", major, minor, patch, build) : std::format("{}.{}.{}", major, minor, patch);
    }
    [[nodiscard]] bool Known() const noexcept { return major || minor || patch || build; }
};

// The ProductVersion text in an executable's version resource ("1.10.163.0"),
// else its numeric version fields.
[[nodiscard]] inline Version VersionOf(const std::filesystem::path& a_exe)
{
    Version v;
    const auto& exe = a_exe;
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &ignored);
    if (!size) return v;
    std::string data(size, '\0');
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return v;

    struct Translation
    {
        WORD language, codePage;
    };
    Translation* translation = nullptr;
    UINT length = 0;
    std::wstring key = L"\\StringFileInfo\\040904B0\\ProductVersion";
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translation), &length) &&
        translation && length >= sizeof(Translation))
        key = std::format(L"\\StringFileInfo\\{:04X}{:04X}\\ProductVersion", translation->language, translation->codePage);
    wchar_t* text = nullptr;
    if (VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&text), &length) && text && length > 1) {
        std::array<unsigned, 4> parts{};
        if (swscanf_s(text, L"%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]) >= 3 ||
            swscanf_s(text, L"%u, %u, %u, %u", &parts[0], &parts[1], &parts[2], &parts[3]) >= 3) {
            v.major = static_cast<std::uint16_t>(parts[0]);
            v.minor = static_cast<std::uint16_t>(parts[1]);
            v.patch = static_cast<std::uint16_t>(parts[2]);
            v.build = static_cast<std::uint16_t>(parts[3]);
            return v;
        }
    }

    VS_FIXEDFILEINFO* info = nullptr;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info ||
        length < sizeof(VS_FIXEDFILEINFO))
        return v;
    v.major = HIWORD(info->dwProductVersionMS);
    v.minor = LOWORD(info->dwProductVersionMS);
    v.patch = HIWORD(info->dwProductVersionLS);
    v.build = LOWORD(info->dwProductVersionLS);
    return v;
}

// The running game's version.
[[nodiscard]] inline Version ExeVersion() { return VersionOf(ExePath()); }

// Documents\My Games\<folder>\F4SE, the folder F4SE and its plugins log to.
// a_saveFolder: the name F4SE 0.7.1+ reports (empty before), else
// "Fallout4VR" or "Fallout4".
[[nodiscard]] inline std::optional<std::filesystem::path> LogDirectory(const std::string& a_saveFolder = {})
{
    wchar_t* documents = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)) || !documents) {
        CoTaskMemFree(documents);
        return std::nullopt;
    }
    std::filesystem::path dir = documents;
    CoTaskMemFree(documents);
    dir /= L"My Games";
    if (!a_saveFolder.empty())
        dir /= std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(a_saveFolder.data()), a_saveFolder.size()));
    else
        dir /= IsVR() ? L"Fallout4VR" : L"Fallout4";
    return dir / L"F4SE";
}
}  // namespace Game
