// The inspection plan of a dimensional measurement.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "detail/dimensional.hpp"
#include "detail/keys.hpp"

namespace voxelsieve::detail::dimensional {
namespace {

constexpr std::array<KindInfo, 14> kKinds{{
    {"diameter", Kind::kDiameter, 1, false, "Diameter"},
    {"radius", Kind::kRadius, 1, false, "Radius"},
    {"distance", Kind::kDistance, 2, false, "Distance"},
    {"angle", Kind::kAngle, 2, false, "Angle"},
    {"coordinate", Kind::kCoordinate, 1, false, "Coordinate"},
    {"position", Kind::kPosition, 1, true, "Position"},
    {"flatness", Kind::kFlatness, 1, true, "Flatness"},
    {"cylindricity", Kind::kCylindricity, 1, true, "Cylindricity"},
    {"sphericity", Kind::kSphericity, 1, true, "Sphericity"},
    {"form", Kind::kForm, 1, true, "Form"},
    {"parallelism", Kind::kParallelism, 1, true, "Parallelism"},
    {"perpendicularity", Kind::kPerpendicularity, 1, true, "Perpendicularity"},
    {"profile", Kind::kProfile, 1, true, "Surface profile"},
    {"deviation", Kind::kDeviation, 1, false, "Surface point deviation"},
}};

const KindInfo& kindInfo(const std::string& name) {
  for (const KindInfo& info : kKinds) {
    if (name == info.name) {
      return info;
    }
  }
  std::string known;
  for (const KindInfo& info : kKinds) {
    known += (known.empty() ? "" : ", ") + std::string(info.name);
  }
  throw std::invalid_argument("Unknown characteristic type '" + name + "' (" + known + ")");
}

Vec readVec(const Json& json, const std::string& key, const std::string& what) {
  if (!json.contains(key)) {
    throw std::invalid_argument(what + ": missing '" + key + "'");
  }
  const Json& value = json.at(key);
  if (!value.is_array() || value.size() != 3 ||
      !std::all_of(value.begin(), value.end(), [](const Json& c) { return c.is_number(); })) {
    throw std::invalid_argument(what + ": '" + key + "' must be [x, y, z]");
  }
  return value.get<Vec>();
}

double readPositive(const Json& json, const std::string& key, const std::string& what) {
  if (!json.contains(key) || !json.at(key).is_number() || !(json.at(key).get<double>() > 0.0)) {
    throw std::invalid_argument(what + ": '" + key + "' must be a positive number");
  }
  return json.at(key).get<double>();
}

}  // namespace

int axisIndex(const Json& value, const std::string& what) {
  if (value.is_string()) {
    const std::string axis = value.get<std::string>();
    if (axis == "x") {
      return 0;
    }
    if (axis == "y") {
      return 1;
    }
    if (axis == "z") {
      return 2;
    }
  }
  throw std::invalid_argument(what + R"(: axis must be "x", "y" or "z")");
}

std::string jsonText(const Json& value) {
  return value.is_string() ? value.get<std::string>() : value.dump();
}

ElementPlan parseElement(const Json& item, const FeatureFinder& finder) {
  if (!item.is_object() || !item.contains("id") || !item.contains("type")) {
    throw std::invalid_argument("Every element needs an id and a type");
  }
  ElementPlan e;
  e.id = jsonText(item.at("id"));
  const std::string what = "Element '" + e.id + "'";
  requireKnownKeys(item,
                   {"id", "type", "at", "search_mm", "edge_margin_mm", "radius_mm", "inner",
                    "point", "normal", "direction", "diameter", "length", "center"},
                   "element '" + e.id + "' of the inspection plan");
  const ElementType type = elementTypeFromString(item.at("type").get<std::string>());
  e.search_mm = item.value("search_mm", 0.0);
  e.margin_mm = item.value("edge_margin_mm", 0.0);
  e.radius_mm = item.value("radius_mm", 0.0);
  if (item.contains("at")) {
    const Vec at = readVec(item, "at", what);
    const std::optional<CadFeature> found = finder.probe(type, at);
    if (!found) {
      throw std::invalid_argument(what + ": no " + toString(type) + " of the CAD model at (" +
                                  std::to_string(at[0]) + ", " + std::to_string(at[1]) + ", " +
                                  std::to_string(at[2]) +
                                  "); list the features of the model with cad_features");
    }
    e.nominal = found->element;
    e.triangles = found->triangles;
    e.anchor = found->at;
    e.from_cad = true;
    if (item.contains("inner") && type != ElementType::kPlane && type != ElementType::kPoint &&
        item.at("inner").get<bool>() != e.nominal.inner) {
      throw std::invalid_argument(what + ": the CAD model has a " +
                                  (e.nominal.inner ? "hole" : "boss") + " there");
    }
    return e;
  }
  e.nominal.type = type;
  switch (type) {
    case ElementType::kPlane:
      e.nominal.point = readVec(item, "point", what);
      e.nominal.direction = normalized(readVec(item, "normal", what));
      e.radius_mm = readPositive(item, "radius_mm", what);
      break;
    case ElementType::kCylinder:
      e.nominal.point = readVec(item, "point", what);
      e.nominal.direction = normalized(readVec(item, "direction", what));
      e.nominal.radius = 0.5 * readPositive(item, "diameter", what);
      e.nominal.length = readPositive(item, "length", what);
      e.nominal.inner = item.value("inner", true);
      break;
    case ElementType::kSphere:
      e.nominal.point = readVec(item, "center", what);
      e.nominal.radius = 0.5 * readPositive(item, "diameter", what);
      e.nominal.inner = item.value("inner", false);
      break;
    case ElementType::kPoint:
      e.nominal.point = readVec(item, "point", what);
      e.nominal.direction = normalized(readVec(item, "normal", what));
      break;
  }
  e.anchor = type == ElementType::kCylinder || type == ElementType::kSphere
                 ? e.nominal.point + e.nominal.radius * basis(e.nominal.direction).first
                 : e.nominal.point;
  return e;
}

std::size_t findElement(const std::vector<ElementPlan>& elements, const Json& id,
                        const std::string& what) {
  const std::string name = jsonText(id);
  for (std::size_t i = 0; i < elements.size(); ++i) {
    if (elements[i].id == name) {
      return i;
    }
  }
  throw std::invalid_argument(what + ": no element '" + name + "'");
}

CharacteristicPlan parseCharacteristic(const Json& item, std::size_t index,
                                       const std::vector<ElementPlan>& elements) {
  if (!item.is_object() || !item.contains("type")) {
    throw std::invalid_argument("Characteristic " + std::to_string(index + 1) + " needs a type");
  }
  CharacteristicPlan c;
  c.spec = item;
  c.number = item.contains("number") ? jsonText(item.at("number")) : std::to_string(index + 1);
  c.kind = &kindInfo(item.at("type").get<std::string>());
  const std::string what = "Characteristic " + c.number;
  requireKnownKeys(item,
                   {"number", "type", "elements", "element", "datum", "axis", "along", "nominal",
                    "upper", "lower", "tolerance", "label", "drawing_zone", "method", "note"},
                   "characteristic " + c.number + " of the inspection plan");
  if (item.contains("elements")) {
    for (const Json& id : item.at("elements")) {
      c.elements.push_back(findElement(elements, id, what));
    }
  } else if (item.contains("element")) {
    c.elements.push_back(findElement(elements, item.at("element"), what));
  }
  if (c.elements.size() != static_cast<std::size_t>(c.kind->elements)) {
    throw std::invalid_argument(what + " (" + c.kind->name + ") relates " +
                                std::to_string(c.kind->elements) + " element(s): give " +
                                (c.kind->elements == 1 ? "\"element\"" : "\"elements\": [a, b]"));
  }
  const auto type_of = [&](std::size_t k) { return elements[c.elements[k]].nominal.type; };
  const auto require = [&](bool ok, const std::string& message) {
    if (!ok) {
      throw std::invalid_argument(what + " (" + c.kind->name + "): " + message);
    }
  };
  switch (c.kind->kind) {
    case Kind::kDiameter:
    case Kind::kRadius:
      require(type_of(0) == ElementType::kCylinder || type_of(0) == ElementType::kSphere,
              "needs a cylinder or sphere");
      break;
    case Kind::kFlatness:
      require(type_of(0) == ElementType::kPlane, "needs a plane");
      break;
    case Kind::kCylindricity:
      require(type_of(0) == ElementType::kCylinder, "needs a cylinder");
      break;
    case Kind::kSphericity:
      require(type_of(0) == ElementType::kSphere, "needs a sphere");
      break;
    case Kind::kForm:
      require(type_of(0) != ElementType::kPoint, "needs a plane, cylinder or sphere");
      break;
    case Kind::kDeviation:
      require(type_of(0) == ElementType::kPoint, "needs a point");
      break;
    case Kind::kAngle:
      require(type_of(0) != ElementType::kSphere && type_of(1) != ElementType::kSphere,
              "needs planes, cylinders or points");
      break;
    case Kind::kParallelism:
    case Kind::kPerpendicularity: {
      require(type_of(0) == ElementType::kPlane || type_of(0) == ElementType::kCylinder,
              "needs a plane or cylinder");
      require(item.contains("datum"), "needs a datum");
      c.datum = findElement(elements, item.at("datum"), what);
      const ElementType d = elements[*c.datum].nominal.type;
      require(d == ElementType::kPlane || d == ElementType::kCylinder,
              "the datum must be a plane or cylinder");
      break;
    }
    case Kind::kCoordinate:
      require(item.contains("axis"), "needs an axis");
      c.axis = axisIndex(item.at("axis"), what);
      break;
    case Kind::kDistance:
      if (item.contains("along")) {
        c.axis = axisIndex(item.at("along"), what);
      }
      break;
    case Kind::kPosition:
    case Kind::kProfile:
      break;
  }
  if (item.contains("nominal")) {
    c.nominal = item.at("nominal").get<double>();
  }
  if (item.contains("upper")) {
    c.upper = item.at("upper").get<double>();
  }
  if (item.contains("lower")) {
    c.lower = item.at("lower").get<double>();
  }
  if (item.contains("tolerance")) {
    const double t = item.at("tolerance").get<double>();
    require(t >= 0.0, "the tolerance must not be negative");
    if (!c.upper) {
      c.upper = t;
    }
    if (!c.lower && !c.kind->zone) {
      c.lower = -t;
    }
  }
  if (c.upper && c.lower) {
    require(*c.lower <= *c.upper, "the lower deviation lies above the upper one");
  }
  std::string label = item.value("label", std::string());
  if (label.empty()) {
    label = c.kind->label;
    if (c.axis >= 0) {
      label += std::string(" ") + "xyz"[c.axis];
    }
    for (const std::size_t e : c.elements) {
      label += " " + elements[e].id;
    }
    if (c.datum) {
      label += " to " + elements[*c.datum].id;
    }
  }
  c.label = label;
  return c;
}

}  // namespace voxelsieve::detail::dimensional
