#pragma once

// How the samples of a raw file become 16-bit grey values (ADR 0044): shared by the raw sources
// and the HDF5 reader.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "voxelsieve/source.hpp"

namespace voxelsieve::detail {

/// One sample of type T from `p`, its bytes reversed when the file's byte order is not the
/// machine's. memcpy handles samples that are not aligned (headers of odd length).
template <class T>
T loadSample(const unsigned char* p, bool swap) {
  std::array<unsigned char, sizeof(T)> bytes{};
  std::memcpy(bytes.data(), p, sizeof(T));
  if (swap) {
    std::reverse(bytes.begin(), bytes.end());
  }
  T value{};
  std::memcpy(&value, bytes.data(), sizeof(T));
  return value;
}

/// Grey value of `value` under `mapping`, clipped to 0..65535; values outside and not a number
/// are counted in `outside`.
inline std::uint16_t mapToGrey(double value, const ValueMapping& mapping, std::uint64_t& outside) {
  const double grey = std::round(mapping.toGrey(value));
  if (!(grey >= 0.0 && grey <= 65535.0)) {
    ++outside;
    if (std::isnan(grey)) {
      return 0;
    }
  }
  return static_cast<std::uint16_t>(std::clamp(grey, 0.0, 65535.0));
}

template <class T>
void convertTyped(const unsigned char* samples, std::size_t count, bool swap, bool mapped,
                  const ValueMapping& mapping, std::uint16_t* target, std::uint64_t& outside) {
  for (std::size_t i = 0; i < count; ++i) {
    const T value = loadSample<T>(samples + i * sizeof(T), swap);
    if constexpr (std::is_same_v<T, std::int8_t>) {
      target[i] = static_cast<std::uint16_t>(value + 128);
    } else if constexpr (std::is_same_v<T, std::int16_t>) {
      target[i] = static_cast<std::uint16_t>(value + 32768);
    } else if constexpr (std::is_integral_v<T>) {
      if (mapped) {
        target[i] = mapToGrey(static_cast<double>(value), mapping, outside);
      } else if (value < 0 || value > 65535) {
        ++outside;
        target[i] = value < 0 ? 0 : 65535;
      } else {
        target[i] = static_cast<std::uint16_t>(value);
      }
    } else {
      target[i] = mapToGrey(static_cast<double>(value), mapping, outside);
    }
  }
}

/// How the samples of a raw file become 16-bit grey values (ADR 0044), and the count of values
/// that were clipped.
struct RawSamples {
  RawSampleType type = RawSampleType::kUInt16;
  bool swap = false;
  /// Wide samples mapped linearly over `range`; false for the types that are kept exactly.
  bool mapped = false;
  std::array<double, 2> range{0.0, 65535.0};
  ValueMapping mapping;
  mutable std::mutex mutex;
  mutable std::set<std::array<std::int64_t, 6>> counted;  // regions whose clipped values count
  mutable std::uint64_t clipped = 0;

  [[nodiscard]] std::size_t bytes() const { return rawSampleBytes(type); }

