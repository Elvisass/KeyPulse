#pragma once

namespace keypulse {

// The setting is opt-in: merely querying it never creates or changes registry data.
[[nodiscard]] bool IsStartupEnabled() noexcept;
[[nodiscard]] bool SetStartupEnabled(bool enabled);
// Migrates an existing opt-in value to the current executable and arguments.
// It is a no-op when startup is disabled.
void RefreshStartupCommandIfEnabled();

} // namespace keypulse
