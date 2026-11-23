// The layout of raw volume files without a sidecar (ADR 0039): from the file name, else from the
// grey values.

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "voxelsieve/source.hpp"

namespace voxelsieve {
namespace {

/// What a file name says about the layout.
struct NameHints {
  std::optional<std::array<std::int64_t, 3>> dims;
  std::optional<SampleType> sample_type;
  bool big_endian = false;
  std::optional<double> voxel_size_mm;
};

std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) {
    return static_cast<char>(c < 0x80 ? std::tolower(c) : c);
  });
  return text;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isLetter(char c) { return c >= 'a' && c <= 'z'; }

/// The words of a lower-case name: runs of letters and digits, with the characters between them.
struct Word {
  std::string text;
  std::string before;  // the separator before it
};

std::vector<Word> wordsOf(const std::string& text) {
  std::vector<Word> words;
  std::string separator;
  std::size_t i = 0;
  while (i < text.size()) {
    if (isDigit(text[i]) || isLetter(text[i])) {
      Word word{.text = {}, .before = separator};
      while (i < text.size() && (isDigit(text[i]) || isLetter(text[i]))) {
        word.text += text[i++];
      }
      words.push_back(std::move(word));
      separator.clear();
    } else {
      separator += text[i++];
    }
  }
  return words;
}

/// The bits a word names: "16bit", "uint8", "u16", "int16"; with `bits_follow` also a bare
/// "16" or "uint16" before the word "bit".
std::optional<int> bitsOf(std::string word, bool bits_follow) {
  bool typed = false;
  for (const std::string prefix : {"uint", "int", "u"}) {
    if (word.starts_with(prefix) && word.size() > prefix.size() && isDigit(word[prefix.size()])) {
      word = word.substr(prefix.size());
      typed = true;
      break;
    }
  }
  std::size_t digits = 0;
  while (digits < word.size() && isDigit(word[digits])) {
    ++digits;
  }
  const std::string number = word.substr(0, digits);
  const std::string rest = word.substr(digits);
  if (number != "8" && number != "16" && number != "32") {
    return std::nullopt;
  }
  if (rest == "bit" || rest == "bits" || (rest.empty() && (typed || bits_follow))) {
    return std::stoi(number);
  }
  return std::nullopt;
}

