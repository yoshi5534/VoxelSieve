#include "voxelsieve/diconde.hpp"

// DCMTK's configuration header comes before any of its other headers.
// clang-format off
#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dctk.h>
// clang-format on
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;
using Vec3 = std::array<double, 3>;

// Tags of DICONDE that the DICOM dictionary lists as retired, so DCMTK names them so too.
DcmTagKey otherComponentIdsTag() { return {0x0010, 0x1000}; }
DcmTagKey examinationNotesTag() { return {0x0032, 0x4000}; }

// Maximum lengths of the value representations (PS3.5, 6.2).
constexpr std::size_t kShortString = 16;    // SH, CS
constexpr std::size_t kLongString = 64;     // LO, PN per component group
constexpr std::size_t kShortText = 1024;    // ST
constexpr std::size_t kLongText = 10240;    // LT
constexpr std::size_t kDecimalString = 16;  // DS

/// A number as decimal string of at most 16 characters, with as many digits as fit.
std::string decimal(double value) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument("A DICOM decimal string cannot hold " + std::to_string(value));
  }
  std::array<char, 40> buffer{};
  for (int precision = 15; precision > 0; --precision) {
    const int length = std::snprintf(buffer.data(), buffer.size(), "%.*g", precision, value);
    if (length > 0 && static_cast<std::size_t>(length) <= kDecimalString) {
      return {buffer.data(), static_cast<std::size_t>(length)};
    }
  }
  throw std::invalid_argument("Cannot write " + std::to_string(value) + " as decimal string");
}

std::string decimals(std::initializer_list<double> values) {
  std::string text;
  for (const double value : values) {
    text += (text.empty() ? "" : "\\") + decimal(value);
  }
  return text;
}

/// Text cut to `max` bytes at a UTF-8 character boundary; with `single`, backslashes (the value
/// separator of most string VRs) become slashes so the text stays one value.
std::string fit(std::string text, std::size_t max, bool single = true) {
  if (single) {
    std::ranges::replace(text, '\\', '/');
  }
  if (text.size() > max) {
    std::size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
      --end;
    }
    text.resize(end);
  }
  return text;
}

/// A code string: upper case letters, digits, space and underscore, at most 16 characters.
std::string codeString(const std::string& text) {
  std::string code;
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    code += std::isalnum(u) != 0 && u < 128U ? static_cast<char>(std::toupper(u))
            : c == ' '                       ? ' '
                                             : '_';
  }
  return fit(code, kShortString);
}

