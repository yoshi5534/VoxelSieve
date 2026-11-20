#include "voxelsieve/cli.hpp"

#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>

#include "voxelsieve/resources.hpp"

namespace voxelsieve::cli {

const std::string_view kCommonOptions =
    R"(  --memory <MB>           Memory budget of caches and work buffers (default 3/4 of RAM)
  --cache <MB>            Brick cache of all datasets (default a third of the memory budget)
  --threads <N>           Threads of all parallel work (default all cores)
  --telemetry <file>      Write time and resource use per phase as JSON
  -h, --help              Show this help
)";

Arguments::Arguments(int argc, char** argv) {
  args_.emplace_back(argc > 0 ? argv[0] : "");
  for (int i = 1; i < argc; ++i) {
    args_.emplace_back(argv[i]);
    help_ = help_ || args_.back() == "-h" || args_.back() == "--help";
  }
}

bool Arguments::next() {
  if (help_) {
    return false;
  }
  while (++index_ < args_.size()) {
    if (is("--telemetry")) {
      telemetry_ = value();
    } else if (!applyResourceOption(arg(), [this] { return value(); })) {
      return true;
    }
  }
  return false;
}

bool Arguments::positional() const { return !arg().starts_with("-"); }

std::string_view Arguments::peek() const {
  return index_ + 1 < args_.size() ? std::string_view(args_[index_ + 1]) : std::string_view();
}

std::string Arguments::value() {
  if (index_ + 1 >= args_.size()) {
    throw UsageError("Missing value for " + std::string(arg()));
  }
  return args_[++index_];
}

double Arguments::number() {
  const std::string name(arg());
  const std::string text = value();
  try {
    std::size_t used = 0;
    const double result = std::stod(text, &used);
    if (used == text.size()) {
      return result;
    }
  } catch (const std::logic_error&) {
    // reported below
  }
  throw UsageError(name + " needs a number, not '" + text + "'");
}

std::int64_t Arguments::integer() {
  const std::string name(arg());
  const std::string text = value();
  std::int64_t result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc() || end != text.data() + text.size()) {
    throw UsageError(name + " needs a whole number, not '" + text + "'");
  }
  return result;
}

void Arguments::unknown() const { throw UsageError("Unknown option: " + std::string(arg())); }

void printUsage(std::ostream& out, std::string_view usage) { out << usage << kCommonOptions; }

void printError(std::string_view name, std::string_view usage, const std::exception& error) {
  std::cerr << name << ": " << error.what() << '\n';
  if (dynamic_cast<const UsageError*>(&error) != nullptr) {
    std::cerr << '\n';
    printUsage(std::cerr, usage);
  }
}

}  // namespace voxelsieve::cli
