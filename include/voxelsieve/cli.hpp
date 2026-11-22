#pragma once

// What the command-line tools in apps/ share: walking their arguments, the options every tool
// takes (--memory, --cache, --threads, --telemetry, --help), the usage text, errors and the
// telemetry of a run.

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/telemetry.hpp"

namespace voxelsieve::cli {

/// A mistake on the command line: the tool prints it followed by its usage.
class UsageError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// The options every tool takes, at the end of each usage text.
inline constexpr std::string_view kCommonOptions =
    R"(  --memory <MB>           Memory budget of caches and work buffers (default 3/4 of RAM)
  --cache <MB>            Brick cache of all datasets (default a third of the memory budget)
  --threads <N>           Threads of all parallel work (default all cores)
  --telemetry <file>      Write time and resource use per phase as JSON
  --version               Show the version
  -h, --help              Show this help
)";
;

/// The arguments of a tool, walked one at a time:
///
///   cli::Arguments args(argc, argv);
///   while (args.next()) {
///     if (args.is("--out")) { options.out = args.value(); }
///     else if (args.is("--min-pore")) { options.min_pore = args.integer(); }
///     else if (args.positional()) { options.inputs.emplace_back(args.arg()); }
///     else { args.unknown(); }
///   }
///
/// The loop sees every argument but the options all tools take: --memory, --cache and --threads
/// are applied as they come (`applyResourceOption`), --telemetry is kept, and -h or --help
/// anywhere sets help() and ends the walk at once. Values that are missing or no numbers throw a
/// UsageError naming the option.
class Arguments {
 public:
  Arguments(int argc, char** argv);

  /// Moves to the next argument of the tool; false after the last one, or at once for --help.
  bool next();
  /// The current argument.
  [[nodiscard]] std::string_view arg() const { return args_[index_]; }
  [[nodiscard]] bool is(std::string_view name) const { return arg() == name; }
  /// Whether the current argument is no option (does not start with '-').
  [[nodiscard]] bool positional() const;
  /// The argument after the current one without moving to it; empty after the last.
  [[nodiscard]] std::string_view peek() const;
  /// The argument after the current option, its value.
  std::string value();
  /// The value as a number or a whole number.
  double number();
  std::int64_t integer();
  /// Throws a UsageError for the current argument, an option the tool does not take.
  [[noreturn]] void unknown() const;

  [[nodiscard]] bool help() const { return help_; }
  /// --version was given: like --help, it ends the walk at once.
  [[nodiscard]] bool version() const { return version_; }
  /// The file given with --telemetry, empty without.
  [[nodiscard]] const std::filesystem::path& telemetry() const { return telemetry_; }

 private:
  std::vector<std::string> args_;
  std::size_t index_ = 0;
  bool help_ = false;
  bool version_ = false;
  std::filesystem::path telemetry_;
};

/// The version of this VoxelSieve: of the release, or for a build from a checkout what git said
/// about it ("0.7.0-12-g21fd9a5").
[[nodiscard]] std::string_view versionText();

/// Prints the usage text of a tool followed by the common options.
void printUsage(std::ostream& out, std::string_view usage);

/// Prints the error that ended a tool: "<name>: <message>", and the usage after a UsageError.
void printError(std::string_view name, std::string_view usage, const std::exception& error);

/// The main function of a tool. `parse(Arguments&)` reads its options; `work(options)` runs it
/// and returns the exit code. --help prints the usage. The time and resources of the run are
/// reported on stderr, and written to the --telemetry file when given. An exception ends the tool
/// with exit code 1 and printError.
template <typename Parse, typename Work>
int runTool(std::string_view name, std::string_view usage, int argc, char** argv, Parse&& parse,
            Work&& work) {
  try {
    Arguments args(argc, argv);  // NOLINT(misc-const-correctness): parse walks it
    if (args.help()) {
      printUsage(std::cout, usage);
      return 0;
    }
    if (args.version()) {
      std::cout << name << ' ' << versionText() << '\n';
      return 0;
    }
    const auto options = parse(args);
    Telemetry telemetry{std::string(name)};
    int code = 0;
    {
      const TelemetryScope scope(telemetry);
      code = work(options);
    }
    reportTelemetry(telemetry, args.telemetry());
    return code;
  } catch (const std::exception& error) {
    printError(name, usage, error);
    return 1;
  }
}

}  // namespace voxelsieve::cli
