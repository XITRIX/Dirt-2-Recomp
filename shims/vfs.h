// Windows path <-> host path mapping.
//
//   C:\Dirt2\...   -> the game folder (also the initial current directory)
//   C:\...         -> <data dir>/C/...  (saves, settings, user profile)
//
// Lookups are case-insensitive so the game works on case-sensitive file
// systems (iOS). Paths the game sees use backslashes and a drive letter.
#pragma once

#include <string>

namespace vfs {

constexpr const char* kInstallDir = "C:\\Dirt2";

void set_data_dir(const std::string& d);
std::string data_dir();

// Absolute, normalised Windows path ("C:\a\b") from an absolute or relative one.
std::string full_path(const std::string& win);
// Host path for a Windows path; existing components resolved case-insensitively.
std::string host_path(const std::string& win);
// Notifies the case cache that a directory's contents changed.
void invalidate(const std::string& host_dir);

std::string cwd();
bool set_cwd(const std::string& win);

}  // namespace vfs
