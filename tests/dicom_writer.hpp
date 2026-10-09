#pragma once

// Synthetic DICOM slices for the tests, written with DCMTK.

// clang-format off
#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcrleerg.h>
// clang-format on

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace voxelsieve::testing {

struct DicomSliceSpec {
  std::uint16_t rows = 0;
  std::uint16_t columns = 0;
  std::string series = "1.2.826.0.1.3680043.2.1125.1";
  std::optional<int> instance;
  std::optional<std::array<double, 3>> position;
  std::array<double, 6> orientation{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
  std::array<double, 2> pixel_spacing{1.0, 1.0};  // between rows, between columns
  std::optional<double> thickness;
  std::optional<double> spacing_between_slices;
  bool is_signed = true;
  int bits_stored = 16;
  double slope = 1.0;
  double intercept = 0.0;
  E_TransferSyntax transfer = EXS_LittleEndianExplicit;
  /// False: the data set alone, without preamble and meta header, as some scanners write it.
  bool file_format = true;
};

inline std::string decimals(std::initializer_list<double> values) {
  std::string text;
  for (const double value : values) {
    std::array<char, 32> buffer{};
    (void)std::snprintf(buffer.data(), buffer.size(), "%.8g", value);
    text += (text.empty() ? "" : "\\") + std::string(buffer.data());
  }
  return text;
}

/// Writes one slice; `samples` are the stored values (x fastest), whose bits above `bits_stored`
/// are written as they are.
inline void writeDicomSlice(const std::filesystem::path& path, const DicomSliceSpec& spec,
                            const std::vector<std::int32_t>& samples) {
  static const bool registered = [] {
    DcmRLEEncoderRegistration::registerCodecs();
    return true;
  }();
  (void)registered;
  DcmFileFormat file;
  DcmDataset& data = *file.getDataset();
  std::array<char, 128> uid{};
  const auto check = [&path](const OFCondition& status) {
    if (status.bad()) {
      throw std::runtime_error("Writing " + path.string() + ": " + status.text());
    }
  };
  check(data.putAndInsertString(DCM_SOPClassUID, UID_CTImageStorage));
  check(data.putAndInsertString(DCM_SOPInstanceUID, dcmGenerateUniqueIdentifier(uid.data())));
  check(data.putAndInsertString(DCM_Modality, "CT"));
  check(data.putAndInsertString(DCM_SeriesInstanceUID, spec.series.c_str()));
  if (spec.instance) {
    check(data.putAndInsertString(DCM_InstanceNumber, std::to_string(*spec.instance).c_str()));
  }
  if (spec.position) {
    const auto& p = *spec.position;
    check(data.putAndInsertString(DCM_ImagePositionPatient, decimals({p[0], p[1], p[2]}).c_str()));
    const auto& o = spec.orientation;
    check(data.putAndInsertString(DCM_ImageOrientationPatient,
                                  decimals({o[0], o[1], o[2], o[3], o[4], o[5]}).c_str()));
  }
  check(data.putAndInsertString(DCM_PixelSpacing,
                                decimals({spec.pixel_spacing[0], spec.pixel_spacing[1]}).c_str()));
  if (spec.thickness) {
    check(data.putAndInsertString(DCM_SliceThickness, decimals({*spec.thickness}).c_str()));
  }
  if (spec.spacing_between_slices) {
    check(data.putAndInsertString(DCM_SpacingBetweenSlices,
                                  decimals({*spec.spacing_between_slices}).c_str()));
  }
  check(data.putAndInsertString(DCM_RescaleSlope, decimals({spec.slope}).c_str()));
  check(data.putAndInsertString(DCM_RescaleIntercept, decimals({spec.intercept}).c_str()));
  check(data.putAndInsertUint16(DCM_SamplesPerPixel, 1));
  check(data.putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2"));
  check(data.putAndInsertUint16(DCM_Rows, spec.rows));
  check(data.putAndInsertUint16(DCM_Columns, spec.columns));
  check(data.putAndInsertUint16(DCM_BitsAllocated, 16));
  check(data.putAndInsertUint16(DCM_BitsStored, static_cast<Uint16>(spec.bits_stored)));
  check(data.putAndInsertUint16(DCM_HighBit, static_cast<Uint16>(spec.bits_stored - 1)));
  check(data.putAndInsertUint16(DCM_PixelRepresentation, Uint16{spec.is_signed ? 1U : 0U}));
  std::vector<Uint16> words(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    words[i] = static_cast<Uint16>(samples[i]);  // two's complement for negative values
  }
  check(data.putAndInsertUint16Array(DCM_PixelData, words.data(),
                                     static_cast<unsigned long>(words.size())));
  if (DcmXfer(spec.transfer).usesEncapsulatedFormat()) {
    check(data.chooseRepresentation(spec.transfer, nullptr));
  }
  std::filesystem::create_directories(path.parent_path());
  check(file.saveFile(path.string().c_str(), spec.transfer, EET_ExplicitLength, EGL_recalcGL,
                      EPD_noChange, 0, 0, spec.file_format ? EWM_fileformat : EWM_dataset));
}

}  // namespace voxelsieve::testing
