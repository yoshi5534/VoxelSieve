#include "voxelsieve/operation.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

bool hasType(const Json& value, const std::string& type) {
  if (type == "string") {
    return value.is_string();
  }
  if (type == "number") {
    return value.is_number();
  }
  if (type == "integer") {
    return value.is_number_integer() ||
           (value.is_number_float() && std::floor(value.get<double>()) == value.get<double>());
  }
  if (type == "boolean") {
    return value.is_boolean();
  }
  if (type == "array") {
    return value.is_array();
  }
  if (type == "object") {
    return value.is_object();
  }
  return true;
}

void checkValue(const std::string& name, const Json& schema, const Json& value) {
  if (const auto type = schema.find("type"); type != schema.end() && type->is_string()) {
    if (!hasType(value, type->get<std::string>())) {
      throw std::invalid_argument("Parameter '" + name + "' must be of type " +
                                  type->get<std::string>());
    }
  }
  if (const auto options = schema.find("enum"); options != schema.end()) {
    if (std::find(options->begin(), options->end(), value) == options->end()) {
      throw std::invalid_argument("Parameter '" + name + "' must be one of " + options->dump());
    }
  }
  if (value.is_number()) {
    if (const auto minimum = schema.find("minimum");
        minimum != schema.end() && value.get<double>() < minimum->get<double>()) {
      throw std::invalid_argument("Parameter '" + name + "' must be at least " + minimum->dump());
    }
    if (const auto maximum = schema.find("maximum");
        maximum != schema.end() && value.get<double>() > maximum->get<double>()) {
      throw std::invalid_argument("Parameter '" + name + "' must be at most " + maximum->dump());
    }
  }
  if (value.is_array()) {
    if (const auto items = schema.find("items"); items != schema.end()) {
      for (std::size_t i = 0; i < value.size(); ++i) {
        checkValue(name + "[" + std::to_string(i) + "]", *items, value[i]);
      }
    }
    if (const auto count = schema.find("minItems");
        count != schema.end() && value.size() < count->get<std::size_t>()) {
      throw std::invalid_argument("Parameter '" + name + "' needs at least " + count->dump() +
                                  " items");
    }
    if (const auto count = schema.find("maxItems");
        count != schema.end() && value.size() > count->get<std::size_t>()) {
      throw std::invalid_argument("Parameter '" + name + "' allows at most " + count->dump() +
                                  " items");
    }
  }
}

}  // namespace

Json validateParameters(const Json& schema, const Json& params) {
  if (!params.is_null() && !params.is_object()) {
    throw std::invalid_argument("Parameters must be a JSON object");
  }
  Json out = params.is_object() ? params : Json::object();
  const Json properties = schema.value("properties", Json::object());
  for (const auto& [name, value] : out.items()) {
    if (!properties.contains(name)) {
      throw std::invalid_argument("Unknown parameter '" + name + "'");
    }
    if (!value.is_null()) {
      checkValue(name, properties.at(name), value);
    }
  }
  for (const auto& [name, property] : properties.items()) {
    if ((!out.contains(name) || out.at(name).is_null()) && property.contains("default")) {
      out[name] = property.at("default");
    }
  }
  for (const Json& name : schema.value("required", Json::array())) {
    const auto key = name.get<std::string>();
    if (!out.contains(key) || out.at(key).is_null()) {
      throw std::invalid_argument("Missing parameter '" + key + "'");
    }
  }
  return out;
}

void OperationRegistry::add(std::shared_ptr<const Operation> operation) {
  if (!operation) {
    throw std::invalid_argument("Cannot register a null operation");
  }
  const std::string id = operation->info().id;
  if (id.empty()) {
    throw std::invalid_argument("Operation id must not be empty");
  }
  operations_[id] = std::move(operation);
}

std::shared_ptr<const Operation> OperationRegistry::find(const std::string& id) const {
  const auto it = operations_.find(id);
  return it == operations_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<const Operation>> OperationRegistry::all() const {
  std::vector<std::shared_ptr<const Operation>> out;
  out.reserve(operations_.size());
  for (const auto& [id, operation] : operations_) {
    out.push_back(operation);
  }
  return out;
}

void OperationRegistry::loadPlugin(const std::filesystem::path& library) {
  void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* error = dlerror();
    throw std::runtime_error("Cannot load " + library.string() + ": " +
                             (error != nullptr ? error : "unknown error"));
  }
  std::shared_ptr<void> guard(handle, [](void* h) { dlclose(h); });
  using VersionFn = int (*)();
  using RegisterFn = void (*)(OperationRegistry&);
  // dlsym returns object pointers; converting them to function pointers is how POSIX works.
  auto* version = reinterpret_cast<VersionFn>(dlsym(handle, "voxelsieve_plugin_api_version"));
  auto* register_operations =
      reinterpret_cast<RegisterFn>(dlsym(handle, "voxelsieve_register_operations"));
  if (version == nullptr || register_operations == nullptr) {
    throw std::runtime_error(library.string() + " is not a VoxelSieve plugin");
  }
  if (const int api = version(); api != kPluginApiVersion) {
    throw std::runtime_error(library.string() + " uses plugin API " + std::to_string(api) +
                             ", this VoxelSieve needs " + std::to_string(kPluginApiVersion));
  }
  register_operations(*this);
  libraries_.push_back(std::move(guard));
}

std::vector<std::string> OperationRegistry::loadPlugins(const std::filesystem::path& dir) {
  std::vector<std::string> messages;
  if (!std::filesystem::is_directory(dir)) {
    messages.push_back(dir.string() + ": not a directory");
    return messages;
  }
  std::vector<std::filesystem::path> libraries;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".so") {
      libraries.push_back(entry.path());
    }
  }
  std::sort(libraries.begin(), libraries.end());
  for (const auto& library : libraries) {
    try {
      loadPlugin(library);
      messages.push_back(library.filename().string() + ": loaded");
    } catch (const std::exception& error) {
      messages.push_back(library.filename().string() + ": " + error.what());
    }
  }
  return messages;
}

}  // namespace voxelsieve
