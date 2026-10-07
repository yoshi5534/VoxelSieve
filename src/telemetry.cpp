#include "voxelsieve/telemetry.hpp"

#include <tbb/task_arena.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: psapi.h needs its types.
#include <psapi.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#else
#include <sys/resource.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace voxelsieve {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

[[maybe_unused]] constexpr double kKb = 1024.0;  // Linux reports kB
constexpr double kMb = 1024.0 * 1024.0;

/// Cumulative counters of the process at one moment.
struct Counters {
  double t_s = 0.0;  // since the recorder started
  double cpu_user_s = 0.0;
  double cpu_system_s = 0.0;
  std::int64_t major_faults = 0;
  std::int64_t minor_faults = 0;
  // Linux fills every field; macOS and Windows what they report (see readProcessCounters).
  // /proc/self/io: rchar/wchar count every read() and write() (page cache hits too, but not
  // accesses to mapped files); read_bytes/write_bytes what reached the storage device.
  double read_mb = 0.0;
  double write_mb = 0.0;
  double disk_read_mb = 0.0;
  double disk_write_mb = 0.0;
  // /proc/self/status: resident memory of the process's own pages (heap, stacks) and of mapped
  // files (a memory-mapped input, which the kernel may evict at any time), and the high-water mark.
  double rss_anon_mb = 0.0;
  double rss_file_mb = 0.0;
  double hwm_mb = 0.0;
};

#if defined(_WIN32)

double fileTimeSeconds(const FILETIME& time) {
  const auto ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
  return static_cast<double>(ticks) * 1e-7;  // 100 ns ticks
}

/// Windows has no split of resident memory into own and file pages without walking the working
/// set, so all of it counts as the process's own; page faults are not split into major and minor.
void readProcessCounters(Counters& c) {
  const HANDLE process = GetCurrentProcess();
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetProcessTimes(process, &created, &exited, &kernel, &user) != 0) {
    c.cpu_user_s = fileTimeSeconds(user);
    c.cpu_system_s = fileTimeSeconds(kernel);
  }
  PROCESS_MEMORY_COUNTERS memory{};
  if (GetProcessMemoryInfo(process, &memory, sizeof(memory)) != 0) {
    c.minor_faults = memory.PageFaultCount;
    c.rss_anon_mb = static_cast<double>(memory.WorkingSetSize) / kMb;
    c.hwm_mb = static_cast<double>(memory.PeakWorkingSetSize) / kMb;
  }
  IO_COUNTERS io{};
  if (GetProcessIoCounters(process, &io) != 0) {
    c.read_mb = static_cast<double>(io.ReadTransferCount) / kMb;
    c.write_mb = static_cast<double>(io.WriteTransferCount) / kMb;
  }
}

/// Total and available physical memory in MB.
std::pair<double, double> systemMemoryMb() {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status) == 0) {
    return {0.0, 0.0};
  }
  return {static_cast<double>(status.ullTotalPhys) / kMb,
          static_cast<double>(status.ullAvailPhys) / kMb};
}

#else

double timevalSeconds(const timeval& tv) {
  return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) * 1e-6;
}

void readUsage(Counters& c) {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    c.cpu_user_s = timevalSeconds(usage.ru_utime);
    c.cpu_system_s = timevalSeconds(usage.ru_stime);
    c.major_faults = usage.ru_majflt;
    c.minor_faults = usage.ru_minflt;
#if defined(__APPLE__)
    c.hwm_mb = static_cast<double>(usage.ru_maxrss) / kMb;  // bytes on macOS
#endif
  }
}

#endif

#if defined(__APPLE__)

/// macOS: the memory footprint counts as the process's own pages, the rest of the resident set
/// as mapped files; bytes read and written come from the storage device counters.
void readProcessCounters(Counters& c) {
  readUsage(c);
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
      KERN_SUCCESS) {
    const auto own = static_cast<double>(info.phys_footprint);
    const auto resident = static_cast<double>(info.resident_size);
    c.rss_anon_mb = own / kMb;
    c.rss_file_mb = std::max(0.0, resident - own) / kMb;
  }
  rusage_info_v4 io{};
  if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&io)) == 0) {
    c.disk_read_mb = static_cast<double>(io.ri_diskio_bytesread) / kMb;
    c.disk_write_mb = static_cast<double>(io.ri_diskio_byteswritten) / kMb;
    c.read_mb = c.disk_read_mb;
    c.write_mb = static_cast<double>(io.ri_logical_writes) / kMb;
  }
}

