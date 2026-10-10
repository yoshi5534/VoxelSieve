#include "detail/network.hpp"

#include <cstdint>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#else
#include <sys/vfs.h>
#endif

namespace voxelsieve::detail {

bool onNetworkShare(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(path, error);
  if (error) {
    return false;
  }
#if defined(_WIN32)
  const std::wstring root = absolute.root_path().wstring();
  if (root.starts_with(L"\\\\") || root.starts_with(L"//")) {
    return true;  // \\server\share
  }
  return !root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
#elif defined(__APPLE__)
  struct statfs info {};
  if (statfs(absolute.c_str(), &info) != 0) {
    return false;
  }
  return (info.f_flags & MNT_LOCAL) == 0;
#else
  struct statfs info {};
  if (statfs(absolute.c_str(), &info) != 0) {
    return false;
  }
  // Magic numbers of the network file systems (linux/magic.h and the file systems' sources).
  constexpr std::uint32_t kSmb = 0x517B;
  constexpr std::uint32_t kCifs = 0xFF534D42;
  constexpr std::uint32_t kSmb2 = 0xFE534D42;
  constexpr std::uint32_t kNfs = 0x6969;
  constexpr std::uint32_t kAfs = 0x5346414F;
  constexpr std::uint32_t kCeph = 0x00C36400;
  const auto type = static_cast<std::uint32_t>(info.f_type);
  return type == kSmb || type == kCifs || type == kSmb2 || type == kNfs || type == kAfs ||
         type == kCeph;
#endif
}

}  // namespace voxelsieve::detail