  /// Sets up the mapping. `sample` gives the values of slice z (or a part of it) for the
  /// estimate of the range; slices are taken from 0 to `last` (inclusive).
  void setUp(const RawLayout& layout, std::int64_t last,
             const std::function<std::vector<double>(std::int64_t)>& sample) {
    type = storedSampleType(layout);
    swap = rawSampleBytes(type) > 1 && layout.byte_order != std::endian::native;
    const bool wide = type == RawSampleType::kUInt32 || type == RawSampleType::kInt32 ||
                      type == RawSampleType::kFloat32 || type == RawSampleType::kFloat64;
    if (type == RawSampleType::kInt8) {
      mapping = {-128.0, 1.0};
    } else if (type == RawSampleType::kInt16) {
      mapping = {-32768.0, 1.0};
    }
    if (!wide) {
      if (layout.value_range) {
        throw std::invalid_argument("A value range applies only to 32-bit and float samples");
      }
      return;
    }
    const bool is_float = type == RawSampleType::kFloat32 || type == RawSampleType::kFloat64;
    if (layout.value_range) {
      range = *layout.value_range;
      mapped = true;
    } else {
      constexpr std::int64_t kSampleSlices = 33;
      const std::int64_t count = std::min(kSampleSlices, last + 1);
      std::vector<std::array<double, 2>> ranges(static_cast<std::size_t>(count));
      for (std::int64_t i = 0; i < count; ++i) {
        const std::int64_t z = count == 1 ? 0 : i * last / (count - 1);
        std::array<double, 2> found{std::numeric_limits<double>::infinity(),
                                    -std::numeric_limits<double>::infinity()};
        for (const double v : sample(z)) {
          if (std::isfinite(v)) {
            found = {std::min(found[0], v), std::max(found[1], v)};
          }
        }
        ranges[static_cast<std::size_t>(i)] = found;
      }
      std::array<double, 2> found{std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity()};
      for (const auto& r : ranges) {
        found = {std::min(found[0], r[0]), std::max(found[1], r[1])};
      }
      if (!(found[0] <= found[1])) {
        throw std::runtime_error("The raw file holds no finite values");
      }
      if (!is_float && found[0] >= 0.0 && found[1] <= 65535.0) {
        return;  // 32-bit integers that fit: kept exactly
      }
      const double margin = found[1] > found[0] ? 0.1 * (found[1] - found[0]) : 0.5;
      range = {found[0] - margin, found[1] + margin};
      if (!is_float) {
        range[0] = std::floor(range[0]);
        range[1] = std::ceil(range[1]);
      }
      mapped = true;
    }
    if (!(std::isfinite(range[0]) && std::isfinite(range[1]) && range[0] < range[1])) {
      throw std::invalid_argument("The value range must be two finite numbers, low before high");
    }
    mapping = {range[0], (range[1] - range[0]) / 65535.0};
  }

  /// Converts `count` samples; values outside the range are added to `outside`.
  void convert(const unsigned char* samples, std::size_t count, std::uint16_t* target,
               std::uint64_t& outside) const {
    switch (type) {
      case RawSampleType::kUInt8:
        std::copy(samples, samples + count, target);
        return;
      case RawSampleType::kUInt16:
        std::memcpy(target, samples, count * sizeof(std::uint16_t));
        if (swap) {
          std::transform(target, target + count, target, [](std::uint16_t v) {
            return static_cast<std::uint16_t>((v >> 8U) | (v << 8U));
          });
        }
        return;
      case RawSampleType::kInt8:
        convertTyped<std::int8_t>(samples, count, swap, mapped, mapping, target, outside);
        return;
      case RawSampleType::kInt16:
        convertTyped<std::int16_t>(samples, count, swap, mapped, mapping, target, outside);
        return;
      case RawSampleType::kUInt32:
        convertTyped<std::uint32_t>(samples, count, swap, mapped, mapping, target, outside);
        return;
      case RawSampleType::kInt32:
        convertTyped<std::int32_t>(samples, count, swap, mapped, mapping, target, outside);
        return;
      case RawSampleType::kFloat32:
        convertTyped<float>(samples, count, swap, mapped, mapping, target, outside);
        return;
      case RawSampleType::kFloat64:
        convertTyped<double>(samples, count, swap, mapped, mapping, target, outside);
        return;
    }
  }

  /// The values of `count` samples, for the estimate of the range.
  [[nodiscard]] std::vector<double> values(const unsigned char* samples, std::size_t count) const {
    std::vector<double> result(count);
    for (std::size_t i = 0; i < count; ++i) {
      const unsigned char* p = samples + i * bytes();
      switch (type) {
        case RawSampleType::kUInt32:
          result[i] = loadSample<std::uint32_t>(p, swap);
          break;
        case RawSampleType::kInt32:
          result[i] = loadSample<std::int32_t>(p, swap);
          break;
        case RawSampleType::kFloat32:
          result[i] = loadSample<float>(p, swap);
          break;
        case RawSampleType::kFloat64:
          result[i] = loadSample<double>(p, swap);
          break;
        default:
          result[i] = 0.0;
      }
    }
    return result;
  }

  /// Adds the clipped values of a region read, once per region.
  void count(const Box& box, std::uint64_t outside) const {
    if (outside == 0) {
      return;
    }
    const std::scoped_lock lock(mutex);
    if (counted.insert({box.min[0], box.min[1], box.min[2], box.max[0], box.max[1], box.max[2]})
            .second) {
      clipped += outside;
    }
  }

  [[nodiscard]] SampleType greyType() const {
    return type == RawSampleType::kUInt8 || type == RawSampleType::kInt8 ? SampleType::kUInt8
                                                                         : SampleType::kUInt16;
  }
};

}  // namespace voxelsieve::detail
