#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace keypulse {

// Raw Input is authoritative. Hook events wait briefly and are emitted only when
// no matching Raw Input event arrives, which fills system-hotkey gaps without
// double-counting ordinary keyboard input.
class KeyEventMerger final {
public:
    void ObserveRaw(std::uint16_t keyCode, std::uint32_t messageTime,
                    std::uint64_t arrivalTime);
    [[nodiscard]] bool ObserveHook(std::uint16_t keyCode, std::uint32_t messageTime,
                                   std::uint64_t arrivalTime);
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
};

} // namespace keypulse