/// Digits of a date such as "2026-09-25" as DICOM date, or empty when they are not eight.
std::string dicomDate(const std::string& text) {
  std::string digits;
  std::ranges::copy_if(text, std::back_inserter(digits),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
  return digits.size() == 8 ? digits : std::string();
}

std::string newUid() {
  std::array<char, 100> uid{};
  return dcmGenerateUniqueIdentifier(uid.data(), SITE_INSTANCE_UID_ROOT);
}

void check(const OFCondition& status, const std::string& what) {
  if (status.bad()) {
    throw std::runtime_error("DICONDE: " + what + ": " + status.text());
  }
}

/// Puts a string; empty values only for attributes that must be present (`always`).
void put(DcmItem& item, const DcmTagKey& tag, const std::string& value, bool always = false) {
  if (value.empty() && !always) {
    return;
  }
  check(item.putAndInsertString(tag, value.c_str()), DcmTag(tag).getTagName());
}

std::string getString(DcmItem& item, const DcmTagKey& tag) {
  OFString value;
  if (item.findAndGetOFStringArray(tag, value).good()) {
    return value.c_str();
  }
  return {};
}

std::optional<double> getDouble(DcmItem& item, const DcmTagKey& tag, unsigned long index = 0) {
  Float64 value = 0.0;
  if (item.findAndGetFloat64(tag, value, index).good()) {
    return value;
  }
  return std::nullopt;
}

/// An integer string (IS); 0 when missing.
int getInteger(DcmItem& item, const DcmTagKey& tag) {
  Sint32 value = 0;
  return item.findAndGetSint32(tag, value).good() ? static_cast<int>(value) : 0;
}

std::vector<std::string> splitValues(const std::string& text) {
  std::vector<std::string> values;
  std::stringstream stream(text);
  std::string value;
  while (std::getline(stream, value, '\\')) {
    if (!value.empty()) {
      values.push_back(value);
    }
  }
  return values;
}

std::string joinValues(const std::vector<std::string>& values, std::size_t max_each) {
  std::string text;
  for (const std::string& value : values) {
    text += (text.empty() ? "" : "\\") + fit(value, max_each);
  }
  return text;
}

/// The attributes every slice of a series shares.
void putCommon(DcmDataset& data, const DicondeWriteOptions& options, const DicondeSeries& series,
               const VoxelSize& size, const ValueMapping& mapping, int bits_stored,
               const std::array<std::int64_t, 3>& dims) {
  const DicondeComponent& component = options.component;
  const DicondeInspection& inspection = options.inspection;
  const DicondeEquipment& equipment = options.equipment;
  // SOP Common: UTF-8 text.
  put(data, DCM_SpecificCharacterSet, "ISO_IR 192");
  put(data, DCM_SOPClassUID, UID_CTImageStorage);
  // Component (Patient) module.
  put(data, DCM_PatientName, fit(component.name, kLongString), true);
  put(data, DCM_PatientID, fit(component.id, kLongString), true);
  put(data, DCM_PatientBirthDate, dicomDate(component.manufacturing_date), true);
  put(data, DCM_PatientSex, "", true);
  put(data, otherComponentIdsTag(), joinValues(component.other_ids, kLongString));
  if (!component.material.empty() && component.material.size() <= kShortString) {
    put(data, DCM_Occupation, fit(component.material, kShortString));  // Material Name
  }
  put(data, DCM_PatientComments, fit(component.notes, kLongText, false));
  put(data, DCM_ComponentManufacturer, fit(component.manufacturer, kShortText, false));
  put(data, DCM_MaterialGrade, fit(component.material, kShortText, false));
  // Component Study module.
  put(data, DCM_StudyInstanceUID, series.study_uid);
  put(data, DCM_StudyDate, dicomDate(inspection.date), true);
  put(data, DCM_StudyTime, "", true);
  put(data, DCM_ReferringPhysicianName, fit(inspection.owner, kLongString), true);
  put(data, DCM_StudyID, fit(inspection.study_id, kShortString), true);
  put(data, DCM_AccessionNumber, fit(inspection.accession, kShortString), true);
  put(data, DCM_StudyDescription, fit(inspection.description, kLongString));
  put(data, DCM_InstitutionName, fit(inspection.company, kLongString));
  put(data, DCM_InstitutionAddress, fit(inspection.company_address, kShortText, false));
  put(data, DCM_PerformingPhysicianName, joinValues(inspection.inspectors, kLongString));
  put(data, DCM_NameOfPhysiciansReadingStudy, fit(inspection.certifying_inspector, kLongString));
  put(data, examinationNotesTag(), fit(inspection.notes, kLongText, false));
  // Series and frame of reference.
  put(data, DCM_Modality, "CT");
  put(data, DCM_SeriesInstanceUID, series.series_uid);
  put(data, DCM_SeriesNumber, std::to_string(options.series_number));
  put(data, DCM_SeriesDescription, fit(options.series_description, kLongString));
  put(data, DCM_FrameOfReferenceUID, series.frame_of_reference_uid);
  put(data, DCM_PositionReferenceIndicator, "", true);
  // Equipment and acquisition.
  put(data, DCM_Manufacturer, fit(equipment.manufacturer, kLongString), true);
  put(data, DCM_ManufacturerModelName, fit(equipment.model, kLongString));
  put(data, DCM_SoftwareVersions, std::string("VoxelSieve ") + VOXELSIEVE_VERSION);
  put(data, DCM_KVP, equipment.kv ? decimal(*equipment.kv) : std::string(), true);
  if (equipment.current_ua) {
    put(data, DCM_XRayTubeCurrentInuA, decimal(*equipment.current_ua));
  }
  if (equipment.exposure_ms) {
    check(data.putAndInsertFloat64(DCM_ExposureTimeInms, *equipment.exposure_ms),
          "ExposureTimeInms");
  }
  put(data, DCM_FilterType, fit(equipment.filter, kShortString));
  put(data, DCM_DetectorDescription, fit(equipment.detector, kLongText, false));
  put(data, DCM_AcquisitionDeviceProcessingDescription, fit(equipment.reconstruction, kLongString));
  put(data, DCM_AcquisitionNumber, "", true);
  // Image.
  put(data, DCM_ImageType,
      options.derived ? "DERIVED\\SECONDARY\\AXIAL" : "ORIGINAL\\PRIMARY\\AXIAL");
  put(data, DCM_ImageComments, fit(options.image_comments, kLongText, false));
  const auto& r = options.pose.rotation;
  put(data, DCM_ImageOrientationPatient, decimals({r[0], r[3], r[6], r[1], r[4], r[7]}));
  put(data, DCM_PixelSpacing, decimals({size[1], size[0]}));
  put(data, DCM_SliceThickness, decimal(size.sliceThicknessMm()));
  put(data, DCM_SpacingBetweenSlices, decimal(size[2]));
  check(data.putAndInsertUint16(DCM_SamplesPerPixel, 1), "SamplesPerPixel");
  put(data, DCM_PhotometricInterpretation, "MONOCHROME2");
  check(data.putAndInsertUint16(DCM_Rows, static_cast<Uint16>(dims[1])), "Rows");
  check(data.putAndInsertUint16(DCM_Columns, static_cast<Uint16>(dims[0])), "Columns");
  check(data.putAndInsertUint16(DCM_BitsAllocated, 16), "BitsAllocated");
  check(data.putAndInsertUint16(DCM_BitsStored, static_cast<Uint16>(bits_stored)), "BitsStored");
  check(data.putAndInsertUint16(DCM_HighBit, static_cast<Uint16>(bits_stored - 1)), "HighBit");
  check(data.putAndInsertUint16(DCM_PixelRepresentation, 0), "PixelRepresentation");
  put(data, DCM_RescaleIntercept, decimal(mapping.offset));
  put(data, DCM_RescaleSlope, decimal(mapping.scale));
  put(data, DCM_RescaleType, "US");
}

/// Adds an indication to the NDE Indication module of a slice.
void putIndication(DcmDataset& data, const DicondeIndication& indication,
                   const std::string& sop_instance_uid) {
  DcmItem* item = nullptr;
  check(data.findOrCreateSequenceItem(DCM_IndicationSequence, item, -2), "IndicationSequence");
  put(*item, DCM_IndicationNumber, std::to_string(indication.number));
  put(*item, DCM_IndicationLabel, fit(indication.label, kShortString));
  put(*item, DCM_IndicationDescription, fit(indication.description, kShortText, false));
  put(*item, DCM_IndicationType, codeString(indication.type));
  put(*item, DCM_IndicationDisposition, codeString(indication.disposition));
  DcmItem* roi = nullptr;
  check(item->findOrCreateSequenceItem(DCM_IndicationROISequence, roi, -2), "IndicationROI");
  put(*roi, DCM_ReferencedSOPClassUID, UID_CTImageStorage);
  put(*roi, DCM_ReferencedSOPInstanceUID, sop_instance_uid);
  put(*roi, DCM_ContourGeometricType, "POINT");
  put(*roi, DCM_NumberOfContourPoints, "1");
  const Vec3& c = indication.center_mm;
  put(*roi, DCM_ContourData, decimals({c[0], c[1], c[2]}));
  for (const DicondeProperty& property : indication.properties) {
    DcmItem* entry = nullptr;
    check(item->findOrCreateSequenceItem(DCM_IndicationPhysicalPropertySequence, entry, -2),
          "IndicationPhysicalPropertySequence");
    put(*entry, DCM_PropertyLabel, fit(property.label, kShortString));
    DcmItem* measured = nullptr;
    check(entry->findOrCreateSequenceItem(DCM_MeasuredValueSequence, measured, -2),
          "MeasuredValueSequence");
    put(*measured, DCM_NumericValue, decimal(property.value));
    DcmItem* unit = nullptr;
    check(measured->findOrCreateSequenceItem(DCM_MeasurementUnitsCodeSequence, unit, -2),
          "MeasurementUnitsCodeSequence");
    const std::string code = fit(property.unit.empty() ? "1" : property.unit, kShortString);
    put(*unit, DCM_CodeValue, code);
    put(*unit, DCM_CodingSchemeDesignator, "UCUM");
    put(*unit, DCM_CodeMeaning, code);
  }
}

std::vector<DicondeIndication> readIndications(DcmItem& data) {
  std::vector<DicondeIndication> indications;
  DcmItem* item = nullptr;
  for (unsigned long i = 0;
       data.findAndGetSequenceItem(DCM_IndicationSequence, item, static_cast<int>(i)).good(); ++i) {
    DicondeIndication indication;
    indication.number = getInteger(*item, DCM_IndicationNumber);
    indication.label = getString(*item, DCM_IndicationLabel);
    indication.description = getString(*item, DCM_IndicationDescription);
    indication.type = getString(*item, DCM_IndicationType);
    indication.disposition = getString(*item, DCM_IndicationDisposition);
    DcmItem* roi = nullptr;
    if (item->findAndGetSequenceItem(DCM_IndicationROISequence, roi, 0).good()) {
      for (unsigned long axis = 0; axis < 3; ++axis) {
        indication.center_mm[axis] = getDouble(*roi, DCM_ContourData, axis).value_or(0.0);
      }
    }
    DcmItem* entry = nullptr;
    for (int p = 0;
         item->findAndGetSequenceItem(DCM_IndicationPhysicalPropertySequence, entry, p).good();
         ++p) {
      DicondeProperty property;
      property.label = getString(*entry, DCM_PropertyLabel);
      DcmItem* measured = nullptr;
      if (entry->findAndGetSequenceItem(DCM_MeasuredValueSequence, measured, 0).good()) {
        property.value = getDouble(*measured, DCM_NumericValue).value_or(0.0);
        DcmItem* unit = nullptr;
        if (measured->findAndGetSequenceItem(DCM_MeasurementUnitsCodeSequence, unit, 0).good()) {
          property.unit = getString(*unit, DCM_CodeValue);
        }
      }
      indication.properties.push_back(property);
    }
    indications.push_back(std::move(indication));
  }
  return indications;
}

/// Level 0 of a dataset as volume source; removed air reads as the air level.
class DatasetSource final : public VolumeSource {
 public:
  explicit DatasetSource(const Dataset& dataset) : dataset_(dataset) {}
  [[nodiscard]] std::array<std::int64_t, 3> dims() const override { return dataset_.info().dims; }
  [[nodiscard]] VoxelSize voxelSize() const override { return dataset_.info().voxel_size; }
  [[nodiscard]] ValueMapping valueMapping() const override { return dataset_.info().value_mapping; }
  [[nodiscard]] SampleType sampleType() const override {
    return dataset_.info().value_type == ValueType::kUInt8 ? SampleType::kUInt8
                                                           : SampleType::kUInt16;
  }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override {
    std::vector<float> values(out.size());
    dataset_.readRegion(0, box, values, dataset_.info().air_level);
    std::ranges::transform(values, out.begin(), [](float value) {
      return static_cast<std::uint16_t>(std::clamp(std::lround(value), 0L, 65535L));
    });
  }

 private:
  const Dataset& dataset_;
};

/// A material volume as volume source of labels.
class MaterialSource final : public VolumeSource {
 public:
  explicit MaterialSource(const MaterialVolume& materials) : materials_(materials) {}
  [[nodiscard]] std::array<std::int64_t, 3> dims() const override { return materials_.info().dims; }
  [[nodiscard]] VoxelSize voxelSize() const override { return materials_.info().voxel_size; }
  [[nodiscard]] SampleType sampleType() const override { return SampleType::kUInt8; }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override {
    std::vector<std::uint8_t> labels(out.size());
    materials_.readRegion(box, labels);
    std::ranges::copy(labels, out.begin());
  }

 private:
  const MaterialVolume& materials_;
};

std::string text(const Json& object, const char* section, const char* key) {
  if (!object.contains(section) || !object.at(section).is_object() ||
      !object.at(section).contains(key)) {
    return {};
  }
  const Json& value = object.at(section).at(key);
  return value.is_string() ? value.get<std::string>() : value.is_null() ? "" : value.dump();
}

std::optional<double> number(const Json& object, const char* section, const char* key) {
  if (object.contains(section) && object.at(section).is_object() &&
      object.at(section).contains(key) && object.at(section).at(key).is_number()) {
    return object.at(section).at(key).get<double>();
  }
  return std::nullopt;
}

std::string formatted(double value, int digits) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(digits) << value;
  return out.str();
}

}  // namespace

