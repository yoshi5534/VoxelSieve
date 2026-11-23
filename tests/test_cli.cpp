#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "voxelsieve/battery.hpp"
#include "voxelsieve/beam_hardening.hpp"
#include "voxelsieve/cli.hpp"
#include "voxelsieve/compare.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/denoise.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/model.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/resources.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/thickness.hpp"

namespace voxelsieve {
namespace {

/// Arguments from a list, as main() gets them.
class CommandLine {
 public:
  explicit CommandLine(std::vector<std::string> args) : args_(std::move(args)) {
    args_.insert(args_.begin(), "vs-test");
    for (std::string& arg : args_) {
      argv_.push_back(arg.data());
    }
  }
  cli::Arguments arguments() { return {static_cast<int>(argv_.size()), argv_.data()}; }

 private:
  std::vector<std::string> args_;
  std::vector<char*> argv_;
};

TEST(CliTest, WalksTheArgumentsOfTheToolAndKeepsTheCommonOptions) {
  const int threads = threadLimit();
  CommandLine line({"in.raw", "--out", "dir", "--threads", "2", "--min", "5", "--telemetry",
                    "t.json", "--ratio", "0.25", "second"});
  cli::Arguments args = line.arguments();
  std::vector<std::string> positional;
  std::string out;
  std::int64_t min = 0;
  double ratio = 0.0;
  while (args.next()) {
    if (args.is("--out")) {
      EXPECT_EQ(args.peek(), "dir");
      out = args.value();
    } else if (args.is("--min")) {
      min = args.integer();
    } else if (args.is("--ratio")) {
      ratio = args.number();
    } else if (args.positional()) {
      positional.emplace_back(args.arg());
    } else {
      args.unknown();
    }
  }
  EXPECT_FALSE(args.help());
  EXPECT_EQ(positional, (std::vector<std::string>{"in.raw", "second"}));
  EXPECT_EQ(out, "dir");
  EXPECT_EQ(min, 5);
  EXPECT_DOUBLE_EQ(ratio, 0.25);
  EXPECT_EQ(args.telemetry(), "t.json");
  EXPECT_EQ(threadLimit(), 2);
  setThreadLimit(threads);
}

TEST(CliTest, HelpAnywhereEndsTheWalk) {
  CommandLine line({"--out", "dir", "--bogus", "-h"});
  cli::Arguments args = line.arguments();
  EXPECT_TRUE(args.help());
  EXPECT_FALSE(args.next());
}

TEST(CliTest, MistakesAreUsageErrorsNamingTheOption) {
  const auto first = [](cli::Arguments& args) {
    EXPECT_TRUE(args.next());
    return &args;
  };
  CommandLine missing({"--out"});
  auto a = missing.arguments();
  EXPECT_THROW((void)first(a)->value(), cli::UsageError);
  CommandLine bad({"--min", "5x"});
  auto b = bad.arguments();
  try {
    (void)first(b)->integer();
    FAIL() << "no error";
  } catch (const cli::UsageError& error) {
    EXPECT_NE(std::string(error.what()).find("--min"), std::string::npos);
  }
  CommandLine unknown({"--what"});
  auto c = unknown.arguments();
  EXPECT_THROW(first(c)->unknown(), cli::UsageError);
}

TEST(CliTest, OptionsOfAnOperationTakeTheLimitsOfItsSchema) {
  const auto message = [](std::vector<std::string> args, auto read) -> std::string {
    CommandLine line(std::move(args));
    cli::Arguments arguments = line.arguments();
    EXPECT_TRUE(arguments.next());
    try {
      read(arguments);
    } catch (const cli::UsageError& error) {
      return error.what();
    }
    return "";
  };
  const auto min_pore = [](cli::Arguments& args) {
    EXPECT_EQ(args.integer("porosity", "min_pore_voxels"), 5);
  };
  EXPECT_EQ(message({"--min-pore", "5"}, min_pore), "");
  // The error names the option the user typed and the limit of the schema.
  const auto read_pore = [](cli::Arguments& args) {
    (void)args.integer("porosity", "min_pore_voxels");
  };
  EXPECT_EQ(message({"--min-pore", "0"}, read_pore), "--min-pore must be at least 1");
  EXPECT_EQ(message({"--min-pore", "-3"}, read_pore), "--min-pore must be at least 1");
  const auto fraction = [](cli::Arguments& args) {
    (void)args.number("porosity", "min_zone_void_fraction");
  };
  EXPECT_EQ(message({"--zone-min", "1.5"}, fraction), "--zone-min must be at most 1");
  EXPECT_EQ(message({"--zone-min", "0.2"}, fraction), "");
  // Limits that exclude the bound itself hold too.
  const auto strength = [](cli::Arguments& args) { (void)args.number("denoise", "strength"); };
  EXPECT_EQ(message({"--strength", "0"}, strength), "--strength must be greater than 0");
  EXPECT_EQ(message({"--strength", "0.5"}, strength), "");
  EXPECT_THROW((void)validateParameters(
                   {{"properties", {{"strength", builtinParameter("denoise", "strength")}}}},
                   {{"strength", 0}}),
               std::invalid_argument);
  // A name that does not exist is a mistake in the tool, not of the user.
  EXPECT_THROW((void)builtinParameter("porosity", "min_pore"), std::logic_error);
  EXPECT_THROW((void)builtinParameter("porosities", "min_pore_voxels"), std::logic_error);
}

TEST(CliTest, DefaultsOfTheOptionsAreTheDefaultsOfTheSchema) {
  // The tools start from the option structs, the GUI from the schema: both must agree.
  const auto expect = [](const char* operation, const char* parameter, double value) {
    const nlohmann::json schema = builtinParameter(operation, parameter);
    ASSERT_TRUE(schema.contains("default")) << operation << '.' << parameter;
    EXPECT_DOUBLE_EQ(schema.at("default").get<double>(), value) << operation << '.' << parameter;
  };
  const PorosityOptions porosity;
  expect("porosity", "min_pore_voxels", static_cast<double>(porosity.min_pore_voxels));
  expect("porosity", "zone_sigma", porosity.zone_sigma);
  expect("porosity", "min_zone_void_fraction", porosity.min_zone_void_fraction);
  const SegmentationOptions segmentation;
  expect("segment_materials", "materials", segmentation.materials);
  expect("segment_materials", "min_neighbours", segmentation.min_neighbours);
  expect("segment_materials", "grow_steps", segmentation.grow_steps);
  expect("segment_materials", "grow_fraction", segmentation.grow_fraction);
  const ModelSegmentationOptions model;
  expect("segment_model", "tile", static_cast<double>(model.tile));
  const DenoiseOptions denoise;
  expect("denoise", "strength", denoise.non_local_means.strength);
  expect("denoise", "search_radius", denoise.non_local_means.search_radius);
  expect("denoise", "similarity_limit", denoise.non_local_means.similarity_limit);
  expect("denoise", "patch_radius", denoise.non_local_means.patch_radius);
  expect("denoise", "tile", static_cast<double>(denoise.tile));
  const BeamHardeningOptions beam_hardening;
  expect("beam_hardening", "order", beam_hardening.order);
  expect("beam_hardening", "resolution", static_cast<double>(beam_hardening.resolution));
  const SurfaceOptions surface;
  expect("surface", "bits", surface.bits);
  expect("surface", "band_voxels", surface.band_voxels);
  const CompareOptions compare;
  expect("compare_cad", "tolerance_mm", compare.tolerance_mm);
  expect("compare_cad", "non_rigid_spacing_mm", compare.deformation.spacing_mm);
  expect("compare_cad", "non_rigid_stiffness", compare.deformation.stiffness);
  const WallThicknessOptions thickness;
  expect("wall_thickness", "min_wall_mm", thickness.min_wall_mm);
  expect("wall_thickness", "max_wall_mm", thickness.max_wall_mm);
  expect("wall_thickness", "max_thickness_mm", thickness.max_thickness_mm);
  expect("wall_thickness", "search_angle_deg", thickness.search_angle_deg);
  expect("wall_thickness", "max_triangles", static_cast<double>(thickness.max_triangles));
  const BatteryOptions battery;
  expect("battery_inspection", "angles", battery.angles);
  expect("battery_inspection", "sections", battery.sections);
  expect("battery_inspection", "overhang_sections", battery.overhang_sections);
  expect("battery_inspection", "average_voxels", battery.average_voxels);
  expect("battery_inspection", "min_particle_voxels",
         static_cast<double>(battery.min_particle_voxels));
  expect("battery_inspection", "max_end_deviation_mm", battery.max_end_deviation_mm);
  expect("battery_inspection", "min_void_voxels", static_cast<double>(battery.min_void_voxels));
}

TEST(CliTest, SieveParametersGiveTheDatasetOptions) {
  const nlohmann::json schema = {{"type", "object"}, {"properties", sieveParameters()}};
  const DatasetOptions defaults =
      sieveOptions(validateParameters(schema, nlohmann::json::object()));
  EXPECT_FALSE(defaults.threshold);
  EXPECT_EQ(defaults.margin_voxels, DatasetOptions{}.margin_voxels);
  EXPECT_EQ(defaults.brick_size, DatasetOptions{}.brick_size);
  EXPECT_TRUE(defaults.stage_slow_sources);
  const DatasetOptions options =
      sieveOptions(validateParameters(schema, {{"threshold", 9000},
                                               {"margin_voxels", 5},
                                               {"outside_air_axes", "xy"},
                                               {"value_type", "float"},
                                               {"staging", false}}));
  EXPECT_EQ(options.threshold, 9000.0F);
  EXPECT_EQ(options.margin_voxels, 5);
  EXPECT_EQ(options.outside_air_axes, parseAirAxes("xy"));
  EXPECT_EQ(options.value_type, ValueType::kFloat);
  EXPECT_FALSE(options.stage_slow_sources);
  EXPECT_THROW((void)validateParameters(schema, {{"margin_voxels", -1}}), std::invalid_argument);
}

TEST(CliTest, EveryImportTakesTheSieveParameters) {
  OperationRegistry registry;
  registerBuiltinOperations(registry);
  const nlohmann::json parameters = sieveParameters();
  for (const char* id : {"import_raw", "import_tiff", "import_dicom", "import_gzipped_xml",
                         "import_header_volume", "import_png"}) {
    const nlohmann::json& properties = registry.find(id)->info().parameters.at("properties");
    for (const auto& [name, parameter] : parameters.items()) {
      EXPECT_EQ(properties.at(name), parameter) << id << " " << name;
    }
  }
}

}  // namespace
}  // namespace voxelsieve