/// Reads the hints of one name; `hints` keeps what an earlier name gave.
void readName(const std::string& name, NameHints& hints) {
  const std::string text = lower(name);
  const auto number_at = [&](std::size_t& i) -> std::optional<std::int64_t> {
    const std::size_t start = i;
    while (i < text.size() && isDigit(text[i]) && i - start < 7) {
      ++i;
    }
    if (i == start || i - start > 6) {
      return std::nullopt;
    }
    return std::stoll(text.substr(start, i - start));
  };
  const auto skip_spaces = [&](std::size_t& i) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
      ++i;
    }
  };
  const auto times_at = [&](std::size_t& i) {
    if (i < text.size() && text[i] == 'x') {
      ++i;
      return true;
    }
    if (text.compare(i, 2, "\xC3\x97") == 0) {  // ×
      i += 2;
      return true;
    }
    return false;
  };
  // 512x512x400, 512 x 512 x 400, 512×512×400 (UTF-8).
  for (std::size_t start = 0; start < text.size() && !hints.dims; ++start) {
    if (!isDigit(text[start]) || (start > 0 && isDigit(text[start - 1]))) {
      continue;
    }
    std::size_t i = start;
    std::array<std::int64_t, 3> found{};
    bool complete = true;
    for (std::size_t axis = 0; axis < 3 && complete; ++axis) {
      if (axis > 0) {
        skip_spaces(i);
        complete = times_at(i);
        skip_spaces(i);
      }
      const auto n = complete ? number_at(i) : std::nullopt;
      complete = complete && n && *n > 0;
      if (complete) {
        found[axis] = *n;
      }
    }
    if (complete) {
      hints.dims = found;
    }
  }

  const std::vector<Word> words = wordsOf(text);
  for (std::size_t w = 0; w < words.size(); ++w) {
    const bool bits_follow =
        w + 1 < words.size() && (words[w + 1].text == "bit" || words[w + 1].text == "bits") &&
        (words[w + 1].before == "-" || words[w + 1].before == "_" || words[w + 1].before == " ");
    const std::string& word = words[w].text;
    if (!hints.sample_type) {
      if (const auto bits = bitsOf(word, bits_follow)) {
        if (*bits == 32) {
          throw std::invalid_argument(
              "The name says 32 bit: raw files of 32-bit or float values are not read; export "
              "the scan as a float TIFF stack, which is imported with a recorded value mapping");
        }
        hints.sample_type = *bits == 8 ? SampleType::kUInt8 : SampleType::kUInt16;
      } else if (word == "float" || word == "float32" || word == "real32") {
        throw std::invalid_argument(
            "The name says float: raw files of float values are not read; export the scan as a "
            "float TIFF stack, which is imported with a recorded value mapping");
      }
    }
    const bool endian_follows =
        w + 1 < words.size() && words[w + 1].text == "endian" &&
        (words[w + 1].before == "-" || words[w + 1].before == "_" || words[w + 1].before == " ");
    if (word == "bigendian" || word == "be" || word == "msb" || (word == "big" && endian_follows)) {
      hints.big_endian = true;
    }
  }

  // 0.05mm, 0,05 mm, 50um, 50µm, 50μm, 50 micron.
  for (std::size_t start = 0; start < text.size() && !hints.voxel_size_mm; ++start) {
    if (!isDigit(text[start]) || (start > 0 && isDigit(text[start - 1]))) {
      continue;
    }
    std::size_t i = start;
    while (i < text.size() && isDigit(text[i])) {
      ++i;
    }
    if (i + 1 < text.size() && (text[i] == '.' || text[i] == ',') && isDigit(text[i + 1])) {
      ++i;
      while (i < text.size() && isDigit(text[i])) {
        ++i;
      }
    }
    std::string number = text.substr(start, i - start);
    std::ranges::replace(number, ',', '.');
    skip_spaces(i);
    double scale = 0.0;
    for (const auto& [unit, factor] :
         std::array<std::pair<std::string_view, double>, 6>{{{"mm", 1.0},
                                                             {"um", 1e-3},
                                                             {"\xC2\xB5m", 1e-3},
                                                             {"\xCE\xBCm", 1e-3},
                                                             {"microns", 1e-3},
                                                             {"micron", 1e-3}}}) {
      if (text.compare(i, unit.size(), unit) == 0 &&
          (i + unit.size() == text.size() || !isLetter(text[i + unit.size()]))) {
        scale = factor;
        break;
      }
    }
    if (scale > 0.0) {
      const double mm = std::stod(number) * scale;
      if (mm >= 1e-4 && mm <= 10.0) {
        hints.voxel_size_mm = mm;
      }
    }
  }
}

/// Bytes of `path` from `offset` (fewer at the end of the file).
std::vector<std::uint8_t> readBytes(std::ifstream& in, std::uint64_t offset, std::size_t count) {
  std::vector<std::uint8_t> bytes(count);
  in.clear();
  in.seekg(static_cast<std::streamoff>(offset));
  in.read(
      reinterpret_cast<char*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      static_cast<std::streamsize>(count));
  bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
  return bytes;
}

/// Samples of `bytes` as one layout reads them.
std::vector<float> decode(const std::vector<std::uint8_t>& bytes, SampleType type,
                          bool big_endian) {
  if (type == SampleType::kUInt8) {
    return {bytes.begin(), bytes.end()};
  }
  std::vector<float> values(bytes.size() / 2);
  for (std::size_t i = 0; i < values.size(); ++i) {
    const unsigned first = bytes[2 * i];
    const unsigned second = bytes[(2 * i) + 1];
    values[i] = static_cast<float>(big_endian ? (first << 8U) | second : (second << 8U) | first);
  }
  return values;
}

/// Mean absolute difference of the values `lag` apart.
double difference(const std::vector<float>& values, std::size_t lag) {
  if (lag >= values.size()) {
    return std::numeric_limits<double>::infinity();
  }
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t i = 0; i + lag < values.size(); ++i) {
    sum += std::abs(values[i] - values[i + lag]);
    ++count;
  }
  return sum / static_cast<double>(count);
}

