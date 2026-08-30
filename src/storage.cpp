#include "storage.h"

#include <windows.h>
#include <shlobj_core.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <span>
#include <vector>

namespace keypulse {
namespace {

constexpr char kMagic[8] = {'K', 'Y', 'P', 'U', 'L', 'S', 'E', '1'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 40;
constexpr std::size_t kRecordSize = sizeof(std::uint32_t) + kKeySlotCount * sizeof(std::uint64_t);

void AppendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

void AppendU64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

bool ReadU32(std::span<const std::uint8_t> input, std::size_t& offset, std::uint32_t& value) {
    if (input.size() - std::min(input.size(), offset) < sizeof(value)) {
        return false;
    }
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(input[offset++]) << shift;
    }
    return true;
}

bool ReadU64(std::span<const std::uint8_t> input, std::size_t& offset, std::uint64_t& value) {
    if (input.size() - std::min(input.size(), offset) < sizeof(value)) {
        return false;
    }
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(input[offset++]) << shift;
    }
    return true;
}

std::uint32_t Crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool ReadWholeFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return false;
    }
    const auto length = stream.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > 512ull * 1024ull * 1024ull) {
        return false;
    }
    bytes.resize(static_cast<std::size_t>(length));
    stream.seekg(0, std::ios::beg);
    return bytes.empty() || static_cast<bool>(stream.read(
        reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
}

bool WriteWholeFileAndFlush(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool success = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
            success = false;
            break;
        }
        offset += written;
    }
    if (success) {
        success = FlushFileBuffers(file) != FALSE;
    }
    CloseHandle(file);
    return success;
}

// Howard Hinnant's civil-calendar conversion. The returned epoch is 1970-01-01.
constexpr std::int32_t DaysFromCivilImpl(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153u * (month + (month > 2 ? -3 : 9)) + 2u) / 5u + day - 1u;
    const unsigned dayOfEra = yearOfEra * 365u + yearOfEra / 4u - yearOfEra / 100u + dayOfYear;
    return static_cast<std::int32_t>(era * 146097 + static_cast<int>(dayOfEra) - 719468);
}

void CivilFromDaysImpl(std::int32_t days, int& year, unsigned& month, unsigned& day) noexcept {
    int z = days + 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(z - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460u + dayOfEra / 36524u - dayOfEra / 146096u) / 365u;
    year = static_cast<int>(yearOfEra) + era * 400;
    const unsigned dayOfYear = dayOfEra - (365u * yearOfEra + yearOfEra / 4u - yearOfEra / 100u);
    const unsigned monthPrime = (5u * dayOfYear + 2u) / 153u;
    day = dayOfYear - (153u * monthPrime + 2u) / 5u + 1u;
    month = monthPrime < 10 ? monthPrime + 3u : monthPrime - 9u;
    year += month <= 2;
}

} // namespace

StatisticsStore::StatisticsStore(std::filesystem::path path) : path_(std::move(path)) {}

bool StatisticsStore::LoadPath(const std::filesystem::path& source) {
    std::vector<std::uint8_t> bytes;
    if (!ReadWholeFile(source, bytes) || bytes.size() < kHeaderSize) {
        return false;
    }
    if (!std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin())) {
        return false;
    }

    std::size_t offset = sizeof(kMagic);
    std::uint32_t version = 0;
    std::uint32_t slots = 0;
    std::uint32_t recordCount = 0;
    std::uint32_t reserved = 0;
    std::uint64_t payloadSize = 0;
    std::uint32_t payloadCrc = 0;
    std::uint32_t headerCrc = 0;
    if (!ReadU32(bytes, offset, version) || !ReadU32(bytes, offset, slots) ||
        !ReadU32(bytes, offset, recordCount) || !ReadU32(bytes, offset, reserved) ||
        !ReadU64(bytes, offset, payloadSize) || !ReadU32(bytes, offset, payloadCrc) ||
        !ReadU32(bytes, offset, headerCrc)) {
        return false;
    }
    (void)reserved;
    if (version != kFormatVersion || slots != kKeySlotCount ||
        payloadSize != static_cast<std::uint64_t>(recordCount) * kRecordSize ||
        payloadSize != bytes.size() - kHeaderSize ||
        Crc32(std::span(bytes).first(kHeaderSize - sizeof(std::uint32_t))) != headerCrc ||
        Crc32(std::span(bytes).subspan(kHeaderSize)) != payloadCrc) {
        return false;
    }

    std::map<std::int32_t, std::array<std::uint64_t, kKeySlotCount>> loaded;
    offset = kHeaderSize;
    for (std::uint32_t index = 0; index < recordCount; ++index) {
        std::uint32_t encodedDay = 0;
        if (!ReadU32(bytes, offset, encodedDay)) {
            return false;
        }
        const auto day = static_cast<std::int32_t>(encodedDay);
        auto [iterator, inserted] = loaded.try_emplace(day);
        if (!inserted) {
            return false;
        }
        for (auto& count : iterator->second) {
            if (!ReadU64(bytes, offset, count)) {
                return false;
            }
        }
    }
    if (offset != bytes.size()) {
        return false;
    }

    days_ = std::move(loaded);
    dirty_ = false;
    return true;
}