std::pair<double, double> systemMemoryMb() {
  std::uint64_t total = 0;
  std::size_t size = sizeof(total);
  if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) != 0) {
    return {0.0, 0.0};
  }
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  double available = 0.0;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm),
                        &count) == KERN_SUCCESS) {
    available = static_cast<double>(vm.free_count + vm.inactive_count + vm.purgeable_count) *
                static_cast<double>(sysconf(_SC_PAGESIZE));
  }
  return {static_cast<double>(total) / kMb, available / kMb};
}

#elif !defined(_WIN32)

/// Reads "key: value" lines, value in the unit of the file (kB in status and meminfo).
void readKeyValues(const char* file, const std::function<void(std::string_view, double)>& use) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    // Lines without a number (names, flags) are not needed.
    const char* text = line.c_str() + colon + 1;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end != text) {
      use(std::string_view(line).substr(0, colon), value);
    }
  }
}

void readProcessCounters(Counters& c) {
  readUsage(c);
  readKeyValues("/proc/self/io", [&c](std::string_view key, double value) {
    if (key == "rchar") {
      c.read_mb = value / kMb;
    } else if (key == "wchar") {
      c.write_mb = value / kMb;
    } else if (key == "read_bytes") {
      c.disk_read_mb = value / kMb;
    } else if (key == "write_bytes") {
      c.disk_write_mb = value / kMb;
    }
  });
  readKeyValues("/proc/self/status", [&c](std::string_view key, double value) {
    if (key == "RssAnon") {
      c.rss_anon_mb = value / kKb;
    } else if (key == "RssFile" || key == "RssShmem") {
      c.rss_file_mb += value / kKb;
    } else if (key == "VmHWM") {
      c.hwm_mb = value / kKb;
    }
  });
}

std::pair<double, double> systemMemoryMb() {
  double total = 0.0;
  double available = 0.0;
  readKeyValues("/proc/meminfo", [&](std::string_view key, double value) {
    if (key == "MemTotal") {
      total = value / kKb;
    } else if (key == "MemAvailable") {
      available = value / kKb;
    }
  });
  return {total, available};
}

#endif

Counters readCounters(Clock::time_point start) {
  Counters c;
  c.t_s = std::chrono::duration<double>(Clock::now() - start).count();
  readProcessCounters(c);
  return c;
}

/// Resets the high-water mark of resident memory to the current value (Linux 4.0 and later), so
/// each phase gets its own peak. Returns false where that is not possible.
bool resetPeakMemory() {
#if defined(__linux__)
  std::ofstream clear("/proc/self/clear_refs");
  clear << "5";
  clear.flush();
  return static_cast<bool>(clear);
#else
  return false;
#endif
}

double rounded(double value, int digits) {
  const double scale = std::pow(10.0, digits);
  return std::round(value * scale) / scale;
}

std::string nowUtc() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  std::array<char, 32> buffer{};
  (void)std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer.data();
}

struct PhaseRecord {
  std::string name;
  int depth = 0;
  Counters begin;
  Counters end;
  bool open = true;
  double peak_rss_mb = 0.0;
  double peak_anon_mb = 0.0;
  double peak_file_mb = 0.0;

  void fold(const Counters& c) {
    peak_rss_mb = std::max({peak_rss_mb, c.hwm_mb, c.rss_anon_mb + c.rss_file_mb});
    peak_anon_mb = std::max(peak_anon_mb, c.rss_anon_mb);
    peak_file_mb = std::max(peak_file_mb, c.rss_file_mb);
  }

  [[nodiscard]] Json toJson(int threads) const {
    const double wall = end.t_s - begin.t_s;
    const double cpu =
        (end.cpu_user_s - begin.cpu_user_s) + (end.cpu_system_s - begin.cpu_system_s);
    const double cores = wall > 0.0 ? cpu / wall : 0.0;
    Json json = {{"name", name},
                 {"depth", depth},
                 {"start_s", rounded(begin.t_s, 3)},
                 {"wall_s", rounded(wall, 3)},
                 {"cpu_s", rounded(cpu, 3)},
                 {"cpu_system_s", rounded(end.cpu_system_s - begin.cpu_system_s, 3)},
                 {"cores_used", rounded(cores, 2)},
                 {"cpu_utilization", rounded(threads > 0 ? cores / threads : 0.0, 3)},
                 {"peak_rss_mb", rounded(peak_rss_mb, 1)},
                 {"peak_anon_mb", rounded(peak_anon_mb, 1)},
                 {"peak_file_mb", rounded(peak_file_mb, 1)},
                 {"read_mb", rounded(end.read_mb - begin.read_mb, 1)},
                 {"write_mb", rounded(end.write_mb - begin.write_mb, 1)},
                 {"disk_read_mb", rounded(end.disk_read_mb - begin.disk_read_mb, 1)},
                 {"disk_write_mb", rounded(end.disk_write_mb - begin.disk_write_mb, 1)},
                 {"major_faults", end.major_faults - begin.major_faults},
                 {"minor_faults", end.minor_faults - begin.minor_faults}};
    return json;
  }
};

