#include "command_line.h"

#include <iostream>

int main() {
    wchar_t program[] = L"KeyPulse.exe";
    wchar_t background[] = L"--background";
    wchar_t unrelated[] = L"--unrelated";
    wchar_t wrongCase[] = L"--Background";

    wchar_t* noArguments[]{program};
    if (keypulse::HasBackgroundArgument(1, noArguments)) return 10;

    wchar_t* backgroundArguments[]{program, background};
    if (!keypulse::HasBackgroundArgument(2, backgroundArguments)) return 11;

    wchar_t* laterArgument[]{program, unrelated, background};
    if (!keypulse::HasBackgroundArgument(3, laterArgument)) return 12;

    wchar_t* invalidArguments[]{program, wrongCase, unrelated};
    if (keypulse::HasBackgroundArgument(3, invalidArguments)) return 13;
    if (keypulse::HasBackgroundArgument(0, nullptr)) return 14;

    std::cout << "command line tests passed\n";
    return 0;
}
