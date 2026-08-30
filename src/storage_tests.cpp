#include "storage.h"

#include <filesystem>
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::wcerr << L"usage: KeyPulseStorageTests <temporary-file>\n";
        return 2;
    }

    const std::filesystem::path path(argv[1]);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.wstring() + L".bak", ignored);
    std::filesystem::remove(path.wstring() + L".tmp", ignored);

    const auto today = keypulse::TodayLocalDay();
    {
        keypulse::StatisticsStore store(path);
        if (!store.Load()) return 10;
        store.Increment(0x1E, today, 3);
        store.Increment(0x139, today, 7);
        store.Increment(0x1E, today - 8, 11);
        if (!store.Save()) return 11;
    }
    {
        keypulse::StatisticsStore store(path);
        if (!store.Load()) return 12;
        const auto todayOnly = store.Query(today, today + 1);
        if (todayOnly.total != 10 || todayOnly.counts[0x1E] != 3 || todayOnly.counts[0x139] != 7) {
            return 13;
        }
        const auto thirtyDays = store.Query(today - 29, today + 1);
        if (thirtyDays.total != 21 || thirtyDays.counts[0x1E] != 14) {
            return 14;
        }
        if (store.dirty() || store.day_count() != 2) return 15;
        store.Increment(0x1E, today, 2);
        if (!store.Save()) return 16;
    }
    // A second save creates .bak. Corrupting the primary must recover the prior valid snapshot.
    {
        std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!stream) return 17;
        stream.put('X');
    }
    {
        keypulse::StatisticsStore store(path);
        if (!store.Load() || !store.dirty() || store.last_error().empty()) return 18;
        const auto recovered = store.Query(today - 29, today + 1);
        if (recovered.total != 21 || recovered.counts[0x1E] != 14) return 19;
        if (!store.Save()) return 20;
    }
    {
        keypulse::StatisticsStore store(path);
        if (!store.Load() || store.dirty()) return 21;
        if (store.Query(today - 29, today + 1).total != 21) return 22;
    }

    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.wstring() + L".bak", ignored);
    std::wcout << L"storage round-trip passed\n";
    return 0;
}