DicondeSeries writeDicondeSeries(const VolumeSource& source, const std::filesystem::path& dir,
                                 const DicondeWriteOptions& options) {
  const TelemetryPhase phase("write DICONDE");
  const auto dims = source.dims();
  constexpr std::int64_t kMaxSide = std::numeric_limits<Uint16>::max();
  if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0 || dims[0] > kMaxSide || dims[1] > kMaxSide) {
    throw std::invalid_argument("DICONDE slices hold 1 to 65535 rows and columns");
  }
  if (std::filesystem::exists(dir) && !std::filesystem::is_empty(dir)) {
    throw std::invalid_argument("Output directory is not empty: " + dir.string());
  }
  std::filesystem::create_directories(dir);
  DicondeSeries series;
  series.study_uid = options.inspection.study_uid.empty() ? newUid() : options.inspection.study_uid;
  series.series_uid = options.series_uid.empty() ? newUid() : options.series_uid;
  series.frame_of_reference_uid =
      options.frame_of_reference_uid.empty() ? newUid() : options.frame_of_reference_uid;
  series.files = dims[2];

  const VoxelSize size = source.voxelSize();
  const int bits_stored = source.sampleType() == SampleType::kUInt8 ? 8 : 16;
  DcmDataset common;
  putCommon(common, options, series, size, source.valueMapping(), bits_stored, dims);

  std::vector<std::string> instance_uids(static_cast<std::size_t>(dims[2]));
  for (std::string& uid : instance_uids) {
    uid = newUid();
  }
  // Each indication goes with the slice through its centre.
  std::map<std::int64_t, std::vector<const DicondeIndication*>> by_slice;
  const RigidTransform to_volume = options.pose.inverse();
  for (const DicondeIndication& indication : options.indications) {
    const double z = to_volume.apply(indication.center_mm)[2] / size[2];
    by_slice[std::clamp<std::int64_t>(std::llround(z), 0, dims[2] - 1)].push_back(&indication);
  }
  const Vec3 normal{options.pose.rotation[2], options.pose.rotation[5], options.pose.rotation[8]};
  const auto slice_voxels = static_cast<std::size_t>(dims[0] * dims[1]);
  std::mutex common_mutex;
  tbb::parallel_for(std::int64_t{0}, dims[2], [&](std::int64_t z) {
    std::vector<std::uint16_t> pixels(slice_voxels);
    source.readRegion({{0, 0, z}, {dims[0], dims[1], z + 1}}, pixels);
    DcmFileFormat file;
    DcmDataset& data = *file.getDataset();
    {
      // Copying walks DCMTK's element lists with a cursor kept in the list, so not at once.
      const std::lock_guard lock(common_mutex);
      data = common;
    }
    const std::string& uid = instance_uids[static_cast<std::size_t>(z)];
    put(data, DCM_SOPInstanceUID, uid);
    put(data, DCM_InstanceNumber, std::to_string(z + 1));
    const Vec3 position = options.pose.apply(size.toMm({0.0, 0.0, static_cast<double>(z)}));
    put(data, DCM_ImagePositionPatient, decimals({position[0], position[1], position[2]}));
    put(data, DCM_SliceLocation,
        decimal(position[0] * normal[0] + position[1] * normal[1] + position[2] * normal[2]));
    if (const auto found = by_slice.find(z); found != by_slice.end()) {
      for (const DicondeIndication* indication : found->second) {
        putIndication(data, *indication, uid);
      }
    }
    static_assert(sizeof(Uint16) == sizeof(std::uint16_t));
    check(data.putAndInsertUint16Array(DCM_PixelData, pixels.data(),
                                       static_cast<unsigned long>(pixels.size())),
          "PixelData");
    std::array<char, 32> name{};
    (void)std::snprintf(name.data(), name.size(), "slice_%05lld.dcm", static_cast<long long>(z));
    const std::filesystem::path path = dir / name.data();
    check(file.saveFile(path.string().c_str(), EXS_LittleEndianExplicit, EET_ExplicitLength,
                        EGL_recalcGL, EPD_noChange, 0, 0, EWM_fileformat),
          "writing " + path.string());
  });
  return series;
}

