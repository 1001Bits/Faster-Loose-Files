#pragma once

// FasterLooseFiles.log: one line per call, written at once, so a game that is closed
// or crashes still leaves everything up to its last moment.

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace Log
{
// The previous start's log is kept as FasterLooseFiles-previous.log.
void Open(const std::filesystem::path& a_path);

void Write(std::string_view a_level, std::string_view a_text) noexcept;

// At exit: never waits for a line another thread may have been stopped in the middle of.
void WriteIfFree(std::string_view a_level, std::string_view a_text) noexcept;

// A line that cannot be formatted (out of memory) is dropped: logging never ends the game.
template <class... Args>
void Info(std::format_string<Args...> a_fmt, Args&&... a_args) noexcept
{
    try {
        Write("info", std::format(a_fmt, std::forward<Args>(a_args)...));
    } catch (...) {
    }
}

template <class... Args>
void Warn(std::format_string<Args...> a_fmt, Args&&... a_args) noexcept
{
    try {
        Write("warn", std::format(a_fmt, std::forward<Args>(a_args)...));
    } catch (...) {
    }
}

template <class... Args>
void Error(std::format_string<Args...> a_fmt, Args&&... a_args) noexcept
{
    try {
        Write("error", std::format(a_fmt, std::forward<Args>(a_args)...));
    } catch (...) {
    }
}
}  // namespace Log
