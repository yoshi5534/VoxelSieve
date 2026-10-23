#include <tbb/parallel_for.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "detail/png.hpp"
#include "detail/zip.hpp"
#include "voxelsieve/battery.hpp"

namespace voxelsieve {
namespace {

constexpr const char* kLongitudinalFolder = "axial_images";
constexpr const char* kCrossFolder = "radial_images";

/// An image of a cell in the source: where it is and what it shows.
struct ImageEntry {
  std::string cell;  // path of the cell folder in the source, '/' separated
  bool longitudinal = false;
  double position = 0.0;
  std::string path;  // in the source
  std::optional<detail::ZipArchive::Entry> zip_entry;
};

/// The position in a file name such as "65.4000015258789.png" or "-0.5.png".
std::optional<double> positionOf(const std::string& name) {
  if (name.size() < 5) {
    return std::nullopt;
  }
  std::string ext = name.substr(name.size() - 4);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (ext != ".png") {
    return std::nullopt;
  }
  const std::string stem = name.substr(0, name.size() - 4);
  char* end = nullptr;
  const double value = std::strtod(stem.c_str(), &end);
  if (stem.empty() || end != stem.c_str() + stem.size()) {
    return std::nullopt;
  }
  return value;
}

/// Parses "<cell>/<folder>/<position>.png" at the end of a '/' separated path.
std::optional<ImageEntry> parseEntry(const std::string& path) {
  const auto file_sep = path.rfind('/');
  if (file_sep == std::string::npos || file_sep == 0) {
    return std::nullopt;
  }
  const auto folder_sep = path.rfind('/', file_sep - 1);
  const std::string folder =
      path.substr(folder_sep == std::string::npos ? 0 : folder_sep + 1,
                  file_sep - (folder_sep == std::string::npos ? 0 : folder_sep + 1));
  const bool longitudinal = folder == kLongitudinalFolder;
  if (!longitudinal && folder != kCrossFolder) {
    return std::nullopt;
  }
  const auto position = positionOf(path.substr(file_sep + 1));
  if (!position) {
    return std::nullopt;
  }
  ImageEntry entry;
  entry.cell = folder_sep == std::string::npos ? std::string() : path.substr(0, folder_sep);
  entry.longitudinal = longitudinal;
  entry.position = *position;
  entry.path = path;
  return entry;
}

/// Opens the archive; checks its entries only the first time. Every cell of an archive opens it
/// again, and the check reads a little of the whole archive, which on a slow network share takes
/// longer than reading the cell's images.
std::unique_ptr<detail::ZipArchive> openArchive(const std::filesystem::path& source) {
  static std::mutex mutex;
  static std::set<std::tuple<std::string, std::filesystem::file_time_type, std::uintmax_t>> checked;
  const auto key =
      std::make_tuple(std::filesystem::absolute(source).string(),
                      std::filesystem::last_write_time(source), std::filesystem::file_size(source));
  bool known = false;
  {
    const std::lock_guard lock(mutex);
    known = checked.contains(key);
  }
  auto zip = std::make_unique<detail::ZipArchive>(source, !known);
  const std::lock_guard lock(mutex);
  checked.insert(key);
  return zip;
}

std::vector<ImageEntry> scan(const std::filesystem::path& source,
                             std::unique_ptr<detail::ZipArchive>& zip) {
  std::vector<ImageEntry> entries;
  if (std::filesystem::is_directory(source)) {
    for (const auto& file : std::filesystem::recursive_directory_iterator(source)) {
      if (file.is_regular_file()) {
        const std::string relative =
            std::filesystem::relative(file.path(), source).generic_string();
        if (auto entry = parseEntry(relative)) {
          entries.push_back(std::move(*entry));
        }
      }
    }
  } else {
    zip = openArchive(source);
    for (const detail::ZipArchive::Entry& e : zip->entries()) {
      if (auto entry = parseEntry(e.name)) {
        entry->zip_entry = e;
        entries.push_back(std::move(*entry));
      }
    }
  }
  return entries;
}

std::string lastComponent(const std::string& path) {
  const auto sep = path.rfind('/');
  return sep == std::string::npos ? path : path.substr(sep + 1);
}

/// Natural order: digit runs compare as numbers.
bool naturalLess(const std::string& a, const std::string& b) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    const auto da = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
    const auto db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
    if (da && db) {
      std::size_t ei = i;
      std::size_t ej = j;
      while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei])) != 0) {
        ++ei;
      }
      while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej])) != 0) {
        ++ej;
      }
      const std::string na = a.substr(i, ei - i);
      const std::string nb = b.substr(j, ej - j);
      if (na.size() != nb.size()) {
        return na.size() < nb.size();
      }
      if (na != nb) {
        return na < nb;
      }
      i = ei;
      j = ej;
    } else {
      if (a[i] != b[j]) {
        return a[i] < b[j];
      }
      ++i;
      ++j;
    }
  }
  return a.size() - i < b.size() - j;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot open " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

