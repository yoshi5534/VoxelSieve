#pragma once

#include <filesystem>

namespace voxelsieve::detail {

/// Whether `path` lies on a network file system (SMB/CIFS, NFS, a mapped network drive or a UNC
/// path on Windows), where every page of a memory mapping is fetched with a request of its own.
/// False when it cannot be told.
[[nodiscard]] bool onNetworkShare(const std::filesystem::path& path);

}  // namespace voxelsieve::detail
