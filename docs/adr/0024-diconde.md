# 0024: DICONDE as input and output

Status: accepted (2026-10-11)

## Context

DICONDE (ASTM E2339) is the DICOM standard of nondestructive evaluation: scanners, viewers and
archives of industrial CT exchange volumes and findings with it. VoxelSieve already reads DICOM
stacks (ADR 0019) but wrote nothing other programs could open, and its results (pores, materials,
the evaluation of a test report) left the project only as JSON, PNG and VDB.

DICONDE files are DICOM files. A CT volume is a series of CT images, and what DICONDE adds is the
meaning of the metadata: the patient is the inspected component, the study the inspection, the
referring physician the owner of the part, and group 0014 (part of DICOM since 2011) holds the
manufacturer, the material and the indications, the findings of an evaluation.

## Decision

`include/voxelsieve/diconde.hpp`, with DCMTK (dcmdata) as before (ADR 0016), no new dependency.

- **Writing volumes** (`writeDicondeSeries`, `writeDicondeDataset`): one CT Image Storage file
  per slice, explicit VR little endian, UTF-8 text. Samples are unsigned with 16 bits allocated
  and 8 or 16 stored, as the source's sample type. Grey values stay exact (ADR 0002, 0021); the
  value mapping (ADR 0015) becomes the rescale slope and intercept, so a signed DICOM scan comes
  back with its stored values. Image Position and Orientation carry the pose of the object
  (ADR 0018), Pixel Spacing, Spacing Between Slices and Slice Thickness the voxel size
  (ADR 0012). Outside air the sieve removed gets the dataset's air level. Slices are read one at
  a time through `VolumeSource`, so XXL scans export in bounded memory (ADR 0004); datasets are
  opened with bricks loaded on access, as for slice views.
- **Metadata**: `DicondeComponent` (Component module: name, ID, other IDs such as the drawing,
  manufacturer, manufacturing date, material, notes), `DicondeInspection` (Component Study: study
  ID, accession number, date, company, owner, inspectors, examination notes) and
  `DicondeEquipment` (manufacturer, model, kV, µA, exposure, filter, detector, reconstruction).
  `applyInspectionOrder` fills them from the inspection order of the report (ADR 0007) and puts
  the evaluation per zone into the examination notes. Text is cut to the length of its value
  representation, never rejected.
- **Results**: pores and loosened zones are NDE indications (`porosityIndications`): type `PORE`
  or `LOOSENING`, a label, a description and physical properties (volume, size, equivalent
  diameter, porosity) as measured values with UCUM units. Each indication is written with the
  slice through its centre, its ROI the centre as a point referencing that slice. A material
  segmentation is a second series of labels (`writeDicondeMaterials`): image type DERIVED, 8
  bits stored, the label names in Image Comments, same study and frame of reference as the
  volume.
- **Reading**: DICONDE series are read by `DicomStackSource` like any DICOM stack;
  `readDicondeInfo` returns component, inspection, equipment and the indications of all slices.
  `import_dicom` logs the component and the number of indications and puts them in its summary.
- **Studio and CLI**: the operation `export_diconde` (inputs: dataset, optional porosity and
  materials, so the studio wires the object's current results in; parameters: inspection order,
  a directory to copy the series to) writes `diconde/volume/`, `diconde/materials/` and
  `diconde.json` with the UIDs. `vs-diconde` writes the same from the command line and prints a
  series with `--info`.

## Not mapped

- The nominal-actual comparison and the surface: DICONDE has no model of deviations on a surface;
  they stay PLY, STL and the report.
- The HTML test report itself. Its metadata and evaluation travel in the series; a DICOM
  Encapsulated PDF or Structured Report would carry the document and can follow.
- Segmentations as DICOM Segmentation objects (SEG): multi-frame, one bit plane per segment.
  Labels as a derived CT series open in every DICOM viewer and stay one byte per voxel.
- Multi-frame (enhanced CT) files are still refused on input (ADR 0019).

## Consequences

- Scans and their findings open in DICONDE viewers and archives, placed where the project put
  them, and come back into VoxelSieve with their grey values unchanged.
- Each export writes a full copy of the scan, one file per slice: for an XXL scan that is the
  size of the raw volume.
- The indications are readable only by programs that know the NDE Indication module; others see
  plain CT slices.