DicondeSeries writeDicondeDataset(const Dataset& dataset, const std::filesystem::path& dir,
                                  const DicondeWriteOptions& options) {
  const DatasetSource source(dataset);
  return writeDicondeSeries(source, dir, options);
}

DicondeSeries writeDicondeMaterials(const MaterialVolume& materials,
                                    const std::filesystem::path& dir,
                                    const DicondeWriteOptions& options) {
  DicondeWriteOptions labels = options;
  labels.derived = true;
  if (labels.series_description.empty()) {
    labels.series_description = "Materials";
  }
  if (labels.image_comments.empty()) {
    labels.image_comments = "Labels: 0 air";
    for (const Material& material : materials.info().materials) {
      labels.image_comments += "; " + std::to_string(material.id) + " " + material.name;
    }
  }
  const MaterialSource source(materials);
  return writeDicondeSeries(source, dir, labels);
}

std::vector<DicondeIndication> porosityIndications(const PorosityResult& result,
                                                   const RigidTransform& pose) {
  std::vector<DicondeIndication> indications;
  const auto place = [&](const std::array<double, 3>& center_voxels) {
    return pose.apply(result.voxel_size.toMm(center_voxels));
  };
  for (const DetectedPore& pore : result.pores) {
    DicondeIndication indication;
    indication.number = static_cast<int>(indications.size()) + 1;
    indication.label = "Pore " + std::to_string(pore.id);
    indication.type = "PORE";
    const double size_mm = poreSizeMm(pore, result.voxel_size);
    indication.description = "Pore " + std::to_string(pore.id) + ": " +
                             formatted(pore.volume_mm3, 4) + " mm3, " + formatted(size_mm, 3) +
                             " mm largest extent";
    indication.center_mm = place(pore.center_voxels);
    indication.properties = {{"Volume", pore.volume_mm3, "mm3"},
                             {"Size", size_mm, "mm"},
                             {"Eq. diameter", pore.equivalent_diameter_mm, "mm"},
                             {"Voxels", static_cast<double>(pore.voxel_count), "{voxels}"}};
    indications.push_back(std::move(indication));
  }
  for (const PorosityZone& zone : result.zones) {
    DicondeIndication indication;
    indication.number = static_cast<int>(indications.size()) + 1;
    indication.label = "Zone " + std::to_string(zone.id);
    indication.type = "LOOSENING";
    indication.description = "Loosened microstructure, zone " + std::to_string(zone.id) + ": " +
                             formatted(zone.volume_mm3, 3) + " mm3, porosity " +
                             formatted(100.0 * zone.porosity(), 2) + " %";
    indication.center_mm = place(zone.center_voxels);
    indication.properties = {{"Volume", zone.volume_mm3, "mm3"},
                             {"Void volume", zone.void_volume_mm3, "mm3"},
                             {"Porosity", 100.0 * zone.porosity(), "%"}};
    indications.push_back(std::move(indication));
  }
  return indications;
}