struct Sample {
  Counters counters;
  std::string phase;
};

std::string formatNumber(double value, int digits) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(digits) << value;
  return out.str();
}

std::string formatMb(double mb) {
  return mb >= 10.0 * 1024.0 ? formatNumber(mb / 1024.0, 1) + " GB" : formatNumber(mb, 0) + " MB";
}

/// Hints where a phase leaves time or resources on the table. Only phases that take long enough
/// to matter are looked at.
Json hintsFor(const Json& phases, const Json& total, const Json& system) {
  Json hints = Json::array();
  const int threads = system.value("threads", 1);
  const double memory = system.value("memory_total_mb", 0.0);
  const double total_wall = total.value("wall_s", 0.0);
  const auto look = [&](const Json& p) {
    const double wall = p.value("wall_s", 0.0);
    if (wall < 5.0 || (p.value("depth", 0) > 0 && wall < 0.1 * total_wall)) {
      return;
    }
    const std::string name = p.value("name", "");
    const double cores = p.value("cores_used", 0.0);
    if (threads > 1 && cores < 0.5 * threads) {
      const auto faults = p.value("major_faults", std::int64_t{0});
      const double disk = p.value("disk_read_mb", 0.0);
      std::string text = name + ": " + formatNumber(wall, 0) + " s with " + formatNumber(cores, 1) +
                         " of " + std::to_string(threads) + " cores busy; ";
      if (static_cast<double>(faults) / wall > 100.0 || disk / wall > 50.0) {
        text += std::to_string(faults) + " major page faults and " + formatMb(disk) +
                " read from disk: it waits for data that is not in the page cache";
      } else if (cores < 1.5) {
        text += "few page faults, so the work runs on one thread or waits on a lock";
      } else {
        text += "few page faults, so the work is split into too few parallel parts";
      }
      hints.push_back(text);
    }
    if (memory > 0.0 && p.value("peak_rss_mb", 0.0) > 0.8 * memory) {
      hints.push_back(name + ": peak memory " + formatMb(p.value("peak_rss_mb", 0.0)) + " of " +
                      formatMb(memory) + " installed (mapped file pages " +
                      formatMb(p.value("peak_file_mb", 0.0)) +
                      "); pages will be evicted and read again");
    }
  };
  if (phases.empty()) {
    look(total);
  }
  for (const Json& phase : phases) {
    look(phase);
  }
  return hints;
}

thread_local Telemetry* g_current = nullptr;

}  // namespace

struct Telemetry::Impl {
  std::string name;
  TelemetryOptions options;
  Clock::time_point start = Clock::now();
  std::string started = nowUtc();
  Json system;
  bool peak_per_phase = false;

  std::mutex mutex;
  PhaseRecord total;
  std::vector<PhaseRecord> phases;
  std::vector<std::size_t> open;  // indices into phases, innermost last
  std::vector<Sample> samples;
  double interval_s = 0.0;

  std::optional<Json> record;
  std::condition_variable_any wake;
  // Last, so it stops (and joins) before the members it uses are destroyed.
  std::jthread sampler;

  /// Folds the counters into every open phase; at a phase boundary also restarts the peak, so
  /// the next interval gets its own. Needs the lock.
  void boundary(const Counters& c) {
    total.fold(c);
    for (const std::size_t i : open) {
      phases[i].fold(c);
    }
    if (peak_per_phase) {
      resetPeakMemory();
    }
  }

  void addSample(const Counters& c) {
    samples.push_back({c, open.empty() ? std::string() : phases[open.back()].name});
    if (samples.size() > options.max_samples) {
      std::vector<Sample> thinned;
      thinned.reserve(samples.size() / 2 + 1);
      for (std::size_t i = 0; i < samples.size(); i += 2) {
        thinned.push_back(std::move(samples[i]));
      }
      samples = std::move(thinned);
      interval_s *= 2.0;
    }
  }

