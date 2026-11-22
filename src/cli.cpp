#include "voxelsieve/cli.hpp"

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <system_error>

#include "voxelsieve/operation.hpp"
#include "voxelsieve/resources.hpp"

namespace voxelsieve::cli {

Arguments::Arguments(int argc, char** argv) {
  args_.emplace_back(argc > 0 ? argv[0] : "");
  for (int i = 1; i < argc; ++i) {
    args_.emplace_back(argv[i]);
    help_ = help_ || args_.back() == "-h" || args_.back() == "--help";
    version_ = version_ || args_.back() == "--version";
  }
}

std::string_view versionText() { return VOXELSIEVE_VERSION; }

bool Arguments::next() {
  if (help_ || version_) {
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

namespace {

// The limits come from the schema of the operation, the name in the message from the option.
void checkAgainstSchema(const std::string& option, const std::string& operation,
                        const std::string& parameter, const nlohmann::json& value) {
  nlohmann::json schema;
  schema["properties"][parameter] = builtinParameter(operation, parameter);
  nlohmann::json given;
  given[parameter] = value;
  try {
    (void)validateParameters(schema, given);
  } catch (const std::invalid_argument& error) {
    std::string message = error.what();
    const std::string subject = "Parameter '" + parameter + "'";
    if (message.starts_with(subject)) {
      message = option + message.substr(subject.size());
    }
    throw UsageError(message);
  }
}

}  // namespace

double Arguments::number(const std::string& operation, const std::string& parameter) {
  const std::string option(arg());
  const double result = number();
  checkAgainstSchema(option, operation, parameter, result);
  return result;
}

std::int64_t Arguments::integer(const std::string& operation, const std::string& parameter) {
  const std::string option(arg());
  const std::int64_t result = integer();
  checkAgainstSchema(option, operation, parameter, result);
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
