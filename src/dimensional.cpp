#include "voxelsieve/dimensional.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "detail/dimensional.hpp"
#include "voxelsieve/compare.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {

using namespace detail::dimensional;

namespace {

// ---------------------------------------------------------------------------------------------
// Characteristics

Vec directionOf(const GeometricElement& e) { return e.direction; }

double distanceValue(const GeometricElement& a, const GeometricElement& b, int along) {
  if (along >= 0) {
    return std::abs(b.point[static_cast<std::size_t>(along)] -
                    a.point[static_cast<std::size_t>(along)]);
  }
  if (a.type == ElementType::kPlane) {
    return std::abs(dot(a.direction, b.point - a.point));
  }
  if (b.type == ElementType::kPlane) {
    return std::abs(dot(b.direction, a.point - b.point));
  }
  if (a.type == ElementType::kCylinder) {
    return axisDistance(a, b.point);
  }
  if (b.type == ElementType::kCylinder) {
    return axisDistance(b, a.point);
  }
  return norm(b.point - a.point);
}

double angleValue(const GeometricElement& a, const GeometricElement& b) {
  const double angle =
      degrees(std::acos(std::clamp(dot(directionOf(a), directionOf(b)), -1.0, 1.0)));
  const bool axis_a = a.type == ElementType::kCylinder;
  const bool axis_b = b.type == ElementType::kCylinder;
  const double folded = std::min(angle, 180.0 - angle);  // an axis has no side
  if (axis_a && axis_b) {
    return folded;
  }
  if (axis_a || axis_b) {
    return 90.0 - folded;  // between an axis and a face, from the face's normal
  }
  return angle;
}

double spread(const std::vector<Vec>& points, const Vec& direction) {
  double lo = std::numeric_limits<double>::max();
  double hi = std::numeric_limits<double>::lowest();
  for (const Vec& p : points) {
    const double s = dot(p, direction);
    lo = std::min(lo, s);
    hi = std::max(hi, s);
  }
  return points.empty() ? 0.0 : hi - lo;
}

std::pair<Vec, Vec> axisEnds(const GeometricElement& cylinder) {
  const Vec half = (0.5 * cylinder.length) * cylinder.direction;
  return {cylinder.point - half, cylinder.point + half};
}

Vec perpendicularPart(const Vec& v, const Vec& to) { return v - dot(v, to) * to; }

double orientationZone(Kind kind, const GeometricElement& feature, const std::vector<Vec>& points,
                       const GeometricElement& datum) {
  const Vec r = normalized(datum.direction);
  const bool datum_plane = datum.type == ElementType::kPlane;
  // Planes perpendicular to the datum plane or parallel to the datum axis, turned like the
  // feature.
  const Vec across = normalized(perpendicularPart(feature.direction, r));
  if (feature.type == ElementType::kPlane) {
    if (kind == Kind::kParallelism) {
      return datum_plane ? spread(points, r) : spread(points, across);
    }
    return datum_plane ? spread(points, across) : spread(points, r);
  }
  const auto [e1, e2] = axisEnds(feature);
  const Vec s = e2 - e1;
  const bool along_r = (kind == Kind::kParallelism) != datum_plane;
  return along_r ? norm(perpendicularPart(s, r)) : std::abs(dot(s, r));
}

double positionZone(const GeometricElement& nominal, const GeometricElement& actual,
                    const std::vector<Vec>& points) {
  switch (nominal.type) {
    case ElementType::kCylinder: {
      const auto [e1, e2] = axisEnds(actual);
      return 2.0 * std::max(axisDistance(nominal, e1), axisDistance(nominal, e2));
    }
    case ElementType::kPlane: {
      double worst = 0.0;
      for (const Vec& p : points) {
        worst = std::max(worst, std::abs(dot(p - nominal.point, nominal.direction)));
      }
      return 2.0 * worst;
    }
    case ElementType::kSphere:
    case ElementType::kPoint:
      return 2.0 * norm(actual.point - nominal.point);
  }
  return 0.0;
}

double characteristicValue(const CharacteristicPlan& c,
                           const std::vector<const GeometricElement*>& elements,
                           const GeometricElement* datum) {
  const GeometricElement& a = *elements[0];
  switch (c.kind->kind) {
    case Kind::kDiameter:
      return 2.0 * a.radius;
    case Kind::kRadius:
      return a.radius;
    case Kind::kDistance:
      return distanceValue(a, *elements[1], c.axis);
    case Kind::kAngle:
      return angleValue(a, *elements[1]);
    case Kind::kCoordinate:
      return a.point[static_cast<std::size_t>(c.axis)];
    default:
      (void)datum;
      return 0.0;
  }
}

std::string unitOf(Kind kind) { return kind == Kind::kAngle ? "deg" : "mm"; }

}  // namespace

