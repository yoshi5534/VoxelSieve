#include "detail/convolution.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

#if defined(__x86_64__) || defined(_M_X64)
#define VOXELSIEVE_X86_64 1
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
// MSVC compiles AVX intrinsics without /arch:AVX2, which would let it use AVX2 everywhere.
#define VOXELSIEVE_AVX2
#else
#define VOXELSIEVE_AVX2 __attribute__((target("avx2,fma")))
#endif
#endif

namespace voxelsieve::detail {
namespace {

/// Adds weight * src[x] to the `Count` rows. The rows and the source never overlap, which lets the
/// compiler vectorise the loop.
template <int Count>
void accumulateRows(const std::array<float*, kConvolutionBlock>& rows, const float* weights,
                    const float* src, std::int64_t width) {
  const float* __restrict s = src;
  float* __restrict r0 = rows[0];
  float* __restrict r1 = rows[1];
  float* __restrict r2 = rows[2];
  float* __restrict r3 = rows[3];
  const float w0 = weights[0];
  const float w1 = weights[1];
  const float w2 = weights[2];
  const float w3 = weights[3];
  for (std::int64_t x = 0; x < width; ++x) {
    const float v = s[x];
    r0[x] += w0 * v;
    if constexpr (Count > 1) {
      r1[x] += w1 * v;
    }
    if constexpr (Count > 2) {
      r2[x] += w2 * v;
    }
    if constexpr (Count > 3) {
      r3[x] += w3 * v;
    }
  }
}

template <int Count>
void convolveRowPortable(const ConvolutionRow& row) {
  const int k = row.kernel;
  for (int b = 0; b < Count; ++b) {
    std::fill_n(row.dst[static_cast<std::size_t>(b)], row.width,
                row.bias[static_cast<std::size_t>(b)]);
  }
  const float* weights = row.weights;
  for (std::int64_t i = 0; i < row.in_channels; ++i) {
    for (int kz = 0; kz < k; ++kz) {
      for (int ky = 0; ky < k; ++ky) {
        const float* src = row.src + i * row.channel_stride + kz * row.plane + ky * row.row;
        for (int kx = 0; kx < k; ++kx) {
          accumulateRows<Count>(row.dst, weights, src + kx, row.width);
          weights += kConvolutionBlock;
        }
      }
    }
  }
  if (row.relu) {
    for (int b = 0; b < Count; ++b) {
      float* p = row.dst[static_cast<std::size_t>(b)];
      std::transform(p, p + row.width, p, [](float v) { return std::max(v, 0.0F); });
    }
  }
}

#ifdef VOXELSIEVE_X86_64

/// `Vectors` times 8 outputs of all four channels, which stay in 4 x Vectors registers over all
/// taps; with `Masked`, one vector of the first `mask` lanes.
template <int Vectors, bool Masked>
VOXELSIEVE_AVX2 void convolveStripAvx2(const ConvolutionRow& row, std::int64_t x0, __m256i mask) {
  constexpr std::int64_t kLanes = 8;
  const int k = row.kernel;
  std::array<std::array<__m256, Vectors>, kConvolutionBlock> acc{};
  for (int b = 0; b < kConvolutionBlock; ++b) {
    for (int v = 0; v < Vectors; ++v) {
      acc[b][v] = _mm256_set1_ps(row.bias[static_cast<std::size_t>(b)]);
    }
  }
  const float* weights = row.weights;
  for (std::int64_t i = 0; i < row.in_channels; ++i) {
    for (int kz = 0; kz < k; ++kz) {
      for (int ky = 0; ky < k; ++ky) {
        const float* src = row.src + i * row.channel_stride + kz * row.plane + ky * row.row + x0;
        for (int kx = 0; kx < k; ++kx) {
          std::array<__m256, Vectors> values{};
          for (int v = 0; v < Vectors; ++v) {
            values[v] = Masked ? _mm256_maskload_ps(src + kx + v * kLanes, mask)
                               : _mm256_loadu_ps(src + kx + v * kLanes);
          }
          for (int b = 0; b < kConvolutionBlock; ++b) {
            const __m256 w = _mm256_broadcast_ss(weights + b);
            for (int v = 0; v < Vectors; ++v) {
              acc[b][v] = _mm256_fmadd_ps(w, values[v], acc[b][v]);
            }
          }
          weights += kConvolutionBlock;
        }
      }
    }
  }
  const __m256 zero = _mm256_setzero_ps();
  for (int b = 0; b < row.count; ++b) {
    for (int v = 0; v < Vectors; ++v) {
      const __m256 result = row.relu ? _mm256_max_ps(acc[b][v], zero) : acc[b][v];
      float* dst = row.dst[static_cast<std::size_t>(b)] + x0 + v * kLanes;
      if (Masked) {
        _mm256_maskstore_ps(dst, mask, result);
      } else {
        _mm256_storeu_ps(dst, result);
      }
    }
  }
}

#endif

}  // namespace

void convolveRowPortable(const ConvolutionRow& row) {
  switch (row.count) {
    case 4:
      convolveRowPortable<4>(row);
      break;
    case 3:
      convolveRowPortable<3>(row);
      break;
    case 2:
      convolveRowPortable<2>(row);
      break;
    default:
      convolveRowPortable<1>(row);
  }
}

#ifdef VOXELSIEVE_X86_64

bool hasAvx2Convolution() {
#if defined(_MSC_VER) && !defined(__clang__)
  std::array<int, 4> info{};
  __cpuid(info.data(), 0);
  if (info[0] < 7) {
    return false;
  }
  __cpuid(info.data(), 1);
  const bool fma = (info[2] & (1 << 12)) != 0;
  const bool osxsave = (info[2] & (1 << 27)) != 0;
  const bool avx = (info[2] & (1 << 28)) != 0;
  // The operating system must save the AVX registers.
  if (!fma || !osxsave || !avx || (_xgetbv(0) & 6U) != 6U) {
    return false;
  }
  __cpuidex(info.data(), 7, 0);
  return (info[1] & (1 << 5)) != 0;
#else
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
}

VOXELSIEVE_AVX2 void convolveRowAvx2(const ConvolutionRow& row) {
  constexpr std::int64_t kLanes = 8;
  constexpr std::int64_t kWide = 3 * kLanes;
  std::int64_t x = 0;
  const __m256i all = _mm256_set1_epi32(-1);
  for (; x + kWide <= row.width; x += kWide) {
    convolveStripAvx2<3, false>(row, x, all);
  }
  for (; x + kLanes <= row.width; x += kLanes) {
    convolveStripAvx2<1, false>(row, x, all);
  }
  if (x < row.width) {
    const auto left = static_cast<int>(row.width - x);
    const __m256i lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256i mask = _mm256_cmpgt_epi32(_mm256_set1_epi32(left), lanes);
    convolveStripAvx2<1, true>(row, x, mask);
  }
}

#else

bool hasAvx2Convolution() { return false; }

void convolveRowAvx2(const ConvolutionRow& row) { convolveRowPortable(row); }

#endif

void convolveRow(const ConvolutionRow& row) {
  static const bool avx2 = hasAvx2Convolution();
  if (avx2) {
    convolveRowAvx2(row);
  } else {
    convolveRowPortable(row);
  }
}

}  // namespace voxelsieve::detail
