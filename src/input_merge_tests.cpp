#include "input_merge.h"

#include <cstdint>
#include <iostream>

int main() {
    constexpr std::uint16_t space = 0x39;
    constexpr std::uint16_t leftWindows = 0x15B;

    // Hook first, then Raw Input: Raw Input is counted by the caller and cancels fallback.
    {
        keypulse::KeyEventMerger merger;
        if (!merger.ObserveHook(space, true, 100, 1'000)) return 10;
        if (!merger.ObserveRaw(space, true, 101, 1'004)) return 11;
        if (merger.has_pending() || !merger.TakeReady(2'000).empty()) return 12;
    }

    // Raw Input first, then hook: the later hook event is recognized as the same press.
    {
        keypulse::KeyEventMerger merger;
        if (!merger.ObserveRaw(space, true, 200, 2'000)) return 13;
        if (merger.ObserveHook(space, true, 200, 2'003) || merger.has_pending()) return 14;
    }

    // A hook-only event is emitted after the short correlation window.
    {
        keypulse::KeyEventMerger merger;
        if (!merger.ObserveHook(space, true, 300, 3'000)) return 15;
        if (!merger.TakeReady(3'079).empty()) return 16;
        const auto ready = merger.TakeReady(3'080);
        if (ready.size() != 1 || ready.front() != space || merger.has_pending()) return 17;
    }

    // Auto-repeat down events from either source are ignored until key-up.
    {
        keypulse::KeyEventMerger merger;
        if (!merger.ObserveHook(space, true, 400, 4'000)) return 18;
        if (merger.ObserveHook(space, true, 420, 4'020)) return 19;
        if (!merger.ObserveRaw(space, true, 400, 4'022)) return 20;
        if (merger.ObserveRaw(space, true, 420, 4'024)) return 21;
        if (merger.has_pending() || !merger.TakeReady(5'000).empty()) return 22;
    }

    // A release rearms the key, so the next physical press is counted again.
    {
        keypulse::KeyEventMerger merger;
        if (!merger.ObserveRaw(space, true, 600, 6'000)) return 23;
        if (merger.ObserveRaw(space, false, 610, 6'010)) return 24;
        if (!merger.ObserveRaw(space, true, 620, 6'020)) return 25;

        if (!merger.ObserveHook(leftWindows, true, 630, 6'030)) return 26;
        if (merger.ObserveHook(leftWindows, false, 640, 6'040)) return 27;
        if (!merger.ObserveHook(leftWindows, true, 650, 6'050)) return 28;
        const auto ready = merger.TakeReady(6'130);
        if (ready.size() != 2 || ready[0] != leftWindows || ready[1] != leftWindows) return 29;
    }

    // Different keys never cancel one another.
    {
        keypulse::KeyEventMerger merger;
        (void)merger.ObserveHook(leftWindows, true, 700, 7'000);
        (void)merger.ObserveRaw(space, true, 700, 7'002);
        const auto ready = merger.TakeReady(7'080);
        if (ready.size() != 1 || ready.front() != leftWindows) return 30;
    }

    // Message timestamps wrap at 32 bits; nearby events across the wrap still match.
    {
        keypulse::KeyEventMerger merger;
        (void)merger.ObserveRaw(space, true, UINT32_MAX - 7u, 8'000);
        if (merger.ObserveHook(space, true, 4, 8'002)) return 31;
    }

    // Clearing capture state also rearms keys after pause/device reset boundaries.
    {
        keypulse::KeyEventMerger merger;
        if (!merger.ObserveRaw(space, true, 900, 9'000)) return 32;
        merger.Clear();
        if (!merger.ObserveRaw(space, true, 910, 9'010)) return 33;
    }

    std::cout << "input merge tests passed\n";
    return 0;
}
