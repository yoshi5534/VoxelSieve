#pragma once

// The inner loop of the 3D convolution of learned models (model.cpp): one output row of up to
// four channels, portable or with AVX2 and FMA where the CPU has them.

#include <array>
#include <cstdint>

namespace voxelsieve::detail {

/// Output channels one row computes at once.
inline constexpr int kConvolutionBlock = 4;

/// One output row of up to four channels of a "same" convolution, read from the input padded by
/// kernel / 2 zero voxels on every side, so no tap needs a bounds check.
struct ConvolutionRow {
  /// Padded input of channel 0 at the row's first output voxel, shifted back by kernel / 2 on
  /// every axis: the first tap of the first output.
  const float* src = nullptr;
  std::int64_t in_channels = 0;
  /// Strides of the padded input, in floats.
  std::int64_t channel_stride = 0;
  std::int64_t plane = 0;
  std::int64_t row = 0;
  int kernel = 1;
  /// Weights of the four channels interleaved per tap: [((i * k + kz) * k + ky) * k + kx][b],
  /// zero for channels beyond `count`.
  const float* weights = nullptr;
  std::array<float, kConvolutionBlock> bias{};
  /// Channels to write, 1 to 4.
  int count = kConvolutionBlock;
  bool relu = false;
  std::array<float*, kConvolutionBlock> dst{};
  std::int64_t width = 0;
};

/// Computes the row with the fastest kernel this CPU runs. The sums run over the taps in the same
/// order in every kernel; with FMA they are rounded once per tap instead of twice.
void convolveRow(const ConvolutionRow& row);

/// The portable kernel, which compilers vectorise for the instructions every CPU of the target
/// has.
void convolveRowPortable(const ConvolutionRow& row);

/// Whether this CPU runs the AVX2 kernel (x86-64 with AVX2 and FMA).
[[nodiscard]] bool hasAvx2Convolution();

/// The AVX2 kernel; only where hasAvx2Convolution().
void convolveRowAvx2(const ConvolutionRow& row);

}  // namespace voxelsieve::detail
