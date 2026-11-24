// MetaImage and NRRD volumes (ADR 0042): the text header is read here, the voxels by the raw
// sources of source.cpp.

#include "voxelsieve/header_volume.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace voxelsieve {
namespace {

/// The longest header read: headers are a few hundred bytes, lists of slice files longer.
constexpr std::size_t kMaxHeaderBytes = std::size_t{64} << 20U;

std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) {
    return static_cast<char>(c < 0x80 ? std::tolower(c) : c);
  });
  return text;
}

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string extensionOf(const std::filesystem::path& path) {
  return lower(path.extension().string());
}

/// Reads lines of the header from the start of the file. `end` receives the offset after the
/// last line returned; `stop` says whether a line ends the header (it is returned too).
template <class Stop>
std::vector<std::string> headerLines(const std::filesystem::path& path, std::uint64_t& end,
                                     Stop stop) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot read " + path.string());
  }
  std::vector<std::string> lines;
  std::string line;
  end = 0;
  while (std::getline(in, line)) {
    end += line.size() + 1;
    if (end > kMaxHeaderBytes) {
      throw std::invalid_argument(path.string() + ": the header is longer than 64 MiB");
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
    if (stop(line)) {
      return lines;
    }
  }
  // The file ended within the header (a detached header without a last newline).
  end = std::min<std::uint64_t>(end, std::filesystem::file_size(path));
  return lines;
}

std::vector<double> numbers(const std::string& text, const std::string& what) {
  std::vector<double> values;
  std::string cleaned = text;
  for (char& c : cleaned) {
    if (c == '(' || c == ')' || c == ',') {
      c = ' ';
    }
  }
  std::istringstream in(cleaned);
  std::string word;
  while (in >> word) {
    try {
      std::size_t used = 0;
      values.push_back(std::stod(word, &used));
      if (used != word.size()) {
        throw std::invalid_argument(word);
      }
    } catch (const std::exception&) {
      if (lower(word) == "nan") {
        values.push_back(std::numeric_limits<double>::quiet_NaN());
      } else {
        std::string message = "'" + what;
        message += "' is not a list of numbers: ";
        message += text;
        throw std::invalid_argument(message);
      }
    }
  }
  return values;
}

std::array<std::int64_t, 3> dimsOf(const std::vector<double>& sizes, const std::string& what) {
  if (sizes.size() < 2 || sizes.size() > 3) {
    throw std::invalid_argument(what + ": volumes of two or three dimensions are read, not " +
                                std::to_string(sizes.size()));
  }
  std::array<std::int64_t, 3> dims{1, 1, 1};
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    if (!(sizes[i] >= 1.0) || sizes[i] != std::floor(sizes[i]) ||
        sizes[i] > static_cast<double>(kMaxVolumeEdge)) {
      throw std::invalid_argument(what + ": the sizes must be whole numbers from 1 to " +
                                  std::to_string(kMaxVolumeEdge));
    }
    dims[i] = static_cast<std::int64_t>(sizes[i]);
  }
  return dims;
}

void setSampleType(RawLayout& layout, RawSampleType type) {
  if (type == RawSampleType::kUInt8) {
    layout.sample_type = SampleType::kUInt8;
  } else if (type == RawSampleType::kUInt16) {
    layout.sample_type = SampleType::kUInt16;
  } else {
    layout.stored_type = type;
  }
}

/// The pose from the directions of the axes (one unit vector or scaled vector per axis) and
/// the position of voxel 0; the spacing is the length of each vector.
void setPose(HeaderVolume& volume, const std::array<std::array<double, 3>, 3>& axes,
             const std::array<double, 3>& origin) {
  std::array<double, 9> rotation{};
  for (std::size_t c = 0; c < 3; ++c) {
    const auto& axis = axes[c];
    const double length = std::hypot(axis[0], axis[1], axis[2]);
    for (std::size_t r = 0; r < 3; ++r) {
      rotation[r * 3 + c] = length > 0.0 ? axis[r] / length : (r == c ? 1.0 : 0.0);
    }
  }
  volume.pose.translation = origin;
  const std::array<double, 12> matrix{rotation[0], rotation[1], rotation[2], origin[0],
                                      rotation[3], rotation[4], rotation[5], origin[1],
                                      rotation[6], rotation[7], rotation[8], origin[2]};
  try {
    volume.pose = RigidTransform::fromMatrix(matrix);
  } catch (const std::exception&) {
    // Mirrored or skewed axes are no rigid motion: only the position is kept.
    volume.mirrored = true;
    volume.notes.emplace_back(
        "The axes of the header are mirrored or not at right angles; the volume is placed at "
        "its origin without turning it.");
  }
}

