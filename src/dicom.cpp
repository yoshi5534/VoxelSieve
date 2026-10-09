#include "voxelsieve/dicom.hpp"

// DCMTK's configuration header comes before any of its other headers.
// clang-format off
#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcrledrg.h>
// clang-format on
#include <tbb/parallel_for.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "voxelsieve/tiff.hpp"

namespace voxelsieve {
namespace {

using Vec3 = std::array<double, 3>;

std::string lowerExtension(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}

Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vec3 normalized(const Vec3& v) {
  const double length = std::sqrt(dot(v, v));
  if (!(length > 1e-6)) {
    throw std::runtime_error("DICOM image orientation is not a direction");
  }
  return {v[0] / length, v[1] / length, v[2] / length};
}

/// What one slice file says about itself, read without its pixel data.
struct SliceHeader {
  std::filesystem::path path;
  std::string series;
  std::optional<long> instance;
  std::optional<Vec3> position;
  std::optional<std::array<Vec3, 2>> orientation;  // row and column direction
  std::uint16_t rows = 0;
  std::uint16_t columns = 0;
  std::uint16_t bits_allocated = 0;
  std::uint16_t bits_stored = 0;
  std::uint16_t high_bit = 0;
  std::uint16_t pixel_representation = 0;
  std::uint16_t samples_per_pixel = 1;
  long frames = 1;
  std::string photometric;
  double slope = 1.0;
  double intercept = 0.0;
  std::optional<std::array<double, 2>> pixel_spacing;  // between rows (y), between columns (x)
  std::optional<double> slice_thickness;
  std::optional<double> spacing_between_slices;
};

OFFilename fileName(const std::filesystem::path& path) { return OFFilename(path.string().c_str()); }

std::optional<double> getDouble(DcmItem& item, const DcmTagKey& tag, unsigned long index = 0) {
  Float64 value = 0.0;
  if (item.findAndGetFloat64(tag, value, index).good()) {
    return value;
  }
  return std::nullopt;
}

/// The header of a slice, or nothing when DCMTK cannot read the file or it holds no image.
std::optional<SliceHeader> readHeader(const std::filesystem::path& path) {
  DcmFileFormat file;
  if (file.loadFileUntilTag(fileName(path), EXS_Unknown, EGL_noChange, DCM_MaxReadLength,
                            ERM_autoDetect, DCM_PixelData)
          .bad()) {
    return std::nullopt;
  }
  DcmDataset& data = *file.getDataset();
  SliceHeader header;
  header.path = path;
  if (data.findAndGetUint16(DCM_Rows, header.rows).bad() ||
      data.findAndGetUint16(DCM_Columns, header.columns).bad() || header.rows == 0 ||
      header.columns == 0) {
    return std::nullopt;
  }
  OFString text;
  if (data.findAndGetOFString(DCM_SeriesInstanceUID, text).good()) {
    header.series = text.c_str();
  }
  Sint32 number = 0;
  if (data.findAndGetSint32(DCM_InstanceNumber, number).good()) {
    header.instance = number;
  }
  if (data.findAndGetSint32(DCM_NumberOfFrames, number).good()) {
    header.frames = number;
  }
  if (data.findAndGetOFString(DCM_PhotometricInterpretation, text).good()) {
    header.photometric = text.c_str();
  }
  (void)data.findAndGetUint16(DCM_BitsAllocated, header.bits_allocated);
  (void)data.findAndGetUint16(DCM_BitsStored, header.bits_stored);
  if (data.findAndGetUint16(DCM_HighBit, header.high_bit).bad()) {
    header.high_bit = static_cast<std::uint16_t>(std::max(1, int{header.bits_stored}) - 1);
  }
  (void)data.findAndGetUint16(DCM_PixelRepresentation, header.pixel_representation);
  (void)data.findAndGetUint16(DCM_SamplesPerPixel, header.samples_per_pixel);
  header.slope = getDouble(data, DCM_RescaleSlope).value_or(1.0);
  header.intercept = getDouble(data, DCM_RescaleIntercept).value_or(0.0);
  if (header.slope == 0.0) {
    header.slope = 1.0;  // a missing slope written as 0
  }
  const auto x = getDouble(data, DCM_ImagePositionPatient, 0);
  const auto y = getDouble(data, DCM_ImagePositionPatient, 1);
  const auto z = getDouble(data, DCM_ImagePositionPatient, 2);
  if (x && y && z) {
    header.position = Vec3{*x, *y, *z};
  }
  std::array<double, 6> cosines{};
  bool oriented = true;
  for (unsigned long i = 0; i < 6 && oriented; ++i) {
    const auto value = getDouble(data, DCM_ImageOrientationPatient, i);
    oriented = value.has_value();
    cosines[i] = value.value_or(0.0);
  }
  if (oriented) {
    header.orientation = std::array<Vec3, 2>{normalized({cosines[0], cosines[1], cosines[2]}),
                                             normalized({cosines[3], cosines[4], cosines[5]})};
  }
  const auto row_spacing = getDouble(data, DCM_PixelSpacing, 0);
  const auto column_spacing = getDouble(data, DCM_PixelSpacing, 1);
  if (row_spacing && column_spacing && *row_spacing > 0.0 && *column_spacing > 0.0) {
    header.pixel_spacing = std::array<double, 2>{*row_spacing, *column_spacing};
  }
  if (const auto thickness = getDouble(data, DCM_SliceThickness); thickness && *thickness > 0.0) {
    header.slice_thickness = thickness;
  }
  if (const auto spacing = getDouble(data, DCM_SpacingBetweenSlices); spacing && *spacing > 0.0) {
    header.spacing_between_slices = spacing;
  }
  return header;
}

/// Throws unless `slice` is a kind of image we read and has the layout of `first`.
void checkSlice(const SliceHeader& first, const SliceHeader& slice) {
  const std::string name = slice.path.filename().string();
  if (slice.frames != 1) {
    throw std::runtime_error(name + " holds " + std::to_string(slice.frames) +
                             " frames; multi-frame DICOM files are not supported yet");
  }
  if (slice.samples_per_pixel != 1 || !slice.photometric.starts_with("MONOCHROME")) {
    throw std::runtime_error(name + " is not a grey-value image (" + slice.photometric + ")");
  }
  if (slice.bits_allocated != 8 && slice.bits_allocated != 16) {
    throw std::runtime_error(name + " has " + std::to_string(slice.bits_allocated) +
                             " bits per sample; 8 and 16 are supported");
  }
  if (slice.bits_stored == 0 || slice.bits_stored > slice.bits_allocated ||
      slice.high_bit + 1 != slice.bits_stored) {
    throw std::runtime_error(name + " has an unsupported bit layout (bits stored " +
                             std::to_string(slice.bits_stored) + ", high bit " +
                             std::to_string(slice.high_bit) + ")");
  }
  if (slice.rows != first.rows || slice.columns != first.columns ||
      slice.bits_allocated != first.bits_allocated || slice.bits_stored != first.bits_stored ||
      slice.pixel_representation != first.pixel_representation) {
    throw std::runtime_error(name + " differs in size or sample type from " +
                             first.path.filename().string());
  }
  if (slice.slope != first.slope || slice.intercept != first.intercept) {
    throw std::runtime_error(name + " has another rescale slope or intercept than " +
                             first.path.filename().string() +
                             "; slices rescaled differently cannot share one value mapping");
  }
  if (slice.orientation.has_value() != first.orientation.has_value() ||
      (slice.orientation &&
       (std::abs(dot((*slice.orientation)[0], (*first.orientation)[0]) - 1.0) > 1e-4 ||
        std::abs(dot((*slice.orientation)[1], (*first.orientation)[1]) - 1.0) > 1e-4))) {
    throw std::runtime_error(name + " is oriented differently from " +
                             first.path.filename().string());
  }
}

std::once_flag codecs_registered;

}  // namespace

struct DicomStackSource::Impl {
  DicomStackOptions options;
  std::vector<SliceHeader> slices;  // in slice order
  std::vector<std::filesystem::path> files;
  std::vector<std::pair<std::string, std::size_t>> other_series;
  std::size_t skipped = 0;
  std::array<std::int64_t, 3> dims{0, 0, 0};
  std::optional<VoxelSize> file_voxel_size;
  RigidTransform pose;