void applyInspectionOrder(const Json& order, DicondeWriteOptions& options,
                          const Evaluation* evaluation) {
  DicondeComponent& component = options.component;
  component.name = text(order, "part", "name");
  component.id = text(order, "part", "serial");
  component.other_ids.clear();
  if (const std::string drawing = text(order, "part", "drawing"); !drawing.empty()) {
    component.other_ids.push_back(drawing);
  }
  component.manufacturer = text(order, "part", "manufacturer");
  component.material = text(order, "part", "material");
  component.notes = text(order, "part", "condition");

  DicondeInspection& inspection = options.inspection;
  inspection.study_id = text(order, "report", "number");
  inspection.accession = text(order, "order", "number");
  inspection.date = text(order, "order", "test_date");
  if (inspection.date.empty()) {
    inspection.date = text(order, "report", "date");
  }
  const std::string guideline = text(order, "acceptance", "guideline");
  inspection.description = guideline.empty() ? "CT inspection" : "CT inspection, " + guideline;
  inspection.company = text(order, "laboratory", "name");
  inspection.company_address = text(order, "laboratory", "address");
  inspection.owner = text(order, "customer", "name");
  inspection.inspectors.clear();
  if (const std::string inspector = text(order, "approval", "inspector"); !inspector.empty()) {
    inspection.inspectors.push_back(inspector);
  }
  inspection.certifying_inspector = text(order, "approval", "approver");

  DicondeEquipment& equipment = options.equipment;
  equipment.model = text(order, "scan", "device");
  equipment.kv = number(order, "scan", "voltage_kv");
  equipment.current_ua = number(order, "scan", "current_ua");
  equipment.exposure_ms = number(order, "scan", "exposure_ms");
  equipment.filter = text(order, "scan", "filter");
  equipment.detector = text(order, "scan", "detector");
  equipment.reconstruction = text(order, "scan", "reconstruction");

  if (evaluation != nullptr && !evaluation->zones.empty()) {
    std::string notes = "Evaluation";
    notes += guideline.empty() ? "" : " to " + guideline;
    notes += ": " + std::string(evaluation->passed() ? "passed" : "failed");
    for (const ZoneEvaluation& zone : evaluation->zones) {
      notes += "\n" + zone.name + ": " + (zone.passed() ? "passed" : "failed") + "; " +
               std::to_string(zone.pore_count) + " pores, largest " +
               formatted(zone.largest_pore_size_mm, 3) + " mm, porosity " +
               formatted(zone.porosity_percent, 3) + " %";
      for (const CriterionResult& criterion : zone.criteria) {
        if (!criterion.passed) {
          notes += "; " + criterion.name + " " + formatted(criterion.measured, 3) + " > " +
                   formatted(criterion.limit, 3);
        }
      }
    }
    inspection.notes = notes;
  }
}