/// Files of a numbered pattern such as "slice%03d.raw 1 400 1", relative to `dir`.
std::vector<std::filesystem::path> patternFiles(const std::string& value,
                                                const std::filesystem::path& dir) {
  std::istringstream in(value);
  std::string pattern;
  long long first = 0;
  long long last = 0;
  long long step = 1;
  if (!(in >> pattern >> first >> last)) {
    throw std::invalid_argument("Cannot read the file pattern '" + value + "'");
  }
  if (!(in >> step)) {
    step = 1;
  }
  const std::size_t percent = pattern.find('%');
  if (percent == std::string::npos || pattern.find('%', percent + 1) != std::string::npos) {
    throw std::invalid_argument("The file pattern '" + pattern + "' needs one number field");
  }
  // Only %d with flags and width, so that the pattern cannot read anything but the number.
  std::size_t i = percent + 1;
  while (i < pattern.size() &&
         (pattern[i] == '0' || pattern[i] == '-' || pattern[i] == '+' || pattern[i] == ' ' ||
          std::isdigit(static_cast<unsigned char>(pattern[i])) != 0)) {
    ++i;
  }
  if (i >= pattern.size() || (pattern[i] != 'd' && pattern[i] != 'i' && pattern[i] != 'u')) {
    throw std::invalid_argument("The file pattern '" + pattern + "' needs a %d field");
  }
  if (step == 0 || (last - first) / step < 0 || (last - first) / step > kMaxVolumeEdge) {
    throw std::invalid_argument("The file pattern '" + value + "' gives no files");
  }
  const std::string before = pattern.substr(0, percent);
  const std::string format = pattern.substr(percent, i - percent) + "lld";  // %03d -> %03lld
  const std::string after = pattern.substr(i + 1);
  std::vector<std::filesystem::path> files;
  for (long long n = first; step > 0 ? n <= last : n >= last; n += step) {
    std::array<char, 64> number{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    std::snprintf(number.data(), number.size(), format.c_str(), n);
    std::string name = before;
    name += number.data();
    name += after;
    files.push_back(dir / name);
  }
  return files;
}

// --- MetaImage --------------------------------------------------------------------------------

RawSampleType metaType(const std::string& value) {
  static const std::map<std::string, RawSampleType> types = {
      {"MET_UCHAR", RawSampleType::kUInt8},   {"MET_CHAR", RawSampleType::kInt8},
      {"MET_USHORT", RawSampleType::kUInt16}, {"MET_SHORT", RawSampleType::kInt16},
      {"MET_UINT", RawSampleType::kUInt32},   {"MET_INT", RawSampleType::kInt32},
      {"MET_FLOAT", RawSampleType::kFloat32}, {"MET_DOUBLE", RawSampleType::kFloat64}};
  if (const auto it = types.find(value); it != types.end()) {
    return it->second;
  }
  throw std::invalid_argument("The element type " + value +
                              " is not read; 8-, 16- and 32-bit integers and floats are");
}

bool metaTrue(const std::string& value) {
  const std::string v = lower(value);
  return v == "true" || v == "1" || v == "yes";
}

HeaderVolume readMetaImage(const std::filesystem::path& path) {
  std::uint64_t end = 0;
  const std::vector<std::string> lines = headerLines(path, end, [&](const std::string& line) {
    const auto equals = line.find('=');
    return equals != std::string::npos && lower(trim(line.substr(0, equals))) == "elementdatafile";
  });
  std::map<std::string, std::string> fields;
  for (const std::string& line : lines) {
    const auto equals = line.find('=');
    if (equals != std::string::npos) {
      fields[lower(trim(line.substr(0, equals)))] = trim(line.substr(equals + 1));
    }
  }
  const auto field = [&](const std::string& key) -> const std::string* {
    const auto it = fields.find(key);
    return it == fields.end() ? nullptr : &it->second;
  };
  const std::string what = path.filename().string();
  if (field("elementdatafile") == nullptr) {
    throw std::invalid_argument(what + " is no MetaImage header: it has no ElementDataFile");
  }
  if (const auto* type = field("objecttype"); type != nullptr && lower(*type) != "image") {
    throw std::invalid_argument(what + " holds a MetaImage " + *type + ", not an image");
  }
  HeaderVolume volume;
  volume.format = "metaimage";
  RawLayout& layout = volume.layout;
  const auto* dim_size = field("dimsize");
  if (dim_size == nullptr) {
    throw std::invalid_argument(what + " gives no DimSize");
  }
  const std::vector<double> sizes = numbers(*dim_size, "DimSize");
  if (const auto* ndims = field("ndims"); ndims != nullptr) {
    const std::vector<double> n = numbers(*ndims, "NDims");
    if (n.size() != 1 || n[0] != static_cast<double>(sizes.size())) {
      throw std::invalid_argument(what + ": NDims and DimSize do not agree");
    }
  }
  layout.dims = dimsOf(sizes, what);
  if (const auto* channels = field("elementnumberofchannels");
      channels != nullptr && numbers(*channels, "ElementNumberOfChannels") != std::vector{1.0}) {
    throw std::invalid_argument(what + " has " + *channels +
                                " channels per voxel; volumes of one grey value are read");
  }
  const auto* type = field("elementtype");
  if (type == nullptr) {
    throw std::invalid_argument(what + " gives no ElementType");
  }
  setSampleType(layout, metaType(*type));
  const auto* msb = field("binarydatabyteordermsb");
  if (msb == nullptr) {
    msb = field("elementbyteordermsb");
  }
  layout.byte_order = msb != nullptr && metaTrue(*msb) ? std::endian::big : std::endian::little;
  const auto* spacing = field("elementspacing");
  if (spacing == nullptr) {
    spacing = field("elementsize");
  }
  std::array<double, 3> pitch{1.0, 1.0, 1.0};
  if (spacing != nullptr) {
    const std::vector<double> values = numbers(*spacing, "ElementSpacing");
    if (values.size() != sizes.size()) {
      throw std::invalid_argument(what + ": ElementSpacing and DimSize do not agree");
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
      pitch[i] = std::abs(values[i]);
    }
    volume.voxel_size_known = true;
  }
  if (sizes.size() == 2) {
    pitch[2] = pitch[0];
  }
  layout.voxel_size = VoxelSize(pitch[0], pitch[1], pitch[2]);
  layout.voxel_size.validate();
  // Position and directions; ITK writes the direction of each axis as three numbers in turn.
  std::array<double, 3> origin{};
  for (const char* key : {"offset", "origin", "position"}) {
    if (const auto* value = field(key)) {
      const std::vector<double> values = numbers(*value, key);
      for (std::size_t i = 0; i < std::min<std::size_t>(3, values.size()); ++i) {
        origin.at(i) = values[i];
      }
      break;
    }
  }
  std::array<std::array<double, 3>, 3> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  for (const char* key : {"transformmatrix", "rotation", "orientation"}) {
    if (const auto* value = field(key)) {
      const std::vector<double> values = numbers(*value, key);
      const std::size_t n = sizes.size();
      if (values.size() == n * n) {
        for (std::size_t c = 0; c < n; ++c) {
          for (std::size_t r = 0; r < n; ++r) {
            axes.at(c).at(r) = values[c * n + r];
          }
        }
      }
      break;
    }
  }
  setPose(volume, axes, origin);
  volume.compressed = field("compresseddata") != nullptr && metaTrue(*field("compresseddata"));
  const std::filesystem::path dir = path.parent_path();
  const std::string data = *field("elementdatafile");
  if (lower(data) == "local") {
    volume.files = {path};
    if (volume.compressed) {
      layout.compressed_offset = end;
      layout.header_bytes = 0;
    } else {
      layout.header_bytes = end;
    }
  } else {
    if (lower(data).starts_with("list")) {
      // The file names follow, one per line, to the end of the header file.
      std::ifstream in(path, std::ios::binary);
      in.seekg(static_cast<std::streamoff>(end));
      std::string line;
      while (std::getline(in, line)) {
        if (const std::string name = trim(line); !name.empty()) {
          volume.files.push_back(dir / name);
        }
      }
    } else if (data.find('%') != std::string::npos) {
      volume.files = patternFiles(data, dir);
    } else {
      volume.files = {dir / data};
    }
    // HeaderSize -1: the data is at the end of the file, after a header of any size.
    long long header_size = 0;
    if (const auto* value = field("headersize")) {
      header_size = std::llround(numbers(*value, "HeaderSize").at(0));
    }
    if (header_size >= 0) {
      layout.header_bytes = static_cast<std::uint64_t>(header_size);
    } else if (!volume.compressed) {
      layout.header_bytes.reset();
    } else {
      throw std::invalid_argument(what + ": HeaderSize -1 is read only for uncompressed data");
    }
  }
  return volume;
}

// --- NRRD -------------------------------------------------------------------------------------

RawSampleType nrrdType(const std::string& value) {
  std::string v = lower(value);
  if (v.ends_with("_t")) {
    v.resize(v.size() - 2);
  }
  static const std::map<std::string, RawSampleType> types = {
      {"uchar", RawSampleType::kUInt8},
      {"unsigned char", RawSampleType::kUInt8},
      {"uint8", RawSampleType::kUInt8},
      {"signed char", RawSampleType::kInt8},
      {"int8", RawSampleType::kInt8},
      {"short", RawSampleType::kInt16},
      {"short int", RawSampleType::kInt16},
      {"signed short", RawSampleType::kInt16},
      {"signed short int", RawSampleType::kInt16},
      {"int16", RawSampleType::kInt16},
      {"ushort", RawSampleType::kUInt16},
      {"unsigned short", RawSampleType::kUInt16},
      {"unsigned short int", RawSampleType::kUInt16},
      {"uint16", RawSampleType::kUInt16},
      {"int", RawSampleType::kInt32},
      {"signed int", RawSampleType::kInt32},
      {"int32", RawSampleType::kInt32},
      {"uint", RawSampleType::kUInt32},
      {"unsigned int", RawSampleType::kUInt32},
      {"uint32", RawSampleType::kUInt32},
      {"float", RawSampleType::kFloat32},
      {"double", RawSampleType::kFloat64}};
  if (const auto it = types.find(v); it != types.end()) {
    return it->second;
  }
  throw std::invalid_argument("The NRRD type '" + value +
                              "' is not read; 8-, 16- and 32-bit integers and floats are");
}

/// The vectors of "space directions: (1,0,0) (0,1,0) none".
std::vector<std::optional<std::array<double, 3>>> nrrdVectors(const std::string& value) {
  std::vector<std::optional<std::array<double, 3>>> vectors;
  std::size_t i = 0;
  while (i < value.size()) {
    if (std::isspace(static_cast<unsigned char>(value[i])) != 0) {
      ++i;
    } else if (value[i] == '(') {
      const std::size_t close = value.find(')', i);
      if (close == std::string::npos) {
        throw std::invalid_argument("Cannot read the vectors '" + value + "'");
      }
      const std::vector<double> v = numbers(value.substr(i, close - i + 1), "space directions");
      if (v.size() != 3) {
        throw std::invalid_argument("Space directions of three coordinates are read: " + value);
      }
      vectors.emplace_back(std::array<double, 3>{v[0], v[1], v[2]});
      i = close + 1;
    } else if (lower(value.substr(i, 4)) == "none") {
      vectors.emplace_back(std::nullopt);
      i += 4;
    } else {
      throw std::invalid_argument("Cannot read the vectors '" + value + "'");
    }
  }
  return vectors;
}

/// Bytes of the first `lines` lines of `path`.
std::uint64_t bytesOfLines(const std::filesystem::path& path, std::uint64_t start,
                           long long lines) {
  std::ifstream in(path, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(start));
  std::uint64_t bytes = 0;
  std::string line;
  for (long long i = 0; i < lines && std::getline(in, line); ++i) {
    bytes += line.size() + 1;
  }
  return bytes;
}

HeaderVolume readNrrd(const std::filesystem::path& path) {
  std::uint64_t end = 0;
  bool first = true;
  bool listing = false;
  const std::vector<std::string> lines = headerLines(path, end, [&](const std::string& line) {
    if (first) {
      first = false;
      return false;
    }
    if (listing) {
      return false;
    }
    const std::string l = lower(line);
    if ((l.starts_with("data file:") || l.starts_with("datafile:")) &&
        lower(trim(line.substr(line.find(':') + 1))).starts_with("list")) {
      listing = true;  // the file names follow to the end of the header file
      return false;
    }
    return trim(line).empty();  // a blank line ends the header; attached data follows
  });
  const std::string what = path.filename().string();
  if (lines.empty() || !lines.front().starts_with("NRRD000")) {
    throw std::invalid_argument(what + " is no NRRD file: it does not start with NRRD000");
  }
  std::map<std::string, std::string> fields;
  std::vector<std::string> listed;
  bool in_list = false;
  for (std::size_t n = 1; n < lines.size(); ++n) {
    const std::string& line = lines[n];
    if (in_list) {
      if (const std::string name = trim(line); !name.empty()) {
        listed.push_back(name);
      }
      continue;
    }
    if (line.starts_with("#") || trim(line).empty()) {
      continue;
    }
    if (line.find(":=") != std::string::npos) {
      continue;  // key/value pairs: comments for us
    }
    const auto colon = line.find(": ");
    if (colon == std::string::npos) {
      std::string message = what + ": cannot read the line '";
      message += line;
      message += "'";
      throw std::invalid_argument(message);
    }
    const std::string key = lower(trim(line.substr(0, colon)));
    const std::string value = trim(line.substr(colon + 2));
    fields[key] = value;
    if ((key == "data file" || key == "datafile") && lower(value).starts_with("list")) {
      in_list = true;
    }
  }
  const auto field = [&](const std::string& key) -> const std::string* {
    const auto it = fields.find(key);
    return it == fields.end() ? nullptr : &it->second;
  };
  for (const char* required : {"type", "dimension", "sizes", "encoding"}) {
    if (field(required) == nullptr) {
      throw std::invalid_argument(what + " gives no '" + required + "'");
    }
  }
  HeaderVolume volume;
  volume.format = "nrrd";
  RawLayout& layout = volume.layout;
  std::vector<double> sizes = numbers(*field("sizes"), "sizes");
  const std::vector<double> dimension = numbers(*field("dimension"), "dimension");
  if (dimension.size() != 1 || dimension[0] != static_cast<double>(sizes.size())) {
    throw std::invalid_argument(what + ": dimension and sizes do not agree");
  }
  // A first axis of one sample (a scalar written as a vector) is dropped.
  bool dropped_first = false;
  if (sizes.size() == 4 && sizes[0] == 1.0) {
    sizes.erase(sizes.begin());
    dropped_first = true;
  }
  if (sizes.size() > 3) {
    throw std::invalid_argument(what + " has " + std::to_string(sizes.size()) +
                                " axes; volumes of one grey value per voxel are read");
  }
  layout.dims = dimsOf(sizes, what);
  setSampleType(layout, nrrdType(*field("type")));
  const std::string encoding = lower(*field("encoding"));
  if (encoding == "gzip" || encoding == "gz") {
    volume.compressed = true;
  } else if (encoding != "raw") {
    throw std::invalid_argument(what + ": the encoding '" + encoding +
                                "' is not read; raw and gzip are");
  }
  if (const auto* endian = field("endian")) {
    layout.byte_order = lower(*endian) == "big" ? std::endian::big : std::endian::little;
  } else if (rawSampleBytes(storedSampleType(layout)) > 1) {
    throw std::invalid_argument(what + " gives no endian for samples of several bytes");
  }
  // Spacing from the space directions (with the position), else from the spacings.
  std::array<double, 3> pitch{1.0, 1.0, 1.0};
  std::array<std::array<double, 3>, 3> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  bool directions = false;
  if (const auto* value = field("space directions")) {
    auto vectors = nrrdVectors(*value);
    if (dropped_first && !vectors.empty()) {
      vectors.erase(vectors.begin());
    }
    if (vectors.size() == sizes.size() &&
        std::ranges::all_of(vectors, [](const auto& v) { return v.has_value(); })) {
      for (std::size_t i = 0; i < vectors.size(); ++i) {
        axes.at(i) = vectors[i].value_or(std::array<double, 3>{});
        pitch.at(i) = std::hypot(axes[i][0], axes[i][1], axes[i][2]);
      }
      directions = true;
      volume.voxel_size_known = true;
    }
  }
  if (!directions) {
    if (const auto* value = field("spacings")) {
      std::vector<double> values = numbers(*value, "spacings");
      if (dropped_first && !values.empty()) {
        values.erase(values.begin());
      }
      if (values.size() == sizes.size() &&
          std::ranges::all_of(values, [](double v) { return std::isfinite(v) && v != 0.0; })) {
        for (std::size_t i = 0; i < values.size(); ++i) {
          pitch.at(i) = std::abs(values[i]);
        }
        volume.voxel_size_known = true;
      }
    }
  }
  if (sizes.size() == 2) {
    pitch[2] = pitch[0];
  }
  for (double& p : pitch) {
    if (!(p > 0.0)) {
      p = 1.0;
    }
  }
  layout.voxel_size = VoxelSize(pitch[0], pitch[1], pitch[2]);
  layout.voxel_size.validate();
  std::array<double, 3> origin{};
  if (const auto* value = field("space origin")) {
    const std::vector<double> values = numbers(*value, "space origin");
    for (std::size_t i = 0; i < std::min<std::size_t>(3, values.size()); ++i) {
      origin.at(i) = values[i];
    }
  }
  setPose(volume, axes, origin);
  if (const auto* units = field("space units");
      units != nullptr && lower(*units).find("mm") == std::string::npos) {
    volume.notes.push_back("The header gives the units " + *units +
                           "; VoxelSieve reads the spacing as mm.");
  }
  long long line_skip = 0;
  long long byte_skip = 0;
  if (const auto* value = field("line skip")) {
    line_skip = std::llround(numbers(*value, "line skip").at(0));
  } else if (const auto* skip = field("lineskip")) {
    line_skip = std::llround(numbers(*skip, "lineskip").at(0));
  }
  if (const auto* value = field("byte skip")) {
    byte_skip = std::llround(numbers(*value, "byte skip").at(0));
  } else if (const auto* skip = field("byteskip")) {
    byte_skip = std::llround(numbers(*skip, "byteskip").at(0));
  }
  if (line_skip < 0 || byte_skip < -1) {
    throw std::invalid_argument(what + ": line skip and byte skip cannot be negative");
  }
  const std::filesystem::path dir = path.parent_path();
  const std::string* data = field("data file");
  if (data == nullptr) {
    data = field("datafile");
  }
  std::uint64_t start = 0;  // of the data in its file
  if (data == nullptr) {
    volume.files = {path};
    start = end;
  } else if (lower(*data).starts_with("list")) {
    for (const std::string& name : listed) {
      volume.files.push_back(dir / name);
    }
  } else if (data->find('%') != std::string::npos) {
    volume.files = patternFiles(*data, dir);
  } else {
    volume.files = {dir / *data};
  }
  if (volume.files.empty()) {
    throw std::invalid_argument(what + " names no data files");
  }
  start += bytesOfLines(volume.files.front(), start, line_skip);
  if (volume.compressed) {
    layout.compressed_offset = start;
    if (byte_skip < 0) {
      throw std::invalid_argument(what + ": byte skip -1 is read only for raw encoding");
    }
    layout.header_bytes = static_cast<std::uint64_t>(byte_skip);
  } else if (byte_skip < 0) {
    layout.header_bytes.reset();  // the data is at the end of the file
  } else {
    layout.header_bytes = start + static_cast<std::uint64_t>(byte_skip);
  }
  if (volume.files.size() > 1 && start > 0) {
    throw std::invalid_argument(what + ": line skip with several data files is not read");
  }
  return volume;
}

/// Bytes at the start of `path`.
std::string firstBytes(const std::filesystem::path& path, std::size_t count) {
  std::ifstream in(path, std::ios::binary);
  std::string bytes(count, '\0');
  in.read(bytes.data(), static_cast<std::streamsize>(count));
  bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
  return bytes;
}

}  // namespace

