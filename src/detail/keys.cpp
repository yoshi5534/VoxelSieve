#include "detail/keys.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace voxelsieve::detail {
namespace {

/// Edits (insert, delete, replace one character) that turn `a` into `b`.
std::size_t editDistance(std::string_view a, std::string_view b) {
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) {
    row[j] = j;
  }
  for (std::size_t i = 1; i <= a.size(); ++i) {
    std::size_t diagonal = row[0];
    row[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t above = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0U : 1U)});
      diagonal = above;
    }
  }
  return row[b.size()];
}

}  // namespace

std::string nearestKey(std::string_view key, std::initializer_list<std::string_view> known,
                       bool affixes) {
  std::string_view nearest;
  std::size_t best = 3;
  for (const std::string_view candidate : known) {
    // A key that only lacks or adds its unit ("max_pore_size" for "max_pore_size_mm") is the
    // commonest slip and more than two edits away.
    const bool affix = affixes && (candidate.size() > key.size() ? candidate.starts_with(key)
                                                                 : key.starts_with(candidate));
    const std::size_t distance = affix ? 1 : editDistance(key, candidate);
    if (distance < best && (affix || 3 * distance <= std::max(key.size(), candidate.size()))) {
      best = distance;
      nearest = candidate;
    }
  }
  return std::string(nearest);
}

void requireKnownKeys(const nlohmann::json& object, std::initializer_list<std::string_view> known,
                      std::string_view where) {
  if (!object.is_object()) {
    return;
  }
  for (const auto& [key, value] : object.items()) {
    if (std::find(known.begin(), known.end(), key) != known.end()) {
      continue;
    }
    std::string message = "Unknown key '" + key + "' in " + std::string(where);
    if (const std::string nearest = nearestKey(key, known); !nearest.empty()) {
      message += "; did you mean '" + nearest + "'?";
    } else {
      message += "; known keys:";
      for (const std::string_view name : known) {
        message += " " + std::string(name);
      }
    }
    throw std::invalid_argument(message);
  }
}

}  // namespace voxelsieve::detail