  using Slice = std::shared_ptr<const std::vector<std::uint16_t>>;
  mutable std::mutex mutex;
  mutable std::list<std::pair<std::size_t, Slice>> lru;
  mutable std::unordered_map<std::size_t, std::list<std::pair<std::size_t, Slice>>::iterator>
      cached;

  void open(const std::vector<std::filesystem::path>& candidates);
  [[nodiscard]] Slice slice(std::size_t z) const;
  [[nodiscard]] std::vector<std::uint16_t> decode(const SliceHeader& header) const;
};

void DicomStackSource::Impl::open(const std::vector<std::filesystem::path>& candidates) {
  std::vector<std::optional<SliceHeader>> headers(candidates.size());
  tbb::parallel_for(std::size_t{0}, candidates.size(),
                    [&](std::size_t i) { headers[i] = readHeader(candidates[i]); });
  std::map<std::string, std::vector<SliceHeader>> series;
  for (auto& header : headers) {
    if (header) {
      series[header->series].push_back(std::move(*header));
    } else {
      ++skipped;
    }
  }
  if (series.empty()) {
    throw std::runtime_error("No DICOM images among " + std::to_string(candidates.size()) +
                             " files");
  }
  auto chosen = series.end();
  if (!options.series.empty()) {
    chosen = series.find(options.series);
    if (chosen == series.end()) {
      throw std::runtime_error("No DICOM series " + options.series);
    }
  } else {
    chosen =
        std::ranges::max_element(series, {}, [](const auto& entry) { return entry.second.size(); });
  }
  for (const auto& [uid, members] : series) {
    if (uid != chosen->first) {
      other_series.emplace_back(uid, members.size());
    }
  }
  slices = std::move(chosen->second);
  const SliceHeader first = slices.front();
  for (const SliceHeader& slice : slices) {
    checkSlice(first, slice);
  }

  // Order along the slice normal when every slice states where it lies.
  const bool positioned =
      std::ranges::all_of(slices, [](const SliceHeader& s) { return s.position && s.orientation; });
  std::optional<double> spacing;
  Vec3 normal{0.0, 0.0, 1.0};
  if (positioned) {
    normal = normalized(cross((*first.orientation)[0], (*first.orientation)[1]));
    const auto along = [&normal](const SliceHeader& s) { return dot(*s.position, normal); };
    std::ranges::sort(slices, {}, along);
    if (slices.size() > 1) {
      spacing =
          (along(slices.back()) - along(slices.front())) / static_cast<double>(slices.size() - 1);
      const double tolerance = std::max(1e-3, 0.01 * *spacing);
      for (std::size_t i = 1; i < slices.size(); ++i) {
        const double step = along(slices[i]) - along(slices[i - 1]);
        if (std::abs(step - *spacing) > tolerance) {
          throw std::runtime_error(
              "Uneven slice spacing: " + slices[i - 1].path.filename().string() + " and " +
              slices[i].path.filename().string() + " lie " + std::to_string(step) +
              " mm apart, the stack " + std::to_string(*spacing) +
              " mm on average; gaps and duplicate slices are not supported");
        }
      }
    }
  } else if (std::ranges::all_of(slices,
                                 [](const SliceHeader& s) { return s.instance.has_value(); })) {
    std::ranges::sort(slices, {}, [](const SliceHeader& s) { return *s.instance; });
  } else {
    std::ranges::sort(slices, [](const SliceHeader& a, const SliceHeader& b) {
      return naturalLess(a.path.filename().string(), b.path.filename().string());
    });
  }
  if (!spacing) {
    spacing = first.spacing_between_slices ? first.spacing_between_slices : first.slice_thickness;
  }

  dims = {first.columns, first.rows, static_cast<std::int64_t>(slices.size())};
  if (first.pixel_spacing) {
    const double x = (*first.pixel_spacing)[1];
    const double y = (*first.pixel_spacing)[0];
    VoxelSize size(x, y, spacing.value_or(x));
    if (first.slice_thickness && *first.slice_thickness < size[2] - 1e-6) {
      size.slice_thickness_mm = *first.slice_thickness;
    }
    file_voxel_size = size;
  }
  if (positioned) {
    const Vec3& u = (*first.orientation)[0];
    const Vec3& v = (*first.orientation)[1];
    pose.rotation = {u[0], v[0], normal[0], u[1], v[1], normal[1], u[2], v[2], normal[2]};
    pose.translation = *slices.front().position;
  }
  files.reserve(slices.size());
  for (const SliceHeader& slice : slices) {
    files.push_back(slice.path);
  }
}

std::vector<std::uint16_t> DicomStackSource::Impl::decode(const SliceHeader& header) const {
  std::call_once(codecs_registered, [] { DcmRLEDecoderRegistration::registerCodecs(); });
  const std::string name = header.path.filename().string();
  DcmFileFormat file;
  if (const OFCondition status = file.loadFile(fileName(header.path), EXS_Unknown, EGL_noChange,
                                               DCM_MaxReadLength, ERM_autoDetect);
      status.bad()) {
    throw std::runtime_error("Cannot read " + header.path.string() + ": " + status.text());
  }
  DcmDataset& data = *file.getDataset();
  const E_TransferSyntax transfer = data.getOriginalXfer();
  if (DcmXfer(transfer).usesEncapsulatedFormat() &&
      data.chooseRepresentation(EXS_LittleEndianExplicit, nullptr).bad()) {
    throw std::runtime_error(name + " is compressed as " + DcmXfer(transfer).getXferName() +
                             ", which is not supported (uncompressed and RLE are)");
  }
  DcmElement* element = nullptr;
  if (data.findAndGetElement(DCM_PixelData, element).bad() || element == nullptr) {
    throw std::runtime_error(name + " has no pixel data");
  }
  const std::size_t count = std::size_t{header.rows} * header.columns;
  const Uint8* bytes = nullptr;
  const Uint16* words = nullptr;
  if (header.bits_allocated == 16) {
    Uint16* data16 = nullptr;
    if (element->getUint16Array(data16).bad() || data16 == nullptr ||
        element->getLength() < 2 * count) {
      throw std::runtime_error(name + " has too little pixel data");
    }
    words = data16;
  } else {
    // 8-bit samples may be stored as OW (implicit VR); the bytes keep their file order either way.
    Uint8* data8 = nullptr;
    Uint16* data16 = nullptr;
    if (element->getVR() == EVR_OB && element->getUint8Array(data8).good()) {
      bytes = data8;
    } else if (element->getUint16Array(data16).good() && data16 != nullptr) {
      static_assert(std::endian::native == std::endian::little);
      bytes = reinterpret_cast<const Uint8*>(data16);  // NOLINT(*-reinterpret-cast)
    }
    if (bytes == nullptr || element->getLength() < count) {
      throw std::runtime_error(name + " has too little pixel data");
    }
  }
  const int bits = header.bits_stored;
  const std::uint32_t mask = (std::uint32_t{1} << bits) - 1U;
  const std::uint32_t sign = std::uint32_t{1} << (bits - 1);
  const bool is_signed = header.pixel_representation == 1;
  std::vector<std::uint16_t> grey(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto stored = static_cast<std::uint32_t>(words != nullptr ? words[i] : bytes[i]) & mask;
    if (is_signed) {
      const std::int32_t value = static_cast<std::int32_t>(stored) -
                                 ((stored & sign) != 0U ? static_cast<std::int32_t>(mask) + 1 : 0);
      grey[i] = static_cast<std::uint16_t>(value + 32768);
    } else {
      grey[i] = static_cast<std::uint16_t>(stored);
    }
  }
  return grey;
}

DicomStackSource::Impl::Slice DicomStackSource::Impl::slice(std::size_t z) const {
  {
    const std::lock_guard lock(mutex);
    if (const auto it = cached.find(z); it != cached.end()) {
      lru.splice(lru.begin(), lru, it->second);
      return it->second->second;
    }
  }
  // Decoded outside the lock, so that threads read different slices at once.
  auto decoded = std::make_shared<const std::vector<std::uint16_t>>(decode(slices[z]));
  const std::lock_guard lock(mutex);
  if (const auto it = cached.find(z); it != cached.end()) {
    return it->second->second;
  }
  lru.emplace_front(z, decoded);
  cached[z] = lru.begin();
  const std::size_t slice_bytes = decoded->size() * sizeof(std::uint16_t);
  const std::size_t capacity = std::max<std::size_t>(1, options.cache_bytes / slice_bytes);
  while (lru.size() > capacity) {
    cached.erase(lru.back().first);
    lru.pop_back();
  }
  return decoded;
}

DicomStackSource::DicomStackSource(const std::filesystem::path& path,
                                   const DicomStackOptions& options)
    : impl_(std::make_unique<Impl>()) {
  impl_->options = options;
  std::vector<std::filesystem::path> candidates;
  if (std::filesystem::is_directory(path)) {
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      const auto& file = entry.path();
      if (entry.is_regular_file() && !file.filename().string().starts_with(".") &&
          (isDicomFile(file) || !file.has_extension())) {
        candidates.push_back(file);
      }
    }
  } else if (std::filesystem::is_regular_file(path)) {
    candidates.push_back(path);
  } else {
    throw std::runtime_error("No such file or directory: " + path.string());
  }
  impl_->open(candidates);
}