  void sample(const std::stop_token& stop) {
    std::unique_lock lock(mutex);
    while (true) {
      wake.wait_for(lock, stop, std::chrono::duration<double>(interval_s), [] { return false; });
      if (stop.stop_requested()) {
        break;
      }
      const Counters c = readCounters(start);
      total.fold(c);
      for (const std::size_t i : open) {
        phases[i].fold(c);
      }
      addSample(c);
    }
  }

  [[nodiscard]] Json timeline() const {
    Json t = Json::array();
    Json cores = Json::array();
    Json anon = Json::array();
    Json file = Json::array();
    Json disk_read = Json::array();
    Json disk_write = Json::array();
    Json faults = Json::array();
    Json phase = Json::array();
    for (std::size_t i = 1; i < samples.size(); ++i) {
      const Counters& a = samples[i - 1].counters;
      const Counters& b = samples[i].counters;
      const double dt = std::max(b.t_s - a.t_s, 1e-6);
      t.push_back(rounded(b.t_s, 2));
      cores.push_back(
          rounded(((b.cpu_user_s - a.cpu_user_s) + (b.cpu_system_s - a.cpu_system_s)) / dt, 2));
      anon.push_back(rounded(b.rss_anon_mb, 0));
      file.push_back(rounded(b.rss_file_mb, 0));
      disk_read.push_back(rounded((b.disk_read_mb - a.disk_read_mb) / dt, 1));
      disk_write.push_back(rounded((b.disk_write_mb - a.disk_write_mb) / dt, 1));
      faults.push_back(rounded(static_cast<double>(b.major_faults - a.major_faults) / dt, 0));
      phase.push_back(samples[i].phase);
    }
    return {{"interval_s", interval_s},
            {"t_s", t},
            {"cores_used", cores},
            {"rss_anon_mb", anon},
            {"rss_file_mb", file},
            {"disk_read_mb_s", disk_read},
            {"disk_write_mb_s", disk_write},
            {"major_faults_s", faults},
            {"phase", phase}};
  }
};

Telemetry::Telemetry(std::string name, TelemetryOptions options) : impl_(std::make_unique<Impl>()) {
  Impl& d = *impl_;
  d.name = std::move(name);
  d.options = options;
  d.options.max_samples = std::max<std::size_t>(d.options.max_samples, 2);
  d.interval_s = d.options.sample_interval_s;
  const auto [memory_total, memory_available] = systemMemoryMb();
  d.peak_per_phase = resetPeakMemory();
  d.system = {{"cores", std::thread::hardware_concurrency()},
              {"threads", tbb::this_task_arena::max_concurrency()},
              {"memory_total_mb", rounded(memory_total, 0)},
              {"memory_available_mb", rounded(memory_available, 0)},
              {"peak_per_phase", d.peak_per_phase}};
  d.total.name = "total";
  d.total.depth = -1;
  d.total.begin = readCounters(d.start);
  d.total.fold(d.total.begin);
  if (d.interval_s > 0.0) {
    d.samples.push_back({d.total.begin, ""});
    d.sampler = std::jthread([&d](const std::stop_token& stop) { d.sample(stop); });
  }
}

Telemetry::~Telemetry() {
  if (g_current == this) {
    g_current = nullptr;
  }
}

void Telemetry::beginPhase(const std::string& name) {
  Impl& d = *impl_;
  const std::scoped_lock lock(d.mutex);
  if (d.record) {
    return;
  }
  const Counters c = readCounters(d.start);
  d.boundary(c);
  PhaseRecord phase;
  phase.name = name;
  phase.depth = static_cast<int>(d.open.size());
  phase.begin = c;
  phase.fold(c);
  d.phases.push_back(std::move(phase));
  d.open.push_back(d.phases.size() - 1);
}

void Telemetry::endPhase() {
  Impl& d = *impl_;
  const std::scoped_lock lock(d.mutex);
  if (d.record || d.open.empty()) {
    return;
  }
  const Counters c = readCounters(d.start);
  d.boundary(c);
  PhaseRecord& phase = d.phases[d.open.back()];
  phase.end = c;
  phase.open = false;
  d.open.pop_back();
}