bool StatisticsStore::Load() {
    days_.clear();
    dirty_ = false;
    lastError_.clear();

    std::error_code error;
    if (!std::filesystem::exists(path_, error)) {
        return true;
    }
    if (LoadPath(path_)) {
        return true;
    }

    auto backup = path_;
    backup += L".bak";
    if (std::filesystem::exists(backup, error) && LoadPath(backup)) {
        lastError_ = L"主数据文件校验失败，已从备份恢复";
        dirty_ = true;
        return true;
    }

    lastError_ = L"数据文件校验失败；原文件已保留，当前使用空白统计";
    return false;
}

bool StatisticsStore::Save() {
    if (!dirty_) {
        return true;
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(days_.size() * kRecordSize);
    for (const auto& [day, counts] : days_) {
        AppendU32(payload, static_cast<std::uint32_t>(day));
        for (const auto count : counts) {
            AppendU64(payload, count);
        }
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderSize + payload.size());
    bytes.insert(bytes.end(), std::begin(kMagic), std::end(kMagic));
    AppendU32(bytes, kFormatVersion);
    AppendU32(bytes, static_cast<std::uint32_t>(kKeySlotCount));
    AppendU32(bytes, static_cast<std::uint32_t>(days_.size()));
    AppendU32(bytes, 0);
    AppendU64(bytes, static_cast<std::uint64_t>(payload.size()));
    AppendU32(bytes, Crc32(payload));
    AppendU32(bytes, Crc32(bytes));
    bytes.insert(bytes.end(), payload.begin(), payload.end());

    std::error_code directoryError;
    std::filesystem::create_directories(path_.parent_path(), directoryError);
    if (directoryError) {
        lastError_ = L"无法创建数据目录";
        return false;
    }

    auto temporary = path_;
    temporary += L".tmp";
    if (!WriteWholeFileAndFlush(temporary, bytes)) {
        lastError_ = L"无法写入临时数据文件";
        return false;
    }

    bool replaced = false;
    if (std::filesystem::exists(path_)) {
        auto backup = path_;
        backup += L".bak";
        replaced = ReplaceFileW(path_.c_str(), temporary.c_str(), backup.c_str(),
                                REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
    } else {
        replaced = MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!replaced) {
        replaced = MoveFileExW(temporary.c_str(), path_.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!replaced) {
        lastError_ = L"无法原子替换数据文件";
        DeleteFileW(temporary.c_str());
        return false;
    }

    dirty_ = false;
    lastError_.clear();
    return true;
}

void StatisticsStore::Increment(std::uint16_t keyCode, std::int32_t day, std::uint64_t amount) {
    if (keyCode >= kKeySlotCount || amount == 0) {
        return;
    }
    auto& count = days_[day][keyCode];
    if (std::numeric_limits<std::uint64_t>::max() - count < amount) {
        count = std::numeric_limits<std::uint64_t>::max();
    } else {
        count += amount;
    }
    dirty_ = true;
}

Snapshot StatisticsStore::Query(std::int32_t firstDayInclusive, std::int32_t lastDayExclusive) const {
    Snapshot result;
    for (auto iterator = days_.lower_bound(firstDayInclusive);
         iterator != days_.end() && iterator->first < lastDayExclusive; ++iterator) {
        for (std::size_t key = 0; key < kKeySlotCount; ++key) {
            const auto amount = iterator->second[key];
            result.counts[key] += amount;
            result.total += amount;
        }
    }
    return result;
}

std::filesystem::path DefaultStatisticsPath() {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData))) {
        std::filesystem::path result(localAppData);
        CoTaskMemFree(localAppData);
        return result / L"KeyPulse" / L"stats.kpf";
    }
    return std::filesystem::current_path() / L"stats.kpf";
}

std::int32_t DayFromCivilDate(int year, unsigned month, unsigned day) {
    return DaysFromCivilImpl(year, month, day);
}

CivilDate CivilDateFromDay(std::int32_t value) {
    CivilDate result;
    CivilFromDaysImpl(value, result.year, result.month, result.day);
    return result;
}

std::int32_t TodayLocalDay() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return DaysFromCivilImpl(now.wYear, now.wMonth, now.wDay);
}

std::wstring FormatLocalDay(std::int32_t value) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    CivilFromDaysImpl(value, year, month, day);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04d-%02u-%02u", year, month, day);
    return buffer;
}

} // namespace keypulse
