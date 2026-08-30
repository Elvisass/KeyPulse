#include "input_merge.h"

#include <algorithm>

namespace keypulse {
namespace {

std::uint32_t MessageTimeDistance(std::uint32_t left, std::uint32_t right) noexcept {
    return std::min(left - right, right - left);
}

} // namespace

bool KeyEventMerger::IsMatch(const TimedEvent& event, std::uint16_t keyCode,
                             std::uint32_t messageTime) noexcept {
    return event.keyCode == keyCode &&
           MessageTimeDistance(event.messageTime, messageTime) <= kCorrelationWindowMs;
}

void KeyEventMerger::PruneRecentRaw(std::uint64_t now) {
    while (!recentRaw_.empty() &&
           now - recentRaw_.front().arrivalTime > kRecentRawLifetimeMs) {
        recentRaw_.pop_front();
    }
}

bool KeyEventMerger::ObserveRaw(std::uint16_t keyCode, bool keyDown,
                                std::uint32_t messageTime, std::uint64_t arrivalTime) {
    if (!keyDown) {
        rawPressed_.erase(keyCode);
        return false;
    }
    if (!rawPressed_.insert(keyCode).second) {
        return false;
    }

    PruneRecentRaw(arrivalTime);
    const auto pending = std::find_if(
        pendingHooks_.begin(), pendingHooks_.end(),
        [keyCode, messageTime](const TimedEvent& event) {
            return IsMatch(event, keyCode, messageTime);
        });
    if (pending != pendingHooks_.end()) {
        pendingHooks_.erase(pending);
        return true;
    }
    recentRaw_.push_back({keyCode, messageTime, arrivalTime});
    return true;
}

bool KeyEventMerger::ObserveHook(std::uint16_t keyCode, bool keyDown,
                                 std::uint32_t messageTime, std::uint64_t arrivalTime) {
    if (!keyDown) {
        hookPressed_.erase(keyCode);
        return false;
    }
    if (!hookPressed_.insert(keyCode).second) {
        return false;
    }

    PruneRecentRaw(arrivalTime);
    const auto recent = std::find_if(
        recentRaw_.begin(), recentRaw_.end(),
        [keyCode, messageTime](const TimedEvent& event) {
            return IsMatch(event, keyCode, messageTime);
        });
    if (recent != recentRaw_.end()) {
        recentRaw_.erase(recent);
        return false;
    }
    pendingHooks_.push_back({keyCode, messageTime, arrivalTime});
    return true;
}

std::vector<std::uint16_t> KeyEventMerger::TakeReady(std::uint64_t now) {
    PruneRecentRaw(now);
    std::vector<std::uint16_t> ready;
    for (auto event = pendingHooks_.begin(); event != pendingHooks_.end();) {
        if (now - event->arrivalTime >= kHookFallbackDelayMs) {
            ready.push_back(event->keyCode);
            event = pendingHooks_.erase(event);
        } else {
            ++event;
        }
    }
    return ready;
}

void KeyEventMerger::Clear() noexcept {
    pendingHooks_.clear();
    recentRaw_.clear();
    rawPressed_.clear();
    hookPressed_.clear();
}

} // namespace keypulse
