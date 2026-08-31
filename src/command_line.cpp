#include "command_line.h"

#include <cwchar>

namespace keypulse {

bool HasBackgroundArgument(int argumentCount, wchar_t* const* arguments) noexcept {
    if (!arguments) return false;
    for (int index = 1; index < argumentCount; ++index) {
        if (arguments[index] && std::wcscmp(arguments[index], L"--background") == 0) {
            return true;
        }
    }
    return false;
}

} // namespace keypulse
