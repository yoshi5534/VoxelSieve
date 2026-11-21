#include "voxelsieve/cli.hpp"

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <system_error>

#include "voxelsieve/resources.hpp"

namespace voxelsieve::cli {

Arguments::Arguments(int argc, char** argv) {
  returnFreedMemoryEagerly();
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
  char* end = nullptr;
  const double result = std::strtod(text.c_str(), &end);
  if (text.empty() || end != text.c_str() + text.size()) {
    throw UsageError(name + " needs a number, not '" + text + "'");
  }
  return result;
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