nlohmann::json Telemetry::finish() {
  Impl& d = *impl_;
  {
    const std::scoped_lock lock(d.mutex);
    if (d.record) {
      return *d.record;
    }
  }
  if (d.sampler.joinable()) {
    d.sampler.request_stop();
    d.sampler.join();
  }
  const std::scoped_lock lock(d.mutex);
  const Counters c = readCounters(d.start);
  d.boundary(c);
  while (!d.open.empty()) {
    d.phases[d.open.back()].end = c;
    d.phases[d.open.back()].open = false;
    d.open.pop_back();
  }
  d.total.end = c;
  if (d.interval_s > 0.0) {
    d.samples.push_back({c, ""});
  }
  const int threads = d.system.at("threads").get<int>();
  Json total = d.total.toJson(threads);
  total.erase("name");
  total.erase("depth");
  total.erase("start_s");
  Json phases = Json::array();
  for (const PhaseRecord& phase : d.phases) {
    phases.push_back(phase.toJson(threads));
  }
  Json record = {{"format", 1},        {"name", d.name}, {"started", d.started},
                 {"system", d.system}, {"total", total}, {"phases", phases}};
  Json named_total = total;
  named_total["name"] = "total";
  record["hints"] = hintsFor(phases, named_total, d.system);
  if (d.interval_s > 0.0) {
    record["timeline"] = d.timeline();
  }
  d.record = record;
  return record;
}

Telemetry* Telemetry::current() { return g_current; }

TelemetryScope::TelemetryScope(Telemetry& telemetry) : previous_(g_current) {
  g_current = &telemetry;
}

TelemetryScope::~TelemetryScope() { g_current = previous_; }

TelemetryPhase::TelemetryPhase(const std::string& name) : telemetry_(Telemetry::current()) {
  if (telemetry_ != nullptr) {
    telemetry_->beginPhase(name);
  }
}

TelemetryPhase::~TelemetryPhase() {
  if (telemetry_ != nullptr) {
    telemetry_->endPhase();
  }
}

nlohmann::json telemetrySummary(const nlohmann::json& record) {
  Json summary = record;
  summary.erase("timeline");
  return summary;
}

std::string formatTelemetry(const nlohmann::json& record) {
  constexpr int kLabelWidth = 26;
  constexpr auto kLabelChars = static_cast<std::size_t>(kLabelWidth);
  std::ostringstream out;
  const Json& system = record.at("system");
  const int threads = system.value("threads", 1);
  out << std::left << std::setw(kLabelWidth) << "telemetry" << std::right << std::setw(9)
      << "wall s" << std::setw(8) << "cores" << std::setw(11) << "peak" << std::setw(11)
      << "disk read" << std::setw(11) << "disk write" << std::setw(11) << "maj.faults" << "\n";
  const auto row = [&](const std::string& label, const Json& p) {
    std::string name = label;
    if (name.size() >= kLabelChars) {
      name = name.substr(0, kLabelChars - 2) + "~";
    }
    out << std::left << std::setw(kLabelWidth) << name << std::right << std::setw(9)
        << formatNumber(p.value("wall_s", 0.0), 1) << std::setw(8)
        << formatNumber(p.value("cores_used", 0.0), 1) << std::setw(11)
        << formatMb(p.value("peak_rss_mb", 0.0)) << std::setw(11)
        << formatMb(p.value("disk_read_mb", 0.0)) << std::setw(11)
        << formatMb(p.value("disk_write_mb", 0.0)) << std::setw(11)
        << p.value("major_faults", std::int64_t{0}) << "\n";
  };
  for (const Json& phase : record.value("phases", Json::array())) {
    row(std::string(2 * static_cast<std::size_t>(phase.value("depth", 0) + 1), ' ') +
            phase.value("name", ""),
        phase);
  }
  row("total", record.at("total"));
  out << std::string(kLabelChars, ' ') << "(" << threads << " threads, "
      << formatMb(system.value("memory_total_mb", 0.0)) << " memory"
      << (system.value("peak_per_phase", false) ? "" : "; peaks since process start") << ")\n";
  for (const Json& hint : record.value("hints", Json::array())) {
    out << std::left << std::setw(kLabelWidth) << "hint" << hint.get<std::string>() << "\n";
  }
  return out.str();
}

void reportTelemetry(Telemetry& telemetry, const std::filesystem::path& file) {
  const Json record = telemetry.finish();
  std::cerr << formatTelemetry(record);
  if (!file.empty()) {
    std::ofstream out(file);
    out << record.dump(1) << '\n';
    if (!out) {
      throw std::runtime_error("Cannot write " + file.string());
    }
  }
}

}  // namespace voxelsieve
