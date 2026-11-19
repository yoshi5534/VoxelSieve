#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>

namespace voxelsieve {

/// Thrown by a run that was cancelled (`ProgressToken::check`). Its message is "Cancelled", which
/// the project records for the step.
class Cancelled : public std::runtime_error {
 public:
  Cancelled() : std::runtime_error("Cancelled") {}
};

/// Progress and cancellation of a long run (review step 5.1). The option structs of the analyses
/// hold one; the default reports nothing and is never cancelled. Copies share the run: a token
/// for a part of the run (`part`) reports into the same callback. All methods may be called from
/// several threads.
class ProgressToken {
 public:
  ProgressToken() = default;
  /// `report` gets the fraction of the run done, 0 to 1, never decreasing and at most once per
  /// whole percent, from one thread at a time; it may throw to stop the run. The run is cancelled
  /// once `*cancel` is set.
  ProgressToken(std::function<void(double fraction)> report, const std::atomic<bool>* cancel);

  /// Whether the run was cancelled.
  [[nodiscard]] bool cancelled() const;
  /// Throws `Cancelled` once the run was cancelled. Cheap: call it per brick, tile or slice, at
  /// a point where nothing is half done.
  void check() const;
  /// Reports that `fraction` (0 to 1) of this token's part is done, after `check`.
  void report(double fraction) const;
  /// The token of [begin, end) of this token's part, for one stage of the run.
  [[nodiscard]] ProgressToken part(double begin, double end) const;

 private:
  struct State;
  std::shared_ptr<State> state_;
  double begin_ = 0.0;
  double end_ = 1.0;
};

/// Counts the steps of one stage of a run and reports them through a token; `step` checks for
/// cancellation first. For several threads at once.
class ProgressSteps {
 public:
  ProgressSteps(ProgressToken token, std::size_t total);
  /// Throws `Cancelled` once the run was cancelled, else counts `n` steps as done.
  void step(std::size_t n = 1);

 private:
  ProgressToken token_;
  std::size_t total_;
  std::atomic<std::size_t> done_{0};
};

}  // namespace voxelsieve
