#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace keypulse {

constexpr std::size_t kKeySlotCount = 768;
constexpr std::uint16_t kUnavailableKey = 0xFFFF;

struct Snapshot {
    std::array<std::uint64_t, kKeySlotCount> counts{};
    std::uint64_t total = 0;
};

struct CivilDate {
    int year = 1970;
    unsigned month = 1;
    unsigned day = 1;
};

class StatisticsStore final {
public:
    explicit StatisticsStore(std::filesystem::path path);

    bool Load();
    bool Save();
    void Increment(std::uint16_t keyCode, std::int32_t day, std::uint64_t amount = 1);

    [[nodiscard]] Snapshot Query(std::int32_t firstDayInclusive,
                                 std::int32_t lastDayExclusive) const;
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] std::size_t day_count() const noexcept { return days_.size(); }
    [[nodiscard]] const std::wstring& last_error() const noexcept { return lastError_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    bool LoadPath(const std::filesystem::path& source);

    std::filesystem::path path_;
    std::map<std::int32_t, std::array<std::uint64_t, kKeySlotCount>> days_;
    bool dirty_ = false;
    std::wstring lastError_;
};

[[nodiscard]] std::filesystem::path DefaultStatisticsPath();
[[nodiscard]] std::int32_t DayFromCivilDate(int year, unsigned month, unsigned day);
[[nodiscard]] CivilDate CivilDateFromDay(std::int32_t day);
[[nodiscard]] std::int32_t TodayLocalDay();
[[nodiscard]] std::wstring FormatLocalDay(std::int32_t day);

} // namespace keypulse
