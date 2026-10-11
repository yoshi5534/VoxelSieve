#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/dicom.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/transform.hpp"

namespace voxelsieve {

/// DICONDE (ASTM E2339, DICOM for nondestructive evaluation) as input and output (ADR 0023).
/// DICONDE files are DICOM files: a CT volume is a series of CT images, one file per slice, which
/// `DicomStackSource` reads. What DICONDE adds is the meaning of the metadata: the patient is the
/// inspected component, the study the inspection, and group 0014 holds the material and the
/// indications (findings such as pores) of the evaluation. VoxelSieve writes volumes, label
/// volumes of material segmentations and porosity results as indications, and reads all of it
/// back.

/// The inspected part: the Component module (the Patient module of DICOM, renamed, plus the NDE
/// attributes of group 0014).
struct DicondeComponent {
  std::string name;                    // Component Name (0010,0010)
  std::string id;                      // Component ID Number (0010,0020): serial number, batch
  std::vector<std::string> other_ids;  // Other Component IDs (0010,1000): drawing number
  std::string manufacturer;            // Component Manufacturer (0014,0028)
  std::string manufacturing_date;      // Component Manufacturing Date (0010,0030), YYYYMMDD
  /// Material Grade (0014,0042); also Material Name (0010,2160) when it fits its 16 characters.
  std::string material;
  std::string notes;  // Component Notes (0010,4000): condition of the part
};

/// The inspection: the Component Study module and the people and company of the NDE study.
struct DicondeInspection {
  std::string study_uid;                // Study Instance UID; empty: a new one
  std::string study_id;                 // Study ID (0020,0010): the report number
  std::string accession;                // Accession Number (0008,0050): the order number
  std::string date;                     // Study Date, YYYYMMDD
  std::string description;              // Study Description
  std::string company;                  // Company Name (0008,0080): the testing laboratory
  std::string company_address;          // Company Address (0008,0081)
  std::string owner;                    // Component Owner Name (0008,0090): the customer
  std::vector<std::string> inspectors;  // Inspector Name (0008,1050)
  std::string certifying_inspector;     // Certifying Inspector Name (0008,1060)
  std::string notes;                    // Examination Notes (0032,4000): the evaluation
};

/// The scanner and how it was used: General Equipment and CT acquisition attributes.
struct DicondeEquipment {
  std::string manufacturer;           // Manufacturer (0008,0070)
  std::string model;                  // Manufacturer's Model Name (0008,1090)
  std::optional<double> kv;           // KVP (0018,0060)
  std::optional<double> current_ua;   // X-Ray Tube Current in uA (0018,8151)
  std::optional<double> exposure_ms;  // Exposure Time in ms (0018,9328)
  std::string filter;                 // Filter Type (0018,1160)
  std::string detector;               // Detector Description (0018,7006)
  std::string reconstruction;         // Acquisition Device Processing Description (0018,1400)
};

/// A measured property of an indication, such as its volume.
struct DicondeProperty {
  std::string label;  // Property Label (0014,2032)
  double value = 0.0;
  std::string unit;  // UCUM code, such as "mm", "mm3" or "%"
  bool operator==(const DicondeProperty&) const = default;
};

/// An indication of the NDE Indication module: one finding, such as a pore.
struct DicondeIndication {
  int number = 0;           // Indication Number (0014,2014)
  std::string label;        // Indication Label (0014,2016), at most 16 characters
  std::string description;  // Indication Description (0014,2018)
  std::string type;         // Indication Type (0014,201A): "PORE", "LOOSENING", ...
  std::string disposition;  // Indication Disposition (0014,201C), optional
  /// Where it lies, in the coordinates of the series (those of Image Position, mm). It is
  /// stored with the slice through it, as the point of its Indication ROI.
  std::array<double, 3> center_mm{};
  std::vector<DicondeProperty> properties;
};

struct DicondeWriteOptions {
  DicondeComponent component;
  DicondeInspection inspection;
  DicondeEquipment equipment;
  /// Series Instance UID; empty: a new one.
  std::string series_uid;
  /// Frame of Reference UID; empty: a new one. Series that lie in the same coordinates, such as
  /// a volume and its segmentation, share it.
  std::string frame_of_reference_uid;
  int series_number = 1;
  std::string series_description;
  /// Where the volume lies: from voxel coordinates (index times pitch, voxel centres at
  /// integers) to mm, written as Image Position and Image Orientation (ADR 0018).
  RigidTransform pose;
  /// Image Type DERIVED\SECONDARY instead of ORIGINAL\PRIMARY, for results such as label volumes.
  bool derived = false;
  /// Image Comments (0020,4000) of every slice, such as the meaning of the labels.
  std::string image_comments;
  /// Findings, each written with the slice through its centre.
  std::vector<DicondeIndication> indications;
};

/// UIDs of a written series.
struct DicondeSeries {
  std::string study_uid;
  std::string series_uid;
  std::string frame_of_reference_uid;
  std::int64_t files = 0;
};

/// Writes `source` as a DICONDE CT series into `dir` (created; must not hold files): one CT image
/// per slice (slice_00000.dcm, ...), explicit VR little endian, unsigned samples of 16 bits with
/// as many bits stored as the source's sample type has. Grey values stay exact; the value mapping
/// of the source (ADR 0015) becomes the rescale slope and intercept, so that a signed DICOM scan
/// gets its stored values back. Reads the source slice by slice, in parallel.
DicondeSeries writeDicondeSeries(const VolumeSource& source, const std::filesystem::path& dir,
                                 const DicondeWriteOptions& options = {});

/// Writes the level 0 of a dataset as a DICONDE series; removed outside air gets the dataset's
/// air level, so the slices show the whole scanned box.
DicondeSeries writeDicondeDataset(const Dataset& dataset, const std::filesystem::path& dir,
                                  const DicondeWriteOptions& options = {});

/// Writes a material volume as a derived DICONDE series of labels (0 air, then the material ids)
/// with 8 bits stored. The Image Comments name the materials ("1 Aluminium; 2 Steel"); give it the
/// frame of reference of the volume it was segmented from.
DicondeSeries writeDicondeMaterials(const MaterialVolume& materials,
                                    const std::filesystem::path& dir,
                                    const DicondeWriteOptions& options = {});

/// The pores and loosened zones of a porosity result as indications ("PORE", "LOOSENING") with
/// their volume, size and, for zones, porosity. `pose` places the dataset's voxel coordinates
/// (index times pitch) in the coordinates of the series.
[[nodiscard]] std::vector<DicondeIndication> porosityIndications(const PorosityResult& result,
                                                                 const RigidTransform& pose = {});

/// Component, inspection and equipment from an inspection order (examples/inspection_order.json):
/// part, customer, laboratory, report, order, scan and approval. With an evaluation, its result
/// per zone goes into the examination notes.
void applyInspectionOrder(const nlohmann::json& order, DicondeWriteOptions& options,
                          const Evaluation* evaluation = nullptr);

/// What a DICONDE (or DICOM) series says beyond its pixels.
struct DicondeInfo {
  DicondeComponent component;
  DicondeInspection inspection;
  DicondeEquipment equipment;
  std::string modality;
  std::string series_uid;
  std::string frame_of_reference_uid;
  std::string series_description;
  int series_number = 0;
  bool derived = false;
  std::string image_comments;
  /// The indications of all slices, by number.
  std::vector<DicondeIndication> indications;
};

/// Reads the metadata of the series of `source` from its files: everything but the indications
/// from the first slice, the indications from every slice.
[[nodiscard]] DicondeInfo readDicondeInfo(const DicomStackSource& source);

[[nodiscard]] nlohmann::json toJson(const DicondeInfo& info);

}  // namespace voxelsieve
