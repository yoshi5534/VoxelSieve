#pragma once

#include <cstdint>

namespace voxelsieve::detail {

/// The budgets of all brick caches of the process, the shared one included also while it is not
/// open: the part of the memory budget the caches may take (ADR 0034).
[[nodiscard]] std::uint64_t cacheBudgetBytes();

/// Gives the shared cache, when its size was not set, the default share of a new memory budget.
void sharedCacheFollowsBudget();

}  // namespace voxelsieve::detail