Measurement measure(const SurfaceMask& mask, const Mesh& cad, const RigidTransform& cad_to_scan,
                    const nlohmann::json& plan, const DimensionalOptions& options) {
  if (!plan.is_object()) {
    throw std::invalid_argument("The inspection plan must be a JSON object");
  }
  std::optional<TelemetryPhase> phase(std::in_place, "nominal elements");
  options.progress.check();
  const FeatureFinder finder(cad);
  const MeshDistance cad_distance(cad);
  std::vector<ElementPlan> elements;
  for (const Json& item : plan.value("elements", Json::array())) {
    elements.push_back(parseElement(item, finder));
    for (std::size_t i = 0; i + 1 < elements.size(); ++i) {
      if (elements[i].id == elements.back().id) {
        throw std::invalid_argument("Element '" + elements.back().id + "' is defined twice");
      }
    }
  }
  if (elements.empty()) {
    throw std::invalid_argument("The inspection plan has no elements");
  }
  std::vector<DatumPlan> datums;
  const Json alignment = plan.value("alignment", Json("best_fit"));
  if (alignment.is_object() && alignment.contains("datums")) {
    for (const Json& item : alignment.at("datums")) {
      DatumPlan datum;
      const Json id = item.is_object() ? item.value("element", Json()) : item;
      datum.element = findElement(elements, id, "Datum");
      if (item.is_object() && item.contains("directions")) {
        for (const Json& d : item.at("directions")) {
          if (d.is_string()) {
            Vec axis{};
            axis[static_cast<std::size_t>(axisIndex(d, "Datum " + jsonText(id)))] = 1.0;
            datum.directions.push_back(axis);
          } else {
            datum.directions.push_back(normalized(d.get<Vec>()));
          }
        }
      }
      datums.push_back(datum);
    }
  } else if (!(alignment.is_string() && alignment.get<std::string>() == "best_fit")) {
    throw std::invalid_argument(
        R"(alignment must be "best_fit" or {"datums": [primary, secondary, tertiary]})");
  }
  std::vector<CharacteristicPlan> characteristics;
  const Json items = plan.value("characteristics", Json::array());
  for (std::size_t i = 0; i < items.size(); ++i) {
    characteristics.push_back(parseCharacteristic(items[i], i, elements));
  }

  const VoxelSize& v = mask.info().voxel_size;
  Extraction x;
  x.mask = &mask;
  x.finder = &finder;
  x.cad = &cad_distance;
  x.options = options;
  x.voxel_mm = std::max({v[0], v[1], v[2]});

  RigidTransform correction;  // in the CAD (drawing) frame, after the best fit
  const RigidTransform best_fit = cad_to_scan.inverse();
  std::vector<ElementResult> results(elements.size());
  int fixed = 0;
  Json datum_json = Json::array();
  if (!datums.empty()) {
    phase.reset();
    phase.emplace("datum alignment");
    Vec origin{};
    for (const DatumPlan& d : datums) {
      origin = origin + elements[d.element].nominal.point;
    }
    origin = (1.0 / static_cast<double>(datums.size())) * origin;
    for (int pass = 0; pass < 3; ++pass) {
      const RigidTransform scan_to_frame = correction.after(best_fit);
      std::vector<GeometricElement> measured;
      for (const DatumPlan& d : datums) {
        ElementResult& r = results[d.element];
        r = extractAndFit(x, elements[d.element], scan_to_frame);
        if (!r.measured) {
          throw std::invalid_argument("Datum '" + elements[d.element].id +
                                      "' could not be measured: " + r.message);
        }
        measured.push_back(r.fit.element);
      }
      // Gauss-Newton on the small motion, the datums in order of precedence.
      RigidTransform step;
      for (int iteration = 0; iteration < 20; ++iteration) {
        std::vector<std::vector<Equation>> equations;
        for (std::size_t i = 0; i < datums.size(); ++i) {
          equations.push_back(datumEquations(elements[datums[i].element].nominal,
                                             moved(measured[i], step), datums[i], origin));
        }
        const auto [motion, dof] = solveDatums(equations);
        fixed = dof;
        step = smallMotion(motion, origin).after(step);
        double size = 0.0;
        for (const double m : motion) {
          size = std::max(size, std::abs(m));
        }
        if (size < 1e-12) {
          break;
        }
      }
      correction = step.after(correction);
    }
    for (const DatumPlan& d : datums) {
      Json entry = {{"element", elements[d.element].id}};
      if (!d.directions.empty()) {
        entry["directions"] = d.directions;
      }
      datum_json.push_back(entry);
    }
  }

  phase.reset();
  phase.emplace("elements");
  const RigidTransform scan_to_frame = correction.after(best_fit);
  for (std::size_t i = 0; i < elements.size(); ++i) {
    options.progress.report(static_cast<double>(i) / static_cast<double>(elements.size()));
    results[i] = extractAndFit(x, elements[i], scan_to_frame);
  }

  phase.reset();
  phase.emplace("characteristics");
  Measurement measurement;
  measurement.scan_to_cad = scan_to_frame;
  Json element_json = Json::array();
  for (std::size_t i = 0; i < elements.size(); ++i) {
    const ElementPlan& e = elements[i];
    const ElementResult& r = results[i];
    Json entry = {{"id", e.id},
                  {"type", toString(e.nominal.type)},
                  {"source", e.from_cad ? "cad" : "given"},
                  {"anchor", e.anchor},
                  {"nominal", toJson(e.nominal)},
                  {"measured", r.measured}};
    if (r.measured) {
      entry["actual"] = toJson(r.fit.element);
      entry["points"] = r.fit.points;
      entry["outliers"] = r.fit.outliers;
      entry["rms_mm"] = r.fit.rms_mm;
      entry["form_mm"] = r.fit.formMm();
    } else {
      entry["message"] = r.message;
    }
    element_json.push_back(entry);
    measurement.element_triangles.push_back(e.triangles);
  }

  Json characteristic_json = Json::array();
  for (const CharacteristicPlan& c : characteristics) {
    Json entry = {{"number", c.number},
                  {"label", c.label},
                  {"type", c.kind->name},
                  {"unit", unitOf(c.kind->kind)}};
    Json ids = Json::array();
    for (const std::size_t e : c.elements) {
      ids.push_back(elements[e].id);
    }
    entry["elements"] = ids;
    if (c.datum) {
      entry["datum"] = elements[*c.datum].id;
    }
    if (c.axis >= 0) {
      entry[c.kind->kind == Kind::kCoordinate ? "axis" : "along"] = std::string(1, "xyz"[c.axis]);
    }
    for (const char* key : {"drawing_zone", "method", "note"}) {
      if (c.spec.contains(key)) {
        entry[key] = c.spec.at(key);
      }
    }
    std::vector<const GeometricElement*> nominal;
    std::vector<const GeometricElement*> actual;
    std::string missing;
    for (const std::size_t e : c.elements) {
      nominal.push_back(&elements[e].nominal);
      actual.push_back(&results[e].fit.element);
      if (!results[e].measured) {
        missing = elements[e].id;
      }
    }
    if (c.datum && !results[*c.datum].measured) {
      missing = elements[*c.datum].id;
    }
    double nominal_value = c.kind->zone || c.kind->kind == Kind::kDeviation
                               ? 0.0
                               : characteristicValue(c, nominal, nullptr);
    if (c.nominal) {
      nominal_value = *c.nominal;
    }
    entry["nominal"] = nominal_value;
    if (c.upper) {
      entry["upper"] = *c.upper;
    }
    if (c.lower) {
      entry["lower"] = *c.lower;
    } else if (c.kind->zone) {
      entry["lower"] = 0.0;
    }
    if (!missing.empty()) {
      entry["result"] = "not measured";
      entry["passed"] = nullptr;
      entry["message"] = "element '" + missing + "' was not measured";
      ++measurement.not_measured;
      characteristic_json.push_back(entry);
      continue;
    }
    const std::size_t first = c.elements[0];
    const ElementResult& r = results[first];
    double value = 0.0;
    switch (c.kind->kind) {
      case Kind::kFlatness:
      case Kind::kCylindricity:
      case Kind::kSphericity:
      case Kind::kForm:
        value = r.fit.formMm();
        break;
      case Kind::kParallelism:
      case Kind::kPerpendicularity:
        value =
            orientationZone(c.kind->kind, r.fit.element, r.points, results[*c.datum].fit.element);
        break;
      case Kind::kPosition:
        value = positionZone(elements[first].nominal, r.fit.element, r.points);
        break;
      case Kind::kProfile: {
        double worst = 0.0;
        for (const double d : r.cad_deviation) {
          worst = std::max(worst, std::abs(d));
        }
        value = 2.0 * worst;
        break;
      }
      case Kind::kDeviation:
        value = dot(r.fit.element.point - elements[first].nominal.point,
                    elements[first].nominal.direction);
        break;
      default:
        value = characteristicValue(c, actual, nullptr);
        break;
    }
    const double deviation = value - nominal_value;
    entry["actual"] = value;
    entry["deviation"] = deviation;
    if (!c.upper && !c.lower) {
      entry["result"] = "no tolerance";
      entry["passed"] = nullptr;
      ++measurement.unrated;
    } else {
      const bool passed = (!c.upper || deviation <= *c.upper + 1e-12) &&
                          (!c.lower || deviation >= *c.lower - 1e-12);
      entry["passed"] = passed;
      entry["result"] = passed ? "OK" : "NOK";
      if (c.upper) {
        const double band = c.lower ? *c.upper - *c.lower : *c.upper;
        if (band > 0.0) {
          const double middle = c.lower ? 0.5 * (*c.upper + *c.lower) : 0.0;
          entry["utilization_percent"] = c.lower
                                             ? 100.0 * std::abs(deviation - middle) / (0.5 * band)
                                             : 100.0 * deviation / band;
        }
      }
      ++(passed ? measurement.passed : measurement.failed);
    }
    characteristic_json.push_back(entry);
  }
  measurement.characteristics = characteristics.size();

  Json alignment_json;
  if (datums.empty()) {
    alignment_json = {{"method", "best_fit"}};
  } else {
    alignment_json = {{"method", "datums"},
                      {"datums", datum_json},
                      {"fixed_degrees_of_freedom", fixed},
                      {"correction_rotation_deg", correction.angleDegrees()},
                      {"correction_translation_mm", correction.translation}};
  }
  alignment_json["scan_to_cad"] = scan_to_frame.matrix();
  measurement.json = {
      {"alignment", alignment_json},
      {"voxel_size_mm", v},
      {"search_mm", options.search_mm > 0.0 ? options.search_mm : 5.0 * x.voxel_mm},
      {"edge_margin_mm", options.edge_margin_mm > 0.0 ? options.edge_margin_mm : 2.0 * x.voxel_mm},
      {"elements", element_json},
      {"characteristics", characteristic_json},
      {"summary",
       {{"characteristics", measurement.characteristics},
        {"passed", measurement.passed},
        {"failed", measurement.failed},
        {"not_measured", measurement.not_measured},
        {"unrated", measurement.unrated},
        {"all_passed", measurement.allPassed()}}}};
  return measurement;
}

