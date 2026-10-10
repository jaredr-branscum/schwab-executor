#include "schwab/risk_guard.hpp"

namespace schwab {

// All hot-path methods of RiskGuard (including validate_order) are defined inline
// in include/schwab/risk_guard.hpp to eliminate subroutine call overhead on the critical path.

} // namespace schwab
