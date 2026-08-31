#pragma once

namespace keypulse {

// The setting is opt-in: merely querying it never creates or changes registry data.
[[nodiscard]] bool IsStartupEnabled() noexcept;
[[nodiscard]] bool SetStartupEnabled(bool enabled);

} // namespace keypulse