DicomStackSource::DicomStackSource(const std::vector<std::filesystem::path>& files,
                                   const DicomStackOptions& options)
    : impl_(std::make_unique<Impl>()) {
  impl_->options = options;
  if (files.empty()) {
    throw std::invalid_argument("No DICOM files given");
  }
  for (const auto& file : files) {
    if (!std::filesystem::is_regular_file(file)) {
      throw std::runtime_error("No such file: " + file.string());
    }
  }
  impl_->open(files);
  if (impl_->skipped > 0) {
    throw std::runtime_error(std::to_string(impl_->skipped) +
                             " of the given files are not DICOM images");
  }
}

DicomStackSource::~DicomStackSource() = default;

std::array<std::int64_t, 3> DicomStackSource::dims() const { return impl_->dims; }

VoxelSize DicomStackSource::voxelSize() const {
  return impl_->options.voxel_size.value_or(impl_->file_voxel_size.value_or(VoxelSize(1.0)));
}

ValueMapping DicomStackSource::valueMapping() const {
  const SliceHeader& first = impl_->slices.front();
  const double shift = isSigned() ? 32768.0 : 0.0;
  return {.offset = first.intercept - shift * first.slope, .scale = first.slope};
}

void DicomStackSource::releaseMemory() const {
  const std::lock_guard lock(impl_->mutex);
  impl_->cached.clear();
  impl_->lru.clear();
}

void DicomStackSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  for (std::size_t axis = 0; axis < 3; ++axis) {
    if (box.min[axis] < 0 || box.max[axis] > impl_->dims[axis] || box.min[axis] > box.max[axis]) {
      throw std::out_of_range("Region outside the volume");
    }
  }
  if (static_cast<std::int64_t>(out.size()) != box.voxelCount()) {
    throw std::invalid_argument("Output size does not match the region");
  }
  const std::int64_t nx = box.size(0);
  const std::int64_t ny = box.size(1);
  const std::int64_t width = impl_->dims[0];
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    const auto slice = impl_->slice(static_cast<std::size_t>(z));
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      std::copy_n(slice->data() + y * width + box.min[0], nx,
                  out.data() + ((z - box.min[2]) * ny + (y - box.min[1])) * nx);
    }
  }
}

std::optional<VoxelSize> DicomStackSource::fileVoxelSize() const { return impl_->file_voxel_size; }

RigidTransform DicomStackSource::filePose() const { return impl_->pose; }

const std::vector<std::filesystem::path>& DicomStackSource::files() const { return impl_->files; }

const std::string& DicomStackSource::seriesUid() const { return impl_->slices.front().series; }

const std::vector<std::pair<std::string, std::size_t>>& DicomStackSource::otherSeries() const {
  return impl_->other_series;
}

int DicomStackSource::bitsStored() const { return impl_->slices.front().bits_stored; }

bool DicomStackSource::isSigned() const { return impl_->slices.front().pixel_representation == 1; }

std::size_t DicomStackSource::skippedFiles() const { return impl_->skipped; }

bool isDicomFile(const std::filesystem::path& path) {
  const std::string extension = lowerExtension(path);
  if (extension == ".dcm" || extension == ".dicom" || extension == ".dic" || extension == ".ima") {
    return true;
  }
  std::ifstream in(path, std::ios::binary);
  std::array<char, 132> head{};
  return in.read(head.data(), head.size()) && std::string_view(head.data() + 128, 4) == "DICM";
}

bool containsDicom(const std::filesystem::path& dir) {
  std::error_code error;
  int checked = 0;
  for (auto it = std::filesystem::directory_iterator(dir, error);
       !error && it != std::filesystem::directory_iterator() && checked < 64;
       it.increment(error), ++checked) {
    std::error_code ignored;
    if (it->is_regular_file(ignored) && isDicomFile(it->path())) {
      return true;
    }
  }
  return false;
}

}  // namespace voxelsieve
