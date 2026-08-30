#include "input_merge.h"

#include <cstdint>
#include <iostream>

int main() {
    constexpr std::uint16_t space = 0x39;
    constexpr std::uint16_t leftWindows = 0x15B;

    // Hook first, then Raw Input: Raw Input is counted by the caller and cancels fallback.
    {
        keypulse::KeyEventMerger merger;
        if (!merger.ObserveHook(space, 100, 1'000)) return 10;
        merger.ObserveRaw(space, 101, 1'004);
        if (merger.has_pending() || !merger.TakeReady(2'000).empty()) return 11;
    }

    // Raw Input first, then hook: the later hook event is recognized as the same press.
    {
        keypulse::KeyEventMerger merger;
        merger.ObserveRaw(space, 200, 2'000);
        if (merger.ObserveHook(space, 200, 2'003) || merger.has_pending()) return 12;
    }

    // A hook-only event is emitted after the short correlation window.
    {
        keypulse::KeyEventMerger merger;
        if (!merger.ObserveHook(space, 300, 3'000)) return 13;
        if (!merger.TakeReady(3'079).empty()) return 14;
        const auto ready = merger.TakeReady(3'080);
        if (ready.size() != 1 || ready.front() != space || merger.has_pending()) return 15;
    }

    // Repeats are paired one-for-one even when the same scan code arrives rapidly.
    {
        keypulse::KeyEventMerger merger;
        (void)merger.ObserveHook(space, 400, 4'000);
        (void)merger.ObserveHook(space, 420, 4'020);
        merger.ObserveRaw(space, 420, 4'022);
        merger.ObserveRaw(space, 400, 4'024);
        if (merger.has_pending() || !merger.TakeReady(5'000).empty()) return 16;
    }

    // Different keys never cancel one another.
    {
        keypulse::KeyEventMerger merger;
        (void)merger.ObserveHook(leftWindows, 500, 5'000);
        merger.ObserveRaw(space, 500, 5'002);
        const auto ready = merger.TakeReady(5'080);
        if (ready.size() != 1 || ready.front() != leftWindows) return 17;
    }

    // Message timestamps wrap at 32 bits; nearby events across the wrap still match.
    {
        keypulse::KeyEventMerger merger;
        merger.ObserveRaw(space, UINT32_MAX - 7u, 6'000);
        if (merger.ObserveHook(space, 4, 6'002)) return 18;
    }

    std::cout << "input merge tests passed\n";
    return 0;
}