bool isHeaderVolumeFile(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    return false;
  }
  const std::string extension = extensionOf(path);
  if (extension == ".nrrd" || extension == ".nhdr") {
    return firstBytes(path, 7) == "NRRD000";
  }
  if (extension == ".mhd" || extension == ".mha") {
    const std::string start = lower(firstBytes(path, 4096));
    return start.find("dimsize") != std::string::npos ||
           start.find("elementdatafile") != std::string::npos;
  }
  return false;
}

HeaderVolume readHeaderVolume(const std::filesystem::path& path) {
  const std::string extension = extensionOf(path);
  HeaderVolume volume =
      extension == ".nrrd" || extension == ".nhdr" ? readNrrd(path) : readMetaImage(path);
  if (volume.files.size() > 1) {
    const auto count = static_cast<std::int64_t>(volume.files.size());
    if (volume.layout.dims[2] % count != 0) {
      throw std::invalid_argument(path.filename().string() + " names " + std::to_string(count) +
                                  " data files for " + std::to_string(volume.layout.dims[2]) +
                                  " slices");
    }
  }
  for (const auto& file : volume.files) {
    if (!std::filesystem::exists(file)) {
      throw std::invalid_argument("The data file " + file.string() + " named by " +
                                  path.filename().string() + " does not exist");
    }
  }
  return volume;
}