DicondeInfo readDicondeInfo(const DicomStackSource& source) {
  const auto& files = source.files();
  const auto load = [](const std::filesystem::path& path, DcmFileFormat& file) {
    check(file.loadFileUntilTag(path.string().c_str(), EXS_Unknown, EGL_noChange, DCM_MaxReadLength,
                                ERM_autoDetect, DCM_PixelData),
          "reading " + path.string());
  };
  DicondeInfo info;
  {
    DcmFileFormat file;
    load(files.front(), file);
    DcmDataset& data = *file.getDataset();
    DicondeComponent& component = info.component;
    component.name = getString(data, DCM_PatientName);
    component.id = getString(data, DCM_PatientID);
    component.other_ids = splitValues(getString(data, otherComponentIdsTag()));
    component.manufacturer = getString(data, DCM_ComponentManufacturer);
    component.manufacturing_date = getString(data, DCM_PatientBirthDate);
    component.material = getString(data, DCM_MaterialGrade);
    if (component.material.empty()) {
      component.material = getString(data, DCM_Occupation);
    }
    component.notes = getString(data, DCM_PatientComments);
    DicondeInspection& inspection = info.inspection;
    inspection.study_uid = getString(data, DCM_StudyInstanceUID);
    inspection.study_id = getString(data, DCM_StudyID);
    inspection.accession = getString(data, DCM_AccessionNumber);
    inspection.date = getString(data, DCM_StudyDate);
    inspection.description = getString(data, DCM_StudyDescription);
    inspection.company = getString(data, DCM_InstitutionName);
    inspection.company_address = getString(data, DCM_InstitutionAddress);
    inspection.owner = getString(data, DCM_ReferringPhysicianName);
    inspection.inspectors = splitValues(getString(data, DCM_PerformingPhysicianName));
    inspection.certifying_inspector = getString(data, DCM_NameOfPhysiciansReadingStudy);
    inspection.notes = getString(data, examinationNotesTag());
    DicondeEquipment& equipment = info.equipment;
    equipment.manufacturer = getString(data, DCM_Manufacturer);
    equipment.model = getString(data, DCM_ManufacturerModelName);
    equipment.kv = getDouble(data, DCM_KVP);
    equipment.current_ua = getDouble(data, DCM_XRayTubeCurrentInuA);
    equipment.exposure_ms = getDouble(data, DCM_ExposureTimeInms);
    equipment.filter = getString(data, DCM_FilterType);
    equipment.detector = getString(data, DCM_DetectorDescription);
    equipment.reconstruction = getString(data, DCM_AcquisitionDeviceProcessingDescription);
    info.modality = getString(data, DCM_Modality);
    info.series_uid = getString(data, DCM_SeriesInstanceUID);
    info.frame_of_reference_uid = getString(data, DCM_FrameOfReferenceUID);
    info.series_description = getString(data, DCM_SeriesDescription);
    info.series_number = getInteger(data, DCM_SeriesNumber);
    info.derived = getString(data, DCM_ImageType).starts_with("DERIVED");
    info.image_comments = getString(data, DCM_ImageComments);
  }
  std::vector<std::vector<DicondeIndication>> per_file(files.size());
  tbb::parallel_for(std::size_t{0}, files.size(), [&](std::size_t i) {
    DcmFileFormat file;
    load(files[i], file);
    per_file[i] = readIndications(*file.getDataset());
  });
  for (auto& indications : per_file) {
    std::ranges::move(indications, std::back_inserter(info.indications));
  }
  std::ranges::stable_sort(info.indications, {}, &DicondeIndication::number);
  return info;
}

