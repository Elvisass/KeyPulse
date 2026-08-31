#include "startup.h"

#include <windows.h>

#include <optional>
#include <string>

namespace keypulse {
namespace {

constexpr wchar_t kStartupKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupValue[] = L"KeyPulse";

std::optional<std::wstring> CurrentExecutableCommand() {
    DWORD capacity = MAX_PATH;
    while (capacity <= 32'768) {
        std::wstring path(capacity, L'\0');
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), capacity);
        if (length == 0) return std::nullopt;
        if (length < capacity) {
            path.resize(length);
            return L"\"" + path + L"\" --background";
        }
        capacity *= 2;
    }
    return std::nullopt;
}

} // namespace

bool IsStartupEnabled() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS result = RegQueryValueExW(key, kStartupValue, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool SetStartupEnabled(bool enabled) {
    if (!enabled) {
        HKEY key = nullptr;
        const LSTATUS openResult = RegOpenKeyExW(
            HKEY_CURRENT_USER, kStartupKey, 0, KEY_SET_VALUE, &key);
        if (openResult == ERROR_FILE_NOT_FOUND || openResult == ERROR_PATH_NOT_FOUND) return true;
        if (openResult != ERROR_SUCCESS) return false;
        const LSTATUS deleteResult = RegDeleteValueW(key, kStartupValue);
        RegCloseKey(key);
        return deleteResult == ERROR_SUCCESS || deleteResult == ERROR_FILE_NOT_FOUND;
    }

    const auto command = CurrentExecutableCommand();
    if (!command) return false;

    HKEY key = nullptr;
    DWORD disposition = 0;
    const LSTATUS createResult = RegCreateKeyExW(
        HKEY_CURRENT_USER, kStartupKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, nullptr, &key, &disposition);
    if (createResult != ERROR_SUCCESS) return false;
    const DWORD byteCount = static_cast<DWORD>((command->size() + 1) * sizeof(wchar_t));
    const LSTATUS setResult = RegSetValueExW(
        key, kStartupValue, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command->c_str()), byteCount);
    RegCloseKey(key);
    return setResult == ERROR_SUCCESS;
}

void RefreshStartupCommandIfEnabled() {
    if (IsStartupEnabled()) {
        (void)SetStartupEnabled(true);
    }
}

} // namespace keypulse
