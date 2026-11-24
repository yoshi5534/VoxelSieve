// HDF5 volumes (ADR 0043), read with the HDF5 library through its C API.

#include "voxelsieve/hdf5.hpp"

#include <hdf5.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "detail/raw_samples.hpp"
#include "voxelsieve/tiff.hpp"

namespace voxelsieve {
namespace {

/// The HDF5 library is not built thread-safe: every call goes through this lock.
std::recursive_mutex& libraryMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

/// An HDF5 identifier closed by `close` when it goes out of scope.
class Handle {
 public:
  Handle(hid_t id, herr_t (*close)(hid_t)) : id_(id), close_(close) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept
      : id_(std::exchange(other.id_, H5I_INVALID_HID)), close_(other.close_) {}
  Handle& operator=(Handle&&) = delete;
  ~Handle() {
    if (id_ >= 0) {
      close_(id_);
    }
  }
  [[nodiscard]] hid_t get() const { return id_; }
  [[nodiscard]] bool valid() const { return id_ >= 0; }

 private:
  hid_t id_;
  herr_t (*close_)(hid_t);
};

/// Silences the library's own printing of errors; they become exceptions here.
void quietErrors() { H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr); }

Handle openFile(const std::filesystem::path& path) {
  quietErrors();
  Handle file(H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
  if (!file.valid()) {
    throw std::runtime_error("Cannot open the HDF5 file " + path.string());
  }
  return file;
}

/// The sample type of an HDF5 data type, if it is one a volume can have.
std::optional<RawSampleType> sampleTypeOf(hid_t type) {
  const H5T_class_t type_class = H5Tget_class(type);
  const std::size_t size = H5Tget_size(type);
  if (type_class == H5T_INTEGER) {
    const bool is_signed = H5Tget_sign(type) == H5T_SGN_2;
    switch (size) {
      case 1:
        return is_signed ? RawSampleType::kInt8 : RawSampleType::kUInt8;
      case 2:
        return is_signed ? RawSampleType::kInt16 : RawSampleType::kUInt16;
      case 4:
        return is_signed ? RawSampleType::kInt32 : RawSampleType::kUInt32;
      default:
        return std::nullopt;
    }
  }
  if (type_class == H5T_FLOAT) {
    if (size == 4) {
      return RawSampleType::kFloat32;
    }
    if (size == 8) {
      return RawSampleType::kFloat64;
    }
  }
  return std::nullopt;
}

/// The type in memory for samples of `type`: the machine's byte order.
hid_t memoryType(RawSampleType type) {
  switch (type) {
    case RawSampleType::kUInt8:
      return H5T_NATIVE_UINT8;
    case RawSampleType::kInt8:
      return H5T_NATIVE_INT8;
    case RawSampleType::kUInt16:
      return H5T_NATIVE_UINT16;
    case RawSampleType::kInt16:
      return H5T_NATIVE_INT16;
    case RawSampleType::kUInt32:
      return H5T_NATIVE_UINT32;
    case RawSampleType::kInt32:
      return H5T_NATIVE_INT32;
    case RawSampleType::kFloat32:
      return H5T_NATIVE_FLOAT;
    case RawSampleType::kFloat64:
      return H5T_NATIVE_DOUBLE;
  }
  return H5T_NATIVE_UINT16;
}

/// The shape of a dataset in the file's order (slowest first), when it is a volume: three
/// dimensions of more than one sample.
struct Shape {
  std::vector<hsize_t> dims;  // all of the file's dimensions
  std::array<int, 3> axes{};  // the file dimensions of z, y, x
};

std::optional<Shape> volumeShape(hid_t dataset) {
  const Handle space(H5Dget_space(dataset), H5Sclose);
  const int rank = H5Sget_simple_extent_ndims(space.get());
  if (rank < 3 || rank > 8) {
    return std::nullopt;
  }
  Shape shape;
  shape.dims.resize(static_cast<std::size_t>(rank));
  H5Sget_simple_extent_dims(space.get(), shape.dims.data(), nullptr);
  std::vector<int> large;
  for (int i = 0; i < rank; ++i) {
    if (shape.dims[static_cast<std::size_t>(i)] > 1) {
      large.push_back(i);
    }
  }
  if (rank == 3) {
    large = {0, 1, 2};
  } else if (large.size() != 3) {
    return std::nullopt;
  }
  shape.axes = {large[0], large[1], large[2]};
  for (const int axis : shape.axes) {
    if (shape.dims[static_cast<std::size_t>(axis)] == 0 ||
        shape.dims[static_cast<std::size_t>(axis)] > static_cast<hsize_t>(kMaxVolumeEdge)) {
      return std::nullopt;
    }
  }
  return shape;
}

struct VisitState {
  std::vector<Hdf5Volume> volumes;
  int visited = 0;
};

constexpr int kMaxVisited = 10000;

herr_t visitLink(hid_t group, const char* name, const H5L_info_t* /*info*/, void* data);

void visitGroup(hid_t group, const std::string& prefix, VisitState& state) {
  std::pair<VisitState*, std::string> context{&state, prefix};
  hsize_t index = 0;
  H5Literate(group, H5_INDEX_NAME, H5_ITER_NATIVE, &index, visitLink, &context);
}

herr_t visitLink(hid_t group, const char* name, const H5L_info_t* /*info*/, void* data) {
  auto& [state, prefix] = *static_cast<std::pair<VisitState*, std::string>*>(data);
  if (++state->visited > kMaxVisited) {
    return 1;  // stop: a file of very many objects
  }
  const std::string path = prefix + "/" + name;
  const Handle object(H5Oopen(group, name, H5P_DEFAULT), H5Oclose);
  if (!object.valid()) {
    return 0;  // a dangling or external link
  }
  const H5I_type_t type = H5Iget_type(object.get());
  if (type == H5I_GROUP) {
    if (std::ranges::count(path, '/') <= 8) {
      visitGroup(object.get(), path, *state);
    }
  } else if (type == H5I_DATASET) {
    const Handle file_type(H5Dget_type(object.get()), H5Tclose);
    const auto sample = sampleTypeOf(file_type.get());
    const auto shape = volumeShape(object.get());
    if (sample && shape) {
      Hdf5Volume volume;
      volume.path = path;
      volume.sample_type = *sample;
      for (std::size_t i = 0; i < 3; ++i) {
        volume.dims.at(2 - i) =
            static_cast<std::int64_t>(shape->dims[static_cast<std::size_t>(shape->axes.at(i))]);
      }
      state->volumes.push_back(volume);
    }
  }
  return 0;
}

/// Values of a numeric attribute of `object`, if it has one of that name.
std::optional<std::vector<double>> attributeValues(hid_t object, const char* name) {
  if (H5Aexists(object, name) <= 0) {
    return std::nullopt;
  }
  const Handle attribute(H5Aopen(object, name, H5P_DEFAULT), H5Aclose);
  const Handle type(H5Aget_type(attribute.get()), H5Tclose);
  const H5T_class_t type_class = H5Tget_class(type.get());
  if (type_class != H5T_INTEGER && type_class != H5T_FLOAT) {
    return std::nullopt;
  }
  const Handle space(H5Aget_space(attribute.get()), H5Sclose);
  const hssize_t count = H5Sget_simple_extent_npoints(space.get());
  if (count < 1 || count > 16) {
    return std::nullopt;
  }
  std::vector<double> values(static_cast<std::size_t>(count));
  if (H5Aread(attribute.get(), H5T_NATIVE_DOUBLE, values.data()) < 0) {
    return std::nullopt;
  }
  return values;
}

/// The voxel size an attribute of the dataset gives.
std::optional<VoxelSize> attributeVoxelSize(hid_t dataset) {
  if (const auto um = attributeValues(dataset, "element_size_um"); um && um->size() == 3) {
    // z, y, x in micrometres.
    return VoxelSize((*um)[2] * 1e-3, (*um)[1] * 1e-3, (*um)[0] * 1e-3);
  }
  for (const char* name : {"voxel_size", "voxel_size_mm", "pixel_size", "spacing"}) {
    if (const auto mm = attributeValues(dataset, name)) {
      if (mm->size() == 1) {
        return VoxelSize((*mm)[0]);
      }
      if (mm->size() == 3) {
        return VoxelSize((*mm)[0], (*mm)[1], (*mm)[2]);
      }
    }
  }
  return std::nullopt;
}

}  // namespace

bool isHdf5File(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    return false;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  std::ifstream in(path, std::ios::binary);
  constexpr std::array<char, 8> kSignature = {'\x89', 'H', 'D', 'F', '\r', '\n', '\x1a', '\n'};
  // The signature is at 0, or at 512, 1024, 2048, ... behind a user block.
  for (std::uintmax_t offset = 0; offset + 8 <= size && offset <= (std::uintmax_t{1} << 30U);
       offset = offset == 0 ? 512 : offset * 2) {
    std::array<char, 8> bytes{};
    in.seekg(static_cast<std::streamoff>(offset));
    if (!in.read(bytes.data(), bytes.size())) {
      return false;
    }
    if (bytes == kSignature) {
      return true;
    }
  }
  return false;
}

std::vector<Hdf5Volume> listHdf5Volumes(const std::filesystem::path& path) {
  const std::scoped_lock lock(libraryMutex());
  const Handle file = openFile(path);
  VisitState state;
  visitGroup(file.get(), "", state);
  std::ranges::stable_sort(state.volumes, [](const Hdf5Volume& a, const Hdf5Volume& b) {
    const bool la = looksLikeLabelFolder(a.path);
    const bool lb = looksLikeLabelFolder(b.path);
    if (la != lb) {
      return !la;
    }
    const auto count = [](const Hdf5Volume& v) { return v.dims[0] * v.dims[1] * v.dims[2]; };
    return count(a) > count(b);
  });
  return state.volumes;
}

struct Hdf5Source::Impl {
  std::filesystem::path path;
  std::unique_ptr<Handle> file;
  std::unique_ptr<Handle> dataset;
  Shape shape;
  Hdf5Volume volume;
  std::vector<Hdf5Volume> others;
  VoxelSize voxel_size{1.0};
  std::string voxel_size_from = "assumed";
  std::string identity;
  detail::RawSamples samples;