std::unique_ptr<VolumeSource> openHeaderVolume(const HeaderVolume& header,
                                               const std::optional<VoxelSize>& voxel_size,
                                               const std::optional<std::array<double, 2>>& range) {
  RawLayout layout = header.layout;
  if (voxel_size) {
    layout.voxel_size = *voxel_size;
  }
  layout.value_range = range;
  const auto open = [&](const std::filesystem::path& file, const RawLayout& part) {
    if (header.compressed) {
      return std::unique_ptr<VolumeSource>(std::make_unique<GzipRawSource>(file, part));
    }
    return std::unique_ptr<VolumeSource>(std::make_unique<MappedRawSource>(file, part));
  };
  if (header.files.size() == 1) {
    return open(header.files.front(), layout);
  }
  RawLayout part = layout;
  part.dims[2] = layout.dims[2] / static_cast<std::int64_t>(header.files.size());
  // One value range for all files, from a few spread over them, so that the parts agree.
  const RawSampleType type = storedSampleType(layout);
  const bool wide = type == RawSampleType::kUInt32 || type == RawSampleType::kInt32 ||
                    type == RawSampleType::kFloat32 || type == RawSampleType::kFloat64;
  if (wide && !part.value_range) {
    constexpr std::size_t kSampleFiles = 33;
    const std::size_t count = std::min(kSampleFiles, header.files.size());
    std::array<double, 2> found{std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity()};
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t f = count == 1 ? 0 : i * (header.files.size() - 1) / (count - 1);
      const auto source = open(header.files[f], part);
      const auto clipped = rawClippedValues(*source);
      const std::array<double, 2> r = clipped ? clipped->range : std::array{0.0, 65535.0};
      found = {std::min(found[0], r[0]), std::max(found[1], r[1])};
    }
    part.value_range = found;
  }
  std::vector<std::unique_ptr<VolumeSource>> parts;
  parts.reserve(header.files.size());
  for (const auto& file : header.files) {
    parts.push_back(open(file, part));
  }
  return std::make_unique<ConcatSource>(std::move(parts), 2);
}

std::optional<ClippedValues> rawClippedValues(const VolumeSource& source) {
  if (const auto* mapped = dynamic_cast<const MappedRawSource*>(&source)) {
    if (const auto range = mapped->valueRange()) {
      return ClippedValues{*range, mapped->clippedValues()};
    }
    return std::nullopt;
  }
  if (const auto* gzip = dynamic_cast<const GzipRawSource*>(&source)) {
    if (const auto range = gzip->valueRange()) {
      return ClippedValues{*range, gzip->clippedValues()};
    }
    return std::nullopt;
  }
  if (const auto* concat = dynamic_cast<const ConcatSource*>(&source)) {
    std::optional<ClippedValues> total;
    for (std::size_t i = 0; i < concat->partCount(); ++i) {
      if (const auto part = rawClippedValues(concat->part(i))) {
        if (!total) {
          total = part;
        } else {
          total->clipped += part->clipped;
        }
      }
    }
    return total;
  }
  return std::nullopt;
}

}  // namespace voxelsieve
