#pragma once

#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>

namespace keypulse {

// Raw Input is authoritative. Hook events wait briefly and are emitted only when
// no matching Raw Input event arrives, which fills system-hotkey gaps without
// double-counting ordinary keyboard input.
class KeyEventMerger final {
public:
    // Returns true for a new Raw Input press that should be counted immediately.
    // Repeated down events remain suppressed until the corresponding release.
    [[nodiscard]] bool ObserveRaw(std::uint16_t keyCode, bool keyDown,
                                  std::uint32_t messageTime, std::uint64_t arrivalTime);
    // Returns true when a new hook-only press was queued for delayed fallback.
    // Releases only update state and always return false.
    [[nodiscard]] bool ObserveHook(std::uint16_t keyCode, bool keyDown,
                                   std::uint32_t messageTime, std::uint64_t arrivalTime);
    [[nodiscard]] std::vector<std::uint16_t> TakeReady(std::uint64_t now);
    [[nodiscard]] bool has_pending() const noexcept { return !pendingHooks_.empty(); }
    void Clear() noexcept;

private:
    struct TimedEvent {
        std::uint16_t keyCode = 0;
        std::uint32_t messageTime = 0;
        std::uint64_t arrivalTime = 0;
    };

    static constexpr std::uint32_t kCorrelationWindowMs = 32;
    static constexpr std::uint64_t kHookFallbackDelayMs = 80;
    static constexpr std::uint64_t kRecentRawLifetimeMs = 250;

    static bool IsMatch(const TimedEvent& event, std::uint16_t keyCode,
                        std::uint32_t messageTime) noexcept;
    void PruneRecentRaw(std::uint64_t now);

    std::deque<TimedEvent> pendingHooks_;
    std::deque<TimedEvent> recentRaw_;
    std::unordered_set<std::uint16_t> rawPressed_;
    std::unordered_set<std::uint16_t> hookPressed_;
};

} // namespace keypulse
