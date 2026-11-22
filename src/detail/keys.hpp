#pragma once

#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace voxelsieve::detail {

/// Throws std::invalid_argument when `object` has a key that is not in `known`. Files that say
/// what to inspect (orders, plans) are read key by key, so a misspelled key would be left out
/// without notice: a limit that is not checked, a part that passes. The message names the key,
/// `where` (such as "inspection zone 'A'") and the known key nearest to it. Does nothing when
/// `object` is no object.
void requireKnownKeys(const nlohmann::json& object, std::initializer_list<std::string_view> known,
                      std::string_view where);

/// The key of `known` that `key` is most likely a misspelling of, or empty: at most two letters
/// added, left out or changed, and at most a third of the key.
/// With `affixes`, a key that is the beginning of a known one or the other way round counts as
/// well ("max_pore_size" for "max_pore_size_mm").
[[nodiscard]] std::string nearestKey(std::string_view key,
                                     std::initializer_list<std::string_view> known,
                                     bool affixes = true);

}  // namespace voxelsieve::detail