std::string characteristicsCsv(const nlohmann::json& measurement) {
  const auto field = [](const Json& value) -> std::string {
    if (value.is_null()) {
      return "";
    }
    if (value.is_number()) {
      std::array<char, 32> text{};
      (void)std::snprintf(text.data(), text.size(), "%.4f", value.get<double>());
      return text.data();
    }
    std::string s = value.is_string() ? value.get<std::string>() : value.dump();
    if (s.find_first_of(",\"\n") != std::string::npos) {
      std::string quoted = "\"";
      for (const char ch : s) {
        quoted += ch == '"' ? std::string("\"\"") : std::string(1, ch);
      }
      return quoted + "\"";
    }
    return s;
  };
  std::ostringstream out;
  out << "number,label,type,elements,datum,nominal,upper,lower,actual,deviation,unit,result\n";
  for (const Json& c : measurement.value("characteristics", Json::array())) {
    std::string ids;
    for (const Json& id : c.value("elements", Json::array())) {
      ids += (ids.empty() ? "" : " ") + id.get<std::string>();
    }
    out << field(c.value("number", Json())) << ',' << field(c.value("label", Json())) << ','
        << field(c.value("type", Json())) << ',' << field(ids) << ','
        << field(c.value("datum", Json())) << ',' << field(c.value("nominal", Json())) << ','
        << field(c.value("upper", Json())) << ',' << field(c.value("lower", Json())) << ','
        << field(c.value("actual", Json())) << ',' << field(c.value("deviation", Json())) << ','
        << field(c.value("unit", Json())) << ',' << field(c.value("result", Json())) << '\n';
  }
  return out.str();
}

