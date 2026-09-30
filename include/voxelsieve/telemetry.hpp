#pragma once

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace voxelsieve {

struct TelemetryOptions {
  /// Seconds between two points of the timeline; 0 records no timeline, only the phases.
  double sample_interval_s = 1.0;
  /// Longer runs keep every other point and double the interval, so a record stays small.
  std::size_t max_samples = 600;
};

/// Records how long a run (an operation or a command-line tool) takes and what it uses, per
/// phase: wall time, CPU time and the cores it kept busy, peak memory split into the process's
/// own memory and mapped file pages, bytes read and written, and page faults (ADR 0017). A
/// background thread samples the same counters into a timeline. Everything stays local.
///
/// The counters are those of the whole process, read from getrusage and /proc (Linux). Only one
/// recorder should be active at a time; the studio runs one operation at a time.
class Telemetry {
 public:
  explicit Telemetry(std::string name, TelemetryOptions options = {});
  ~Telemetry();
  Telemetry(const Telemetry&) = delete;
  Telemetry& operator=(const Telemetry&) = delete;
  Telemetry(Telemetry&&) = delete;
  Telemetry& operator=(Telemetry&&) = delete;

  /// Starts a phase; phases may nest. Prefer TelemetryPhase.
  void beginPhase(const std::string& name);
  /// Ends the innermost open phase.
  void endPhase();

  /// Ends open phases, stops the timeline and returns the record. Later calls return the same
  /// record.
  [[nodiscard]] nlohmann::json finish();

  /// The recorder that phases of the calling thread go to, or null.
  [[nodiscard]] static Telemetry* current();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Makes a recorder the current one of the calling thread while it lives, so library code can mark
/// its phases without being handed the recorder.
class TelemetryScope {
 public:
  explicit TelemetryScope(Telemetry& telemetry);
  ~TelemetryScope();
  TelemetryScope(const TelemetryScope&) = delete;
  TelemetryScope& operator=(const TelemetryScope&) = delete;
  TelemetryScope(TelemetryScope&&) = delete;
  TelemetryScope& operator=(TelemetryScope&&) = delete;

 private:
  Telemetry* previous_;
};

/// A phase of the calling thread's current recorder; does nothing without one.
class TelemetryPhase {
 public:
  explicit TelemetryPhase(const std::string& name);
  ~TelemetryPhase();
  TelemetryPhase(const TelemetryPhase&) = delete;
  TelemetryPhase& operator=(const TelemetryPhase&) = delete;
  TelemetryPhase(TelemetryPhase&&) = delete;
  TelemetryPhase& operator=(TelemetryPhase&&) = delete;

 private:
  Telemetry* telemetry_;
};

/// The record without its timeline, as kept in the project protocol.
[[nodiscard]] nlohmann::json telemetrySummary(const nlohmann::json& record);

/// A table of the phases and the hints of a record, for the command line.
[[nodiscard]] std::string formatTelemetry(const nlohmann::json& record);

/// For command-line tools: ends the record, prints its table to stderr and writes the whole record
/// as JSON to `file` unless it is empty.
void reportTelemetry(Telemetry& telemetry, const std::filesystem::path& file);

}  // namespace voxelsieve
