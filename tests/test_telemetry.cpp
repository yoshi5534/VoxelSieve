#include <gtest/gtest.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#if defined(__SANITIZE_THREAD__)
#define VOXELSIEVE_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define VOXELSIEVE_TEST_TSAN 1
#endif
#endif

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "voxelsieve/gui.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/project.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/synthetic.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

/// CPU time of the calling thread so far. Not the process: other threads of the test process
/// (TBB workers winding down) would end `busy` before the thread has spent its time.
double cpuSeconds() {
#ifdef _WIN32
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
  const auto ticks = [](const FILETIME& t) {
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32U) | t.dwLowDateTime;
  };
  return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7;  // 100 ns units
#else
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
#endif
}

/// Spends `seconds` of CPU time on the calling thread and returns a value the compiler cannot drop.
double busy(double seconds) {
  const double end = cpuSeconds() + seconds;
  double x = 0.0;
  while (cpuSeconds() < end) {
    for (int i = 0; i < 10000; ++i) {
      x += 1e-9 * i;
    }
  }
  return x;
}

Json phase(const Json& record, const std::string& name) {
  for (const Json& p : record.at("phases")) {
    if (p.at("name") == name) {
      return p;
    }
  }
  throw std::runtime_error("No phase " + name);
}

TEST(TelemetryTest, PhasesRecordTimeCpuMemoryAndIo) {
  const auto file = std::filesystem::temp_directory_path() / "voxelsieve_telemetry_io.bin";
  constexpr std::size_t kMemoryBytes = std::size_t{96} << 20U;
  Telemetry telemetry("test", {.sample_interval_s = 0.0});
  {
    const TelemetryScope scope(telemetry);
    {
      const TelemetryPhase outer("compute");
      EXPECT_GT(busy(0.2), 0.0);
      const TelemetryPhase inner("inner");
      EXPECT_GT(busy(0.1), 0.0);
    }
    {
      const TelemetryPhase memory("memory");
      // Mapped directly, so it is returned on unmap (malloc and ASan's quarantine would keep it).
#ifdef _WIN32
      void* block = VirtualAlloc(nullptr, kMemoryBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      ASSERT_NE(block, nullptr);
      std::memset(block, 1, kMemoryBytes);  // touched, so resident
      // Held for a few peak samples: not every system has a high-water mark per phase.
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      VirtualFree(block, 0, MEM_RELEASE);
#else
      void* block =
          mmap(nullptr, kMemoryBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      ASSERT_NE(block, MAP_FAILED);
      std::memset(block, 1, kMemoryBytes);  // touched, so resident
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      munmap(block, kMemoryBytes);
#endif
    }
    {
      const TelemetryPhase write("write");
      const std::vector<char> data(std::size_t{8} << 20U, 'x');
      std::ofstream(file, std::ios::binary)
          .write(data.data(), static_cast<std::streamsize>(data.size()));
    }
  }
  const Json record = telemetry.finish();
  std::filesystem::remove(file);

  EXPECT_EQ(record.at("name"), "test");
  EXPECT_EQ(record.at("format"), 1);
  EXPECT_FALSE(record.contains("timeline"));
  ASSERT_EQ(record.at("phases").size(), 4U);
  const Json compute = phase(record, "compute");
  const Json inner = phase(record, "inner");
  EXPECT_EQ(compute.at("depth"), 0);
  EXPECT_EQ(inner.at("depth"), 1);
  // Windows counts CPU time in scheduler ticks of about 15.6 ms; allow for one and for rounding.
  EXPECT_GE(compute.at("wall_s").get<double>(), 0.28);
  EXPECT_GE(inner.at("wall_s").get<double>(), 0.08);
  EXPECT_LT(inner.at("wall_s").get<double>(), compute.at("wall_s").get<double>());
  EXPECT_GE(compute.at("cpu_s").get<double>(), 0.28);
  EXPECT_GT(compute.at("cores_used").get<double>(), 0.0);

  const double memory_peak = phase(record, "memory").at("peak_rss_mb").get<double>();
  // Windows may trim a few pages of the 96 MiB block from the working set while it is written.
  EXPECT_GE(memory_peak, phase(record, "compute").at("peak_rss_mb").get<double>() + 80.0);
  EXPECT_GE(record.at("total").at("peak_rss_mb").get<double>(), memory_peak);
  // The block is the process's own memory, not a mapped file.
  EXPECT_GE(phase(record, "memory").at("peak_anon_mb").get<double>(),
            phase(record, "compute").at("peak_anon_mb").get<double>() + 80.0);
  // Peaks are those of each phase on every system, so the later write phase does not see the
  // block (through a high-water mark per phase, or sampled).
  EXPECT_LT(phase(record, "write").at("peak_anon_mb").get<double>(),
            phase(record, "memory").at("peak_anon_mb").get<double>() - 50.0);
#ifdef _WIN32
  // The library replaces the C runtime's heap with TBB's scalable allocator (src/CMakeLists.txt).
  EXPECT_EQ(record.at("system").at("allocator"), "tbbmalloc");
#else
  EXPECT_EQ(record.at("system").at("allocator"), "system");
#endif
  if (record.at("system").at("peak_per_phase").get<bool>()) {
    // The high-water mark restarts at each phase, so the write phase does not see the block.
    EXPECT_LT(phase(record, "write").at("peak_rss_mb").get<double>(), memory_peak - 50.0);
  }
  EXPECT_GE(phase(record, "write").at("write_mb").get<double>(), 7.9);
  EXPECT_GE(record.at("total").at("wall_s").get<double>(), compute.at("wall_s").get<double>());

  // finish() is final; the table lists every phase.
  EXPECT_EQ(telemetry.finish(), record);
  const std::string table = formatTelemetry(record);
  for (const char* name : {"compute", "inner", "memory", "write", "total"}) {
    EXPECT_NE(table.find(name), std::string::npos) << name;
  }
}

TEST(TelemetryTest, CountedReadsArePerPhase) {
  Telemetry telemetry("test", {.sample_interval_s = 0.0, .peak_interval_s = 0.0});
  {
    const TelemetryScope scope(telemetry);
    {
      const TelemetryPhase bricks("bricks");
      countRead(CountedRead::kBrickFile, std::uint64_t{3} << 20U);
    }
    {
      const TelemetryPhase mapped("mapped");
      countRead(CountedRead::kMappedFile, std::uint64_t{1} << 20U);
    }
  }
  const Json record = telemetry.finish();
  EXPECT_DOUBLE_EQ(phase(record, "bricks").at("brick_read_mb").get<double>(), 3.0);
  EXPECT_DOUBLE_EQ(phase(record, "bricks").at("mapped_read_mb").get<double>(), 0.0);
  EXPECT_DOUBLE_EQ(phase(record, "mapped").at("mapped_read_mb").get<double>(), 1.0);
  EXPECT_DOUBLE_EQ(phase(record, "mapped").at("brick_read_mb").get<double>(), 0.0);
  EXPECT_DOUBLE_EQ(record.at("total").at("brick_read_mb").get<double>(), 3.0);
}

TEST(TelemetryTest, MappedFilePagesAreNotOwnMemory) {
  const auto file = std::filesystem::temp_directory_path() / "voxelsieve_telemetry_mapped.raw";
  constexpr std::int64_t kSide = 2048;
  constexpr std::int64_t kSlices = 8;  // 64 MiB
  std::vector<std::uint16_t> slice(static_cast<std::size_t>(kSide * kSide), 1000);
  {
    std::ofstream out(file, std::ios::binary);
    for (std::int64_t z = 0; z < kSlices; ++z) {
      out.write(reinterpret_cast<const char*>(slice.data()),
                static_cast<std::streamsize>(slice.size() * sizeof(std::uint16_t)));
    }
  }
  Telemetry telemetry("test", {.sample_interval_s = 0.0});
  {
    const TelemetryScope scope(telemetry);
    {
      const TelemetryPhase before("before");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    {
      const TelemetryPhase read("read");
      const MappedRawSource source(file, {kSide, kSide, kSlices}, VoxelSize(1.0, 1.0, 1.0));
      for (std::int64_t z = 0; z < kSlices; ++z) {
        source.readRegion({{0, 0, z}, {kSide, kSide, z + 1}}, slice);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }
  const Json record = telemetry.finish();
  std::filesystem::remove(file);

  const Json read = phase(record, "read");
  EXPECT_NEAR(read.at("mapped_read_mb").get<double>(), 64.0, 0.1);
  EXPECT_DOUBLE_EQ(read.at("brick_read_mb").get<double>(), 0.0);
  if (record.at("system").at("memory_split").get<bool>()) {
    // The mapped pages are file pages; the process's own memory hardly grows.
    EXPECT_GE(read.at("peak_file_mb").get<double>(), 40.0);
#ifndef VOXELSIEVE_TEST_TSAN
    // ThreadSanitizer keeps shadow memory of its own for every page read, more than the pages.
    EXPECT_LT(read.at("peak_anon_mb").get<double>(),
              phase(record, "before").at("peak_anon_mb").get<double>() + 30.0);
#endif
  }
}

TEST(TelemetryTest, PhasesWithoutARecorderDoNothing) {
  EXPECT_EQ(Telemetry::current(), nullptr);
  { const TelemetryPhase phase("nobody listens"); }
  Telemetry outer("outer", {.sample_interval_s = 0.0});
  {
    const TelemetryScope scope(outer);
    EXPECT_EQ(Telemetry::current(), &outer);
    Telemetry inner("inner", {.sample_interval_s = 0.0});
    {
      const TelemetryScope nested(inner);
      const TelemetryPhase phase("in inner");
    }
    EXPECT_EQ(Telemetry::current(), &outer);
    // Other threads have their own current recorder.
    std::thread([] { EXPECT_EQ(Telemetry::current(), nullptr); }).join();
    const TelemetryPhase phase("in outer");
    EXPECT_EQ(inner.finish().at("phases").size(), 1U);
  }
  EXPECT_EQ(Telemetry::current(), nullptr);
  const Json record = outer.finish();
  ASSERT_EQ(record.at("phases").size(), 1U);
  EXPECT_EQ(record.at("phases")[0].at("name"), "in outer");
}

TEST(TelemetryTest, TimelineIsSampledAndThinned) {
  // Long enough for more than max_samples samples even when wake-ups come late, as on macOS
  // runners, where timer coalescing can delay a 10 ms wait several times over.
  Telemetry telemetry("timeline", {.sample_interval_s = 0.01, .max_samples = 8});
  {
    const TelemetryScope scope(telemetry);
    const TelemetryPhase phase("busy");
    EXPECT_GT(busy(1.5), 0.0);
  }
  const Json record = telemetry.finish();
  const Json& timeline = record.at("timeline");
  const auto points = timeline.at("t_s").size();
  EXPECT_GE(points, 3U);
  EXPECT_LE(points, 9U);
  EXPECT_GT(timeline.at("interval_s").get<double>(), 0.01);  // thinned at least once
  for (const char* series : {"cores_used", "rss_anon_mb", "rss_file_mb", "disk_read_mb_s",
                             "disk_write_mb_s", "major_faults_s", "phase"}) {
    EXPECT_EQ(timeline.at(series).size(), points) << series;
  }
  EXPECT_EQ(timeline.at("phase")[0], "busy");
  EXPECT_FALSE(telemetrySummary(record).contains("timeline"));
}

TEST(TelemetryTest, EveryStepOfAProjectRecordsItsPhases) {
  const auto dir = std::filesystem::temp_directory_path() / "voxelsieve_telemetry_project";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  SyntheticSpec spec;
  spec.lunker_count = 1;
  spec.lunker_radius_mm = 0.5;
  const SyntheticScan scan(boxMesh({6.0, 5.0, 4.0}), spec);
  writeRaw(dir / "scan.raw", scan);
  writeJson(dir / "scan.json", scan.toJson());

  {
    Gui gui({});
    (void)gui.call("project_create", {{"path", (dir / "p").string()}});
    const Json imported =
        gui.call("run_import_raw", {{"path", (dir / "scan.raw").string()}, {"brick_size", 32}});
    const Json& summary = imported.at("telemetry");
    EXPECT_EQ(summary.at("name"), "import_raw");
    EXPECT_FALSE(summary.contains("timeline"));  // the protocol keeps only the summary
    std::vector<std::string> names;
    for (const Json& p : summary.at("phases")) {
      names.push_back(p.at("name"));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"preview", "pass 1 (histogram)", "pass 2 (bricks)",
                                               "levels"}));
    (void)gui.call("run_porosity", {});

    const Json full = gui.call("step_telemetry", {{"step", imported.at("id")}});
    EXPECT_EQ(full.at("step"), imported.at("id"));
    EXPECT_TRUE(full.contains("timeline"));
    EXPECT_EQ(gui.call("step_telemetry", {}).at("name"), "porosity");
    EXPECT_THROW((void)gui.call("step_telemetry", {{"step", 99}}), std::invalid_argument);
  }
  // Kept with the protocol when the project is opened again.
  const Project reopened = Project::open(dir / "p");
  ASSERT_EQ(reopened.steps().size(), 2U);
  EXPECT_EQ(reopened.steps()[1].telemetry.at("name"), "porosity");
  EXPECT_GT(reopened.steps()[1].telemetry.at("total").at("wall_s").get<double>(), 0.0);
  std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace voxelsieve