/// Mean absolute difference of every other byte to the one two further, against that of bytes
/// half the sample apart: low for a byte that varies smoothly (the high byte of 16-bit values, or
/// every byte of 8-bit values), near 1 for the noisy low byte of 16-bit values. `odd` chooses the
/// bytes at odd positions. Negative when the byte never changes.
double byteRoughness(const std::vector<std::uint8_t>& bytes, bool odd) {
  const std::size_t start = odd ? 1 : 0;
  const std::size_t half = (bytes.size() / 4) * 2;
  double near = 0.0;
  double far = 0.0;
  std::size_t count = 0;
  for (std::size_t i = start; i + half < bytes.size() && i + 2 < bytes.size(); i += 2) {
    near += std::abs(int{bytes[i]} - int{bytes[i + 2]});
    far += std::abs(int{bytes[i]} - int{bytes[i + half]});
    ++count;
  }
  if (count == 0 || far == 0.0) {
    return -1.0;
  }
  return near / far;
}

/// Sample type and byte order from which bytes vary smoothly.
std::pair<SampleType, bool> detectSampleType(const std::vector<std::uint8_t>& bytes) {
  const double even = byteRoughness(bytes, false);
  const double odd = byteRoughness(bytes, true);
  // A byte that never changes is the high byte of small 16-bit values.
  if (even < 0.0 && odd < 0.0) {
    return {SampleType::kUInt16, false};
  }
  if (even < 0.0 || odd < 0.0) {
    return {SampleType::kUInt16, even < 0.0};
  }
  // 8-bit values: every byte is a voxel, so neighbouring bytes are as alike as bytes two apart.
  // The two bytes of 16-bit values differ much more from each other than from the same byte of
  // the next value, even where both are smooth (a scan without noise).
  double next = 0.0;
  double second = 0.0;
  for (std::size_t i = 0; i + 2 < bytes.size(); ++i) {
    next += std::abs(int{bytes[i]} - int{bytes[i + 1]});
    second += std::abs(int{bytes[i]} - int{bytes[i + 2]});
  }
  constexpr double kSmooth = 0.6;
  constexpr double kNeighbours = 1.2;
  if (even < kSmooth && odd < kSmooth && std::abs(even - odd) < 0.25 * std::max(even, odd) &&
      next <= kNeighbours * second) {
    return {SampleType::kUInt8, false};
  }
  // The smoother byte is the high one: second in little-endian order.
  return {SampleType::kUInt16, even < odd};
}

/// The offset (between `min_lag` and `max_lag`) at which the values of `samples` are most alike,
/// and how alike: the difference there against the median over all offsets.
struct Period {
  std::size_t lag = 0;
  double ratio = 1.0;
};

/// Whether voxels `difference` apart are about as alike as those at the best offset (`best`):
/// within 30 %, or close to it against the difference of unrelated voxels (`unrelated`), which
/// matters for scans with little noise, where both are near 0.
bool alike(double difference, double best, double unrelated) {
  constexpr double kRelative = 1.3;
  constexpr double kAbsolute = 0.15;
  return difference <= kRelative * best || difference <= best + (kAbsolute * unrelated);
}

Period findRowLength(const std::vector<std::vector<float>>& samples, std::size_t min_lag,
                     std::size_t max_lag) {
  // Coarse: every offset on a sparse set of positions; then the best ones on all positions.
  // Coarse: 8 runs of 1024 voxels per sample, read in order, which keeps it fast.
  constexpr std::size_t kRuns = 8;
  constexpr std::size_t kRun = 1024;
  std::vector<std::pair<double, std::size_t>> coarse;
  for (std::size_t lag = min_lag; lag <= max_lag; ++lag) {
    double sum = 0.0;
    for (const auto& values : samples) {
      const std::size_t spacing = (values.size() - lag) / kRuns;
      double run_sum = 0.0;
      for (std::size_t run = 0; run < kRuns; ++run) {
        const std::size_t first = run * spacing;
        const std::size_t end = std::min(first + kRun, values.size() - lag);
        for (std::size_t i = first; i < end; ++i) {
          run_sum += std::abs(values[i] - values[i + lag]);
        }
      }
      sum += run_sum / static_cast<double>(kRuns * kRun);
    }
    coarse.emplace_back(sum, lag);
  }
  if (coarse.empty()) {
    return {};
  }
  std::vector<double> sums(coarse.size());
  std::ranges::transform(coarse, sums.begin(), [](const auto& entry) { return entry.first; });
  std::ranges::nth_element(sums, sums.begin() + static_cast<std::ptrdiff_t>(sums.size() / 2));
  const double median = sums[sums.size() / 2];
  constexpr std::size_t kRefined = 24;
  std::ranges::partial_sort(
      coarse, coarse.begin() + static_cast<std::ptrdiff_t>(std::min(kRefined, coarse.size())));
  const auto exact = [&](std::size_t lag) {
    double sum = 0.0;
    for (const auto& values : samples) {
      sum += difference(values, lag);
    }
    return sum;
  };
  Period best{0, std::numeric_limits<double>::infinity()};
  double best_sum = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < std::min(kRefined, coarse.size()); ++i) {
    const double sum = exact(coarse[i].second);
    if (sum < best_sum) {
      best_sum = sum;
      best.lag = coarse[i].second;
    }
  }
  // Rows (and slices) further apart are alike too: the row length is the shortest offset that
  // divides the best one and is about as alike.
  for (std::size_t divisor = min_lag; divisor < best.lag; ++divisor) {
    if (best.lag % divisor == 0) {
      const double sum = exact(divisor);
      if (alike(sum, best_sum, median)) {
        best.lag = divisor;
        best_sum = sum;
        break;
      }
    }
  }
  best.ratio = median > 0.0 ? best_sum / median : 1.0;
  return best;
}