Json toJson(const DicondeInfo& info) {
  const auto optional = [](const std::optional<double>& value) {
    return value ? Json(*value) : Json(nullptr);
  };
  Json indications = Json::array();
  for (const DicondeIndication& indication : info.indications) {
    Json properties = Json::array();
    for (const DicondeProperty& property : indication.properties) {
      properties.push_back(
          {{"label", property.label}, {"value", property.value}, {"unit", property.unit}});
    }
    indications.push_back({{"number", indication.number},
                           {"label", indication.label},
                           {"description", indication.description},
                           {"type", indication.type},
                           {"disposition", indication.disposition},
                           {"center_mm", indication.center_mm},
                           {"properties", properties}});
  }
  const DicondeComponent& c = info.component;
  const DicondeInspection& s = info.inspection;
  const DicondeEquipment& e = info.equipment;
  return {{"component",
           {{"name", c.name},
            {"id", c.id},
            {"other_ids", c.other_ids},
            {"manufacturer", c.manufacturer},
            {"manufacturing_date", c.manufacturing_date},
            {"material", c.material},
            {"notes", c.notes}}},
          {"inspection",
           {{"study_uid", s.study_uid},
            {"study_id", s.study_id},
            {"accession", s.accession},
            {"date", s.date},
            {"description", s.description},
            {"company", s.company},
            {"company_address", s.company_address},
            {"owner", s.owner},
            {"inspectors", s.inspectors},
            {"certifying_inspector", s.certifying_inspector},
            {"notes", s.notes}}},
          {"equipment",
           {{"manufacturer", e.manufacturer},
            {"model", e.model},
            {"kv", optional(e.kv)},
            {"current_ua", optional(e.current_ua)},
            {"exposure_ms", optional(e.exposure_ms)},
            {"filter", e.filter},
            {"detector", e.detector},
            {"reconstruction", e.reconstruction}}},
          {"modality", info.modality},
          {"series_uid", info.series_uid},
          {"frame_of_reference_uid", info.frame_of_reference_uid},
          {"series_description", info.series_description},
          {"series_number", info.series_number},
          {"derived", info.derived},
          {"image_comments", info.image_comments},
          {"indications", indications}};
}

}  // namespace voxelsieve
