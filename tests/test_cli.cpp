#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "voxelsieve/cli.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/resources.hpp"

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
  for (const char* id : {"import_raw", "import_tiff", "import_dicom", "import_gzipped_xml"}) {
    const nlohmann::json& properties = registry.find(id)->info().parameters.at("properties");
    for (const auto& [name, parameter] : parameters.items()) {
      EXPECT_EQ(properties.at(name), parameter) << id << " " << name;
    }
  }
}

}  // namespace
}  // namespace voxelsieve