/// Size of the voxel data with `slice_voxels` per slice in a file of `file_bytes`: whole slices,
/// with what is left over taken as a header.
struct Fit {
  std::int64_t slices = 0;
  std::uint64_t header = 0;
};

Fit fitSlices(std::uint64_t file_bytes, std::uint64_t slice_bytes) {
  Fit fit;
  fit.slices = static_cast<std::int64_t>(file_bytes / slice_bytes);
  fit.header = file_bytes - (static_cast<std::uint64_t>(fit.slices) * slice_bytes);
  return fit;
}

constexpr std::uint64_t kMaxHeaderBytes = std::uint64_t{1} << 20U;
constexpr std::int64_t kMinEdge = 8;

std::optional<RawLayoutGuess> fromGreyValues(std::ifstream& in, std::uint64_t file_bytes,
                                             const NameHints& hints) {
  constexpr std::size_t kSampleBytes = std::size_t{4} << 20U;
  const std::size_t sample_bytes =
      static_cast<std::size_t>(std::min<std::uint64_t>(kSampleBytes, file_bytes / 4)) & ~1ULL;
  if (sample_bytes < 4096) {
    return std::nullopt;
  }
  // Three places inside the file, where the part rather than the air around it lies.
  std::vector<std::vector<std::uint8_t>> chunks;
  for (const double at : {0.35, 0.5, 0.65}) {
    const auto offset = static_cast<std::uint64_t>(at * static_cast<double>(file_bytes)) & ~1ULL;
    chunks.push_back(readBytes(in, offset, sample_bytes));
  }
  std::vector<std::uint8_t> all;
  for (const auto& chunk : chunks) {
    all.insert(all.end(), chunk.begin(), chunk.end());
  }
  auto [type, big_endian] = detectSampleType(all);
  if (hints.sample_type) {
    type = *hints.sample_type;
  }
  if (hints.big_endian) {
    big_endian = true;
  }
  const std::uint64_t bytes_per_voxel = type == SampleType::kUInt8 ? 1 : 2;

  std::vector<std::vector<float>> samples;
  samples.reserve(chunks.size());
  for (const auto& chunk : chunks) {
    samples.push_back(decode(chunk, type, big_endian));
  }
  const std::size_t voxels = samples.front().size();
  const Period row = findRowLength(samples, kMinEdge, std::min<std::size_t>(16384, voxels / 8));
  if (row.lag == 0) {
    return std::nullopt;
  }
  const auto width = static_cast<std::int64_t>(row.lag);
  const std::uint64_t row_bytes = static_cast<std::uint64_t>(row.lag) * bytes_per_voxel;

  // Slices: every height whose slices fill the file up to a header, compared at that offset.
  const std::uint64_t total_rows = file_bytes / row_bytes;
  constexpr std::int64_t kMinSlices = 4;
  constexpr std::size_t kBlockBytes = std::size_t{64} << 10U;
  constexpr std::size_t kCoarseBytes = std::size_t{8} << 10U;
  const std::size_t block_bytes = std::min<std::size_t>(kBlockBytes, sample_bytes);
  const auto middle = static_cast<std::uint64_t>(0.4 * static_cast<double>(file_bytes)) & ~1ULL;
  const std::vector<float> base = decode(readBytes(in, middle, block_bytes), type, big_endian);
  // Mean difference of the first `bytes` of the base block to those `lag_bytes` further.
  const auto compare = [&](std::uint64_t lag_bytes, std::size_t bytes) {
    const std::vector<float> other =
        decode(readBytes(in, middle + lag_bytes, bytes), type, big_endian);
    double sum = 0.0;
    const std::size_t count = std::min<std::size_t>(other.size(), bytes / bytes_per_voxel);
    for (std::size_t i = 0; i < count; ++i) {
      sum += std::abs(base[i] - other[i]);
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
  };
  std::vector<std::pair<double, std::int64_t>> coarse;
  std::vector<double> all_differences;
  for (std::int64_t height = kMinEdge; height <= static_cast<std::int64_t>(total_rows) / kMinSlices;
       ++height) {
    const Fit fit = fitSlices(file_bytes, static_cast<std::uint64_t>(height) * row_bytes);
    if (fit.header > std::min<std::uint64_t>(kMaxHeaderBytes,
                                             static_cast<std::uint64_t>(height) * row_bytes) ||
        fit.header % bytes_per_voxel != 0) {
      continue;
    }
    const std::uint64_t lag_bytes = static_cast<std::uint64_t>(height) * row_bytes;
    if (middle + lag_bytes + block_bytes > file_bytes) {
      continue;
    }
    const double mean = compare(lag_bytes, std::min(kCoarseBytes, block_bytes));
    coarse.emplace_back(mean, height);
    all_differences.push_back(mean);
  }
  if (coarse.empty()) {
    return std::nullopt;
  }
  // The best heights, their divisors and a square slice again on the whole block.
  std::vector<std::pair<double, std::int64_t>> sorted = coarse;
  constexpr std::size_t kRefinedHeights = 16;
  std::ranges::partial_sort(sorted, sorted.begin() + static_cast<std::ptrdiff_t>(
                                                         std::min(kRefinedHeights, sorted.size())));
  sorted.resize(std::min(kRefinedHeights, sorted.size()));
  std::vector<std::pair<double, std::int64_t>> heights;
  for (const auto& [mean, height] : coarse) {
    const bool chosen = height == width || std::ranges::any_of(sorted, [&](const auto& best) {
                          return best.second % height == 0;
                        });
    if (chosen) {
      heights.emplace_back(compare(static_cast<std::uint64_t>(height) * row_bytes, block_bytes),
                           height);
    }
  }
  if (heights.empty()) {
    return std::nullopt;
  }
  // Far apart: the same block against a block a quarter of the file further.
  const std::vector<float> far =
      decode(readBytes(in, (middle + file_bytes / 4) & ~1ULL, block_bytes), type, big_endian);
  double far_sum = 0.0;
  const std::size_t far_count = std::min(base.size(), far.size());
  for (std::size_t i = 0; i < far_count; ++i) {
    far_sum += std::abs(base[i] - far[i]);
  }
  const double far_mean = far_count > 0 ? far_sum / static_cast<double>(far_count) : 0.0;
  const double reference = std::max(far_mean, [&] {
    std::ranges::nth_element(
        all_differences,
        all_differences.begin() + static_cast<std::ptrdiff_t>(all_differences.size() / 2));
    return all_differences[all_differences.size() / 2];
  }());
  const auto best = std::ranges::min_element(heights);
  std::int64_t height = best->second;
  double height_difference = best->first;
  // Slices two apart are alike too: the slice size is the smallest such height; a square slice
  // wins a near tie.
  for (const auto& [mean, candidate] : heights) {
    if (candidate < height && height % candidate == 0 && alike(mean, best->first, reference)) {
      height = candidate;
      height_difference = mean;
      break;
    }
  }
  for (const auto& [mean, candidate] : heights) {
    if (candidate == width && candidate != height && mean <= 1.1 * height_difference) {
      height = candidate;
      height_difference = mean;
    }
  }
  const double slice_ratio = reference > 0.0 ? height_difference / reference : 1.0;
  const double confidence = 1.0 - std::max(row.ratio, slice_ratio);
  constexpr double kMinConfidence = 0.3;
  if (confidence < kMinConfidence) {
    return std::nullopt;
  }
  const Fit fit = fitSlices(file_bytes, static_cast<std::uint64_t>(height) * row_bytes);
  RawLayoutGuess guess;
  guess.layout.dims = {width, height, fit.slices};
  guess.layout.sample_type = type;
  guess.layout.byte_order = big_endian ? std::endian::big : std::endian::little;
  guess.layout.header_bytes = fit.header;
  guess.dims_from = "grey values";
  guess.confidence = confidence;
  return guess;
}

/// The file name without extensions such as .raw.gz, but with the ".05mm" of "scan_0.05mm".
std::string scanStem(const std::filesystem::path& path) {
  std::filesystem::path stem = path.filename();
  while (stem.extension().string().size() > 1 && stem.extension().string().size() <= 5 &&
         std::isdigit(static_cast<unsigned char>(stem.extension().string()[1])) == 0) {
    stem = stem.stem();
  }
  return stem.string();
}

}  // namespace