  /// Reads `box` as samples of the stored type, x fastest; the caller holds the library lock.
  void read(const Box& box, std::vector<unsigned char>& bytes) const {
    const auto rank = shape.dims.size();
    std::vector<hsize_t> start(rank, 0);
    std::vector<hsize_t> count(rank, 1);
    for (std::size_t i = 0; i < 3; ++i) {
      const auto axis = static_cast<std::size_t>(shape.axes.at(i));
      start[axis] = static_cast<hsize_t>(box.min.at(2 - i));
      count[axis] = static_cast<hsize_t>(box.size(2 - i));
    }
    bytes.resize(static_cast<std::size_t>(box.voxelCount()) * samples.bytes());
    const Handle file_space(H5Dget_space(dataset->get()), H5Sclose);
    if (H5Sselect_hyperslab(file_space.get(), H5S_SELECT_SET, start.data(), nullptr, count.data(),
                            nullptr) < 0) {
      throw std::runtime_error("Cannot select a region of " + volume.path);
    }
    const auto voxels = static_cast<hsize_t>(box.voxelCount());
    const Handle memory_space(H5Screate_simple(1, &voxels, nullptr), H5Sclose);
    if (H5Dread(dataset->get(), memoryType(volume.sample_type), memory_space.get(),
                file_space.get(), H5P_DEFAULT, bytes.data()) < 0) {
      throw std::runtime_error("Cannot read " + volume.path + " of " + path.string() +
                               " (a compression filter the library lacks, or a damaged file)");
    }
  }
};

Hdf5Source::Hdf5Source(const std::filesystem::path& path, const Hdf5Options& options)
    : impl_(std::make_unique<Impl>()) {
  Impl& impl = *impl_;
  impl.path = path;
  impl.identity = inputIdentity(path);
  std::vector<Hdf5Volume> volumes = listHdf5Volumes(path);
  if (volumes.empty()) {
    throw std::invalid_argument(path.filename().string() +
                                " holds no dataset of three dimensions of 8-, 16- or 32-bit "
                                "integers or floats");
  }
  std::string wanted = options.dataset;
  if (!wanted.empty() && !wanted.starts_with('/')) {
    wanted.insert(0, "/");
  }
  const auto chosen =
      wanted.empty()
          ? volumes.begin()
          : std::ranges::find_if(volumes, [&](const Hdf5Volume& v) { return v.path == wanted; });
  if (chosen == volumes.end()) {
    std::string known;
    for (const auto& v : volumes) {
      known += (known.empty() ? "" : ", ") + v.path;
    }
    throw std::invalid_argument("No volume '" + options.dataset + "' in " +
                                path.filename().string() + "; volumes: " + known);
  }
  impl.volume = *chosen;
  for (const auto& v : volumes) {
    if (v.path != impl.volume.path) {
      impl.others.push_back(v);
    }
  }
  checkVolumeDims(impl.volume.dims, path.filename().string());
  {
    const std::scoped_lock lock(libraryMutex());
    impl.file = std::make_unique<Handle>(openFile(path));
    impl.dataset = std::make_unique<Handle>(
        H5Dopen2(impl.file->get(), impl.volume.path.c_str(), H5P_DEFAULT), H5Dclose);
    if (!impl.dataset->valid()) {
      throw std::runtime_error("Cannot open " + impl.volume.path + " of " + path.string());
    }
    const auto shape = volumeShape(impl.dataset->get());
    if (!shape) {
      throw std::runtime_error(impl.volume.path + " is no volume");
    }
    impl.shape = *shape;
    if (options.voxel_size) {
      impl.voxel_size = *options.voxel_size;
      impl.voxel_size_from = "parameters";
    } else if (const auto size = attributeVoxelSize(impl.dataset->get())) {
      impl.voxel_size = *size;
      impl.voxel_size_from = "file";
    } else if (const auto named = voxelSizeFromName(path)) {
      impl.voxel_size = *named;
      impl.voxel_size_from = "file name";
    }
  }
  impl.voxel_size.validate();
  RawLayout layout(impl.volume.dims, impl.voxel_size, SampleType::kUInt16, std::endian::native);
  if (impl.volume.sample_type == RawSampleType::kUInt8) {
    layout.sample_type = SampleType::kUInt8;
  } else if (impl.volume.sample_type != RawSampleType::kUInt16) {
    layout.stored_type = impl.volume.sample_type;
  }
  layout.value_range = options.value_range;
  const auto& dims = impl.volume.dims;
  impl.samples.setUp(layout, dims[2] - 1, [&](std::int64_t z) {
    std::vector<unsigned char> bytes;
    const std::scoped_lock lock(libraryMutex());
    impl.read(Box{{0, 0, z}, {dims[0], dims[1], z + 1}}, bytes);
    return impl.samples.values(bytes.data(), bytes.size() / impl.samples.bytes());
  });
}

Hdf5Source::~Hdf5Source() {
  const std::scoped_lock lock(libraryMutex());
  impl_->dataset.reset();
  impl_->file.reset();
}

std::array<std::int64_t, 3> Hdf5Source::dims() const { return impl_->volume.dims; }
VoxelSize Hdf5Source::voxelSize() const { return impl_->voxel_size; }
ValueMapping Hdf5Source::valueMapping() const { return impl_->samples.mapping; }
SampleType Hdf5Source::sampleType() const { return impl_->samples.greyType(); }
std::string Hdf5Source::identity() const { return impl_->identity; }
const Hdf5Volume& Hdf5Source::volume() const { return impl_->volume; }
const std::vector<Hdf5Volume>& Hdf5Source::otherVolumes() const { return impl_->others; }
const std::string& Hdf5Source::voxelSizeFrom() const { return impl_->voxel_size_from; }

std::optional<std::array<double, 2>> Hdf5Source::valueRange() const {
  return impl_->samples.mapped ? std::optional(impl_->samples.range) : std::nullopt;
}

std::uint64_t Hdf5Source::clippedValues() const {
  const std::scoped_lock lock(impl_->samples.mutex);
  return impl_->samples.clipped;
}

void Hdf5Source::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  const auto& dims = impl_->volume.dims;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    if (box.min.at(axis) < 0 || box.max.at(axis) > dims.at(axis) ||
        box.min.at(axis) > box.max.at(axis)) {
      throw std::out_of_range("Region outside the volume");
    }
  }
  if (static_cast<std::int64_t>(out.size()) != box.voxelCount()) {
    throw std::invalid_argument("Output buffer size does not match the region");
  }
  if (box.voxelCount() == 0) {
    return;
  }
  std::vector<unsigned char> bytes;
  {
    const std::scoped_lock lock(libraryMutex());
    impl_->read(box, bytes);
  }
  std::uint64_t outside = 0;
  impl_->samples.convert(bytes.data(), out.size(), out.data(), outside);
  impl_->samples.count(box, outside);
}

}  // namespace voxelsieve
