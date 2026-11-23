#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace voxelsieve {

/// Reads `count` damaged copies of the file `original` with `read(path)`: bytes overwritten, bits
/// flipped, numbers set to their extremes, the file cut short or a piece of it repeated. A reader
/// of VoxelSieve's own formats must answer each with a result or an exception; a crash, or under
/// ASan a read outside a buffer or an allocation of the size a damaged header names, fails the
/// test. The damage is the same in every run.
template <typename Read>
void readDamagedCopies(const std::filesystem::path& original, int count, Read&& read) {
  std::ifstream in(original, std::ios::binary);
  ASSERT_TRUE(in) << original;
  const std::vector<char> intact((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  ASSERT_FALSE(intact.empty()) << original;
  const std::filesystem::path damaged =
      original.parent_path() / ("damaged_" + original.filename().string());
  std::mt19937 random(20261122U);  // NOLINT(cert-msc32-c,cert-msc51-cpp): repeatable on purpose
  const auto below = [&random](std::size_t limit) {
    return std::uniform_int_distribution<std::size_t>(0, limit - 1)(random);
  };
  int refused = 0;
  for (int i = 0; i < count; ++i) {
    std::vector<char> bytes = intact;
    // Most damage goes to the start, where the headers with sizes and counts are.
    const auto position = [&] {
      return below(4) == 0 ? below(bytes.size()) : below(std::min<std::size_t>(bytes.size(), 256));
    };
    switch (below(6)) {
      case 0:
        bytes[position()] = static_cast<char>(below(256));
        break;
      case 1:
        bytes[position()] ^= static_cast<char>(1U << below(8));
        break;
      case 2: {  // A number of four bytes at its extremes.
        const std::size_t at = position();
        const char value = below(2) == 0 ? '\xff' : '\0';
        for (std::size_t k = at; k < std::min(at + 4, bytes.size()); ++k) {
          bytes[k] = value;
        }
        if (below(2) == 0 && at + 3 < bytes.size()) {
          bytes[at + 3] = '\x7f';
        }
        break;
      }
      case 3:
        bytes.resize(below(bytes.size()));
        break;
      case 4: {  // A piece repeated.
        const std::size_t at = position();
        const std::size_t length = 1 + below(std::min<std::size_t>(64, bytes.size() - at));
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                     intact.begin() + static_cast<std::ptrdiff_t>(at),
                     intact.begin() + static_cast<std::ptrdiff_t>(at + length));
        break;
      }
      default:  // Several bytes at once.
        for (int k = 0; k < 8; ++k) {
          bytes[position()] = static_cast<char>(below(256));
        }
        break;
    }
    {
      std::ofstream out(damaged, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    try {
      read(damaged);
    } catch (const std::exception&) {
      ++refused;
    }
  }
  std::filesystem::remove(damaged);
  // A reader that accepted everything did not look at the file.
  EXPECT_GT(refused, 0) << original;
}

}  // namespace voxelsieve