std::optional<RawLayoutGuess> detectRawLayout(const std::filesystem::path& path) {
  NameHints hints;
  readName(scanStem(path), hints);
  readName(path.parent_path().filename().string(), hints);

  const std::uint64_t file_bytes = std::filesystem::file_size(path);
  const bool gzip = isGzipFile(path);
  std::optional<RawLayoutGuess> guess;
  if (hints.dims) {
    const auto& dims = *hints.dims;
    const auto voxels = static_cast<std::uint64_t>(dims[0] * dims[1] * dims[2]);
    const auto slice = static_cast<std::uint64_t>(dims[0] * dims[1]);
    std::optional<SampleType> type = hints.sample_type;
    if (!type && !gzip) {
      // The size tells 8 from 16 bits: the voxel data fills the file up to a header.
      if (2 * voxels <= file_bytes &&
          file_bytes - (2 * voxels) <= std::max(kMaxHeaderBytes, 2 * slice)) {
        type = SampleType::kUInt16;
      } else if (voxels <= file_bytes && file_bytes - voxels <= std::max(kMaxHeaderBytes, slice)) {
        type = SampleType::kUInt8;
      }
    }
    if (!type && !gzip) {
      throw std::invalid_argument("The name says " + std::to_string(dims[0]) + " x " +
                                  std::to_string(dims[1]) + " x " + std::to_string(dims[2]) +
                                  " voxels, which do not fit the file of " +
                                  std::to_string(file_bytes) + " bytes");
    }
    {
      guess.emplace();
      guess->layout.dims = dims;
      guess->layout.sample_type = type.value_or(SampleType::kUInt16);
      guess->dims_from = "file name";
      guess->confidence = 1.0;
      if (!gzip) {
        const std::uint64_t data =
            voxels * (guess->layout.sample_type == SampleType::kUInt8 ? 1 : 2);
        if (data > file_bytes) {
          throw std::invalid_argument("The name says " + std::to_string(dims[0]) + " x " +
                                      std::to_string(dims[1]) + " x " + std::to_string(dims[2]) +
                                      " voxels, more than the file holds");
        }
        guess->layout.header_bytes = file_bytes - data;
      }
    }
  }
  if (!gzip) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      throw std::runtime_error("Cannot read " + path.string());
    }
    if (!guess) {
      guess = fromGreyValues(in, file_bytes, hints);
    } else if (guess->layout.sample_type == SampleType::kUInt16 && !hints.big_endian) {
      // The byte order from the grey values when the name does not give it.
      const auto middle = (file_bytes / 2) & ~1ULL;
      const auto bytes = readBytes(in, middle,
                                   static_cast<std::size_t>(std::min<std::uint64_t>(
                                       std::uint64_t{1} << 20U, file_bytes / 4)) &
                                       ~std::size_t{1});
      if (bytes.size() >= 64 && detectSampleType(bytes) == std::pair{SampleType::kUInt16, true}) {
        guess->layout.byte_order = std::endian::big;
      }
    }
  }
  if (!guess) {
    return std::nullopt;
  }
  if (hints.big_endian) {
    guess->layout.byte_order = std::endian::big;
  }
  if (hints.voxel_size_mm) {
    guess->layout.voxel_size = VoxelSize(*hints.voxel_size_mm);
    guess->voxel_size_known = true;
  }
  return guess;
}

std::optional<VoxelSize> voxelSizeFromName(const std::filesystem::path& path) {
  NameHints hints;
  try {
    readName(scanStem(path), hints);
    readName(path.parent_path().filename().string(), hints);
  } catch (const std::invalid_argument&) {
    return std::nullopt;  // a name that says float; the voxel size is all that is asked for
  }
  if (!hints.voxel_size_mm) {
    return std::nullopt;
  }
  return VoxelSize(*hints.voxel_size_mm);
}

}  // namespace voxelsieve