void writeMeasurement(const Measurement& measurement, const Mesh& cad,
                      const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  writeJson(dir / "measurement.json", measurement.json);
  writeFile(dir / "characteristics.csv", characteristicsCsv(measurement.json));
  const IndexedMesh mesh = indexedMesh(cad);
  const Json& elements = measurement.json.at("elements");
  const Json& characteristics = measurement.json.at("characteristics");
  // Colour of an element: red when one of its characteristics failed, amber when one could not
  // be measured, green when all passed, blue without tolerances.
  std::map<std::string, int> state;  // 0 none, 1 unrated, 2 ok, 3 not measured, 4 failed
  for (const Json& c : characteristics) {
    const std::string result = c.value("result", "");
    const int s = result == "NOK" ? 4 : (result == "not measured" ? 3 : (result == "OK" ? 2 : 1));
    for (const Json& id : c.at("elements")) {
      int& current = state[id.get<std::string>()];
      current = std::max(current, s);
    }
  }
  const auto color = [&](int s) -> std::array<std::uint8_t, 3> {
    switch (s) {
      case 4:
        return {214, 39, 40};
      case 3:
        return {240, 168, 32};
      case 2:
        return {60, 170, 90};
      case 1:
        return {70, 130, 200};
      default:
        return {150, 155, 165};
    }
  };
  RenderScene scene;
  std::vector<std::array<std::uint8_t, 3>> colors(mesh.triangles.size(), {205, 208, 214});
  std::vector<std::uint32_t> groups(mesh.triangles.size(), 0);
  std::map<std::string, Vec> anchors;
  double size = 0.0;
  {
    const Bounds b = meshBounds(cad);
    size = norm(Vec{b.max[0] - b.min[0], b.max[1] - b.min[1], b.max[2] - b.min[2]});
  }
  for (std::size_t i = 0; i < elements.size(); ++i) {
    const std::string id = elements[i].at("id").get<std::string>();
    anchors[id] = elements[i].at("anchor").get<Vec>();
    const auto c = color(state.count(id) != 0 ? state.at(id) : 0);
    if (i < measurement.element_triangles.size() && !measurement.element_triangles[i].empty()) {
      for (const std::uint32_t t : measurement.element_triangles[i]) {
        if (t < mesh.triangles.size()) {
          colors[t] = c;
          groups[t] = static_cast<std::uint32_t>(i + 1);
        }
      }
    } else {
      scene.spheres.push_back({anchors[id], 0.008 * size, c});
    }
  }
  const IndexedMesh colored = splitByGroup(mesh, groups, colors, scene.vertex_colors);
  scene.mesh = &colored;
  // Element triangles by id, for anchors on the side that faces the camera.
  std::map<std::string, std::size_t> index_of;
  for (std::size_t i = 0; i < elements.size(); ++i) {
    index_of[elements[i].at("id").get<std::string>()] = i;
  }
  const auto anchor_facing = [&](const std::string& id, const Vec& towards) -> std::optional<Vec> {
    const std::size_t i = index_of.at(id);
    if (i >= measurement.element_triangles.size() || measurement.element_triangles[i].empty()) {
      return anchors.at(id);
    }
    // The middle of the element's triangles that face the camera, moved onto the nearest of
    // them.
    Vec sum{};
    double weight = 0.0;
    std::vector<std::pair<Vec, double>> facing;
    for (const std::uint32_t t : measurement.element_triangles[i]) {
      const auto& tri = mesh.triangles[t];
      const Vec a = toVec(mesh.points[tri[0]]);
      const Vec b = toVec(mesh.points[tri[1]]);
      const Vec c = toVec(mesh.points[tri[2]]);
      const Vec n = cross(b - a, c - a);
      const double area = 0.5 * norm(n);
      if (!(area > 0.0) || dot(normalized(n), towards) < 0.25) {
        continue;
      }
      const Vec middle = (1.0 / 3.0) * (a + b + c);
      sum = sum + area * middle;
      weight += area;
      facing.emplace_back(middle, area);
    }
    if (facing.empty()) {
      return std::nullopt;
    }
    // Inside a hole the middle of the far wall is hidden behind the near one; its rim is not.
    const Json& element = elements[i];
    const bool hole = element.at("nominal").value("inner", false);
    Vec target = (1.0 / weight) * sum;
    if (hole) {
      for (const auto& [p, area] : facing) {
        if (dot(p, towards) > dot(target, towards)) {
          target = p;
        }
      }
    }
    Vec best = facing.front().first;
    for (const auto& [p, area] : facing) {
      if (norm(p - target) < norm(best - target)) {
        best = p;
      }
    }
    return best;
  };
  std::vector<std::string> labels;
  std::vector<std::string> anchor_ids;
  for (const Json& c : characteristics) {
    anchor_ids.push_back(c.at("elements").back().get<std::string>());
    labels.push_back(c.at("number").get<std::string>());
  }
  // A balloon goes into the views where its element faces the camera, or into the first view
  // when it faces none.
  std::vector<std::vector<std::optional<Vec>>> view_anchors;
  for (const auto& direction : kDeviationViews) {
    const double azimuth = radians(direction[0]);
    const double elevation = radians(direction[1]);
    const Vec towards{std::cos(elevation) * std::cos(azimuth),
                      std::cos(elevation) * std::sin(azimuth), std::sin(elevation)};
    std::vector<std::optional<Vec>> found;
    found.reserve(anchor_ids.size());
    for (const std::string& id : anchor_ids) {
      found.push_back(anchor_facing(id, towards));
    }
    view_anchors.push_back(std::move(found));
  }
  for (std::size_t i = 0; i < anchor_ids.size(); ++i) {
    if (std::none_of(view_anchors.begin(), view_anchors.end(),
                     [i](const auto& v) { return v[i].has_value(); })) {
      view_anchors[0][i] = anchors.at(anchor_ids[i]);
    }
  }
  for (std::size_t k = 0; k < kDeviationViews.size(); ++k) {
    RenderView view;
    view.azimuth_degrees = kDeviationViews[k][0];
    view.elevation_degrees = kDeviationViews[k][1];
    view.width = 1200;
    view.height = 900;
    view.margin = 0.12;
    std::vector<Vec> balloon_anchors;
    std::vector<std::string> shown;
    for (std::size_t i = 0; i < labels.size(); ++i) {
      if (view_anchors[k][i]) {
        balloon_anchors.push_back(*view_anchors[k][i]);
        shown.push_back(labels[i]);
      }
    }
    RenderImage image = render(scene, view);
    const auto projected = projectToImage(scene, view, balloon_anchors);
    const auto placed = placeBalloons(projected, view.width, view.height, 24.0);
    for (std::size_t i = 0; i < shown.size(); ++i) {
      drawBalloon(image, placed[i][0], placed[i][1], projected[i][0], projected[i][1], shown[i], 3);
    }
    writePng(image, dir / ("measurement_view_" + std::to_string(k + 1) + ".png"));
  }
}

}  // namespace voxelsieve