std::vector<std::string> listCellImages(const std::filesystem::path& source) {
  std::unique_ptr<detail::ZipArchive> zip;
  std::map<std::string, std::array<bool, 2>> cells;
  for (const ImageEntry& e : scan(source, zip)) {
    cells[e.cell][e.longitudinal ? 0 : 1] = true;
  }
  std::vector<std::string> names;
  std::map<std::string, int> counts;
  for (const auto& [cell, kinds] : cells) {
    if (kinds[0] && kinds[1]) {
      ++counts[lastComponent(cell)];
    }
  }
  for (const auto& [cell, kinds] : cells) {
    if (kinds[0] && kinds[1]) {
      // The folder's name where it is unique, else its path.
      names.push_back(counts[lastComponent(cell)] == 1 ? lastComponent(cell) : cell);
    }
  }
  std::sort(names.begin(), names.end(), naturalLess);
  return names;
}

CellImages readCellImages(const std::filesystem::path& source, const std::string& cell) {
  std::unique_ptr<detail::ZipArchive> zip;
  std::vector<ImageEntry> entries = scan(source, zip);
  std::vector<ImageEntry> mine;
  std::string folder;
  for (ImageEntry& e : entries) {
    if (e.cell == cell || lastComponent(e.cell) == cell) {
      if (!folder.empty() && folder != e.cell) {
        throw std::invalid_argument("More than one cell folder is named " + cell + " in " +
                                    source.string() + "; give its path");
      }
      folder = e.cell;
      mine.push_back(std::move(e));
    }
  }
  if (mine.empty()) {
    throw std::invalid_argument("No section images of cell " + cell + " in " + source.string());
  }
  std::sort(mine.begin(), mine.end(), [](const ImageEntry& a, const ImageEntry& b) {
    return a.longitudinal != b.longitudinal ? a.longitudinal : a.position < b.position;
  });
  std::vector<CellImage> images(mine.size());
  tbb::parallel_for(std::size_t{0}, mine.size(), [&](std::size_t i) {
    const ImageEntry& e = mine[i];
    const std::vector<std::uint8_t> bytes =
        e.zip_entry ? zip->read(*e.zip_entry) : readFile(source / e.path);
    detail::GreyImage grey;
    try {
      grey = detail::decodeGreyPng(bytes);
    } catch (const std::exception& error) {
      throw std::runtime_error(e.path + ": " + error.what());
    }
    images[i] = {e.position, grey.width, grey.height, std::move(grey.values)};
  });
  CellImages cell_images;
  cell_images.cell = lastComponent(folder.empty() ? cell : folder);
  for (std::size_t i = 0; i < mine.size(); ++i) {
    (mine[i].longitudinal ? cell_images.longitudinal : cell_images.cross)
        .push_back(std::move(images[i]));
  }
  if (cell_images.longitudinal.empty() || cell_images.cross.empty()) {
    throw std::invalid_argument("Cell " + cell + " needs longitudinal sections (" +
                                kLongitudinalFolder + ") and cross-sections (" + kCrossFolder +
                                ")");
  }
  return cell_images;
}

}  // namespace voxelsieve
