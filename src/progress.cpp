#include "voxelsieve/progress.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>

namespace voxelsieve {

struct ProgressToken::State {
  std::function<void(double)> report;
  const std::atomic<bool>* cancel = nullptr;
  std::mutex mutex;
  int percent = -1;  // last reported
};

ProgressToken::ProgressToken(std::function<void(double fraction)> report,
                             const std::atomic<bool>* cancel)
    : state_(std::make_shared<State>()) {
  state_->report = std::move(report);
  state_->cancel = cancel;
}

bool ProgressToken::cancelled() const {
  return state_ != nullptr && state_->cancel != nullptr && state_->cancel->load();
}

void ProgressToken::check() const {
  if (cancelled()) {
    throw Cancelled();
  }
}

void ProgressToken::report(double fraction) const {
  check();
  if (state_ == nullptr || !state_->report) {
    return;
  }
  const double overall = begin_ + (end_ - begin_) * std::clamp(fraction, 0.0, 1.0);
  const auto percent = static_cast<int>(std::floor(100.0 * overall + 1e-9));
  const std::scoped_lock lock(state_->mutex);
  if (percent <= state_->percent) {
    return;
  }
  state_->percent = percent;
  state_->report(percent / 100.0);
}

ProgressToken ProgressToken::part(double begin, double end) const {
  ProgressToken token = *this;
  token.begin_ = begin_ + (end_ - begin_) * begin;
  token.end_ = begin_ + (end_ - begin_) * end;
  return token;
}

ProgressSteps::ProgressSteps(ProgressToken token, std::size_t total)
    : token_(std::move(token)), total_(std::max<std::size_t>(total, 1)) {}

void ProgressSteps::step(std::size_t n) {
  token_.check();
  const std::size_t done = done_ += n;
  token_.report(static_cast<double>(done) / static_cast<double>(total_));
}

}  // namespace voxelsieve
