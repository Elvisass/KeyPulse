#include "storage.h"
#include "input_merge.h"
#include "startup.h"
#include "window_layout.h"
#include "../resource.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"KeyPulse.MainWindow.v1";
constexpr wchar_t kWindowTitle[] = L"键频 · KeyPulse";
constexpr UINT kTrayMessage = WM_APP + 20;
constexpr UINT kHookKeyboardMessage = WM_APP + 21;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT_PTR kHookFlushTimerId = 2;
constexpr UINT kCommandOpen = 41001;
constexpr UINT kCommandPause = 41002;
constexpr UINT kCommandCompact = 41003;
constexpr UINT kCommandExit = 41004;
constexpr UINT kCommandStartup = 41005;
constexpr float kCanvasWidth = 1400.0f;
constexpr float kNormalCanvasHeight = 646.0f;
constexpr float kCompactCanvasHeight = 461.0f;
constexpr float kTitleBarHeight = 38.0f;
constexpr float kMinimumCanvasWidth = 900.0f;

constexpr std::uint16_t Scan(std::uint16_t code) { return code; }
constexpr std::uint16_t E0(std::uint16_t code) { return static_cast<std::uint16_t>(0x100u | code); }
constexpr std::uint16_t E1(std::uint16_t code) { return static_cast<std::uint16_t>(0x200u | code); }

bool Contains(const D2D1_RECT_F& rectangle, float x, float y) {
    return x >= rectangle.left && x <= rectangle.right && y >= rectangle.top && y <= rectangle.bottom;
}

D2D1_COLOR_F Rgb(std::uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(rgb, alpha);
}

D2D1_COLOR_F Mix(D2D1_COLOR_F left, D2D1_COLOR_F right, float amount) {
    amount = std::clamp(amount, 0.0f, 1.0f);
    return D2D1::ColorF(left.r + (right.r - left.r) * amount,
                        left.g + (right.g - left.g) * amount,
                        left.b + (right.b - left.b) * amount,
                        left.a + (right.a - left.a) * amount);
}

std::wstring FormatCount(std::uint64_t value) {
    const std::wstring digits = std::to_wstring(value);
    std::wstring formatted;
    formatted.reserve(digits.size() + digits.size() / 3);
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index != 0 && (digits.size() - index) % 3 == 0) {
            formatted.push_back(L',');
        }
        formatted.push_back(digits[index]);
    }
    return formatted;
}

SYSTEMTIME SystemTimeForDay(std::int32_t day) {
    const auto date = keypulse::CivilDateFromDay(day);
    SYSTEMTIME result{};
    result.wYear = static_cast<WORD>(date.year);
    result.wMonth = static_cast<WORD>(date.month);
    result.wDay = static_cast<WORD>(date.day);
    return result;
}

std::int32_t DayForSystemTime(const SYSTEMTIME& value) {
    return keypulse::DayFromCivilDate(value.wYear, value.wMonth, value.wDay);
}

std::optional<std::uint16_t> NormalizeKeyCode(UINT virtualKey, UINT makeCode,
                                              bool extended, bool e1 = false) {
    std::uint16_t scanCode = static_cast<std::uint16_t>(makeCode & 0xFFu);
    std::uint16_t prefix = extended ? 0x100u : e1 ? 0x200u : 0u;
    if (scanCode == 0) {
        const UINT mapped = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC_EX);
        scanCode = static_cast<std::uint16_t>(mapped & 0xFFu);
        if ((mapped & 0xFF00u) == 0xE000u) prefix = 0x100u;
        if ((mapped & 0xFF00u) == 0xE100u) prefix = 0x200u;
    }
    // E0-prefixed synthetic shifts are part of PrintScreen's compatibility sequence.
    if (scanCode == 0 || (prefix == 0x100u && (scanCode == 0x2A || scanCode == 0x36))) {
        return std::nullopt;
    }

    std::uint16_t keyCode = static_cast<std::uint16_t>(prefix | scanCode);
    if (virtualKey == VK_SNAPSHOT) keyCode = E0(0x37);
    if (virtualKey == VK_PAUSE) keyCode = E1(0x45);
    if (virtualKey == VK_NUMLOCK) keyCode = Scan(0x45);
    if (keyCode >= keypulse::kKeySlotCount) return std::nullopt;
    return keyCode;
}

thread_local HWND hookTargetWindow = nullptr;

LRESULT CALLBACK LowLevelKeyboardHook(int code, WPARAM wParam, LPARAM lParam) {
    const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    const bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    if (code == HC_ACTION && (keyDown || keyUp)) {
        const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if ((event->flags & LLKHF_INJECTED) == 0) {
            const auto keyCode = NormalizeKeyCode(
                event->vkCode, event->scanCode, (event->flags & LLKHF_EXTENDED) != 0);
            if (keyCode && hookTargetWindow) {
                PostMessageW(hookTargetWindow, kHookKeyboardMessage,
                             MAKEWPARAM(*keyCode, keyDown ? 1 : 0),
                             static_cast<LPARAM>(event->time));
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

struct KeyboardHookThreadContext {
    HINSTANCE instance = nullptr;
    HWND targetWindow = nullptr;
    HANDLE readyEvent = nullptr;
    HHOOK hook = nullptr;
    DWORD threadId = 0;
};

DWORD WINAPI KeyboardHookThread(void* parameter) {
    auto& context = *static_cast<KeyboardHookThreadContext*>(parameter);
    context.threadId = GetCurrentThreadId();
    hookTargetWindow = context.targetWindow;
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    context.hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardHook, context.instance, 0);
    SetEvent(context.readyEvent);

    if (context.hook) {
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        UnhookWindowsHookEx(context.hook);
        context.hook = nullptr;
    }
    hookTargetWindow = nullptr;
    return 0;
}

UINT GetSystemDpiCompat() {
    using GetDpiForSystemFunction = UINT(WINAPI*)();
    if (const HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        const auto getDpiForSystem = reinterpret_cast<GetDpiForSystemFunction>(
            GetProcAddress(user32, "GetDpiForSystem"));
        if (getDpiForSystem) {
            const UINT dpi = getDpiForSystem();
            if (dpi != 0) return dpi;
        }
    }

    const HDC screen = GetDC(nullptr);
    if (!screen) return 96;
    const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

UINT GetWindowDpiCompat(HWND window) {
    using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
    if (const HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFunction>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (getDpiForWindow) {
            const UINT dpi = getDpiForWindow(window);
            if (dpi != 0) return dpi;
        }
    }
    return GetSystemDpiCompat();
}

struct KeyDefinition {
    std::wstring label;
    std::uint16_t code = keypulse::kUnavailableKey;
    D2D1_RECT_F rectangle{};
};

class Application final {
public:
    explicit Application(HINSTANCE instance)
        : instance_(instance), store_(keypulse::DefaultStatisticsPath()) {}

    ~Application() {
        StopKeyboardHook();
        if (pickerFont_) {
            DeleteObject(pickerFont_);
        }
        if (largeIcon_) DestroyIcon(largeIcon_);
        if (smallIcon_) DestroyIcon(smallIcon_);
    }

    bool Initialize(int showCommand) {
        InitializeKeys();
        store_.Load();
        knownToday_ = keypulse::TodayLocalDay();
        customFirstDay_ = knownToday_ - 6;
        customLastDayInclusive_ = knownToday_;
        RefreshSnapshot();

        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     d2dFactory_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(
                                           dwriteFactory_.ReleaseAndGetAddressOf()))) ||
            !CreateTextFormats()) {
            return false;
        }
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(wicFactory_.ReleaseAndGetAddressOf())))) {
            return false;
        }
        largeIcon_ = CreateIconFromPngResource(IDR_KEYPRESS_100, true);
        smallIcon_ = CreateIconFromPngResource(IDR_KEYPRESS_50, true);

        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance_;
        windowClass.hIcon = largeIcon_ ? largeIcon_
                                       : LoadIconW(instance_, MAKEINTRESOURCEW(IDI_KEYPULSE));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kWindowClass;
        windowClass.hIconSm = smallIcon_ ? smallIcon_ : windowClass.hIcon;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        const UINT initialDpi = GetSystemDpiCompat();
        dpi_ = initialDpi;
        const int width = MulDiv(static_cast<int>(kCanvasWidth), static_cast<int>(initialDpi), 96);
        const int height = MulDiv(static_cast<int>(kNormalCanvasHeight), static_cast<int>(initialDpi), 96);
        const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        const DWORD windowStyle = WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX;
        window_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, kWindowTitle, windowStyle,
                                  x, y, width, height, nullptr, nullptr, instance_, this);
        if (!window_) {
            return false;
        }

        dpi_ = GetWindowDpiCompat(window_);
        RecalculateCanvasTransform();
        CreateDatePickers();
        PositionDatePickers();

        RAWINPUTDEVICE keyboard{};
        keyboard.usUsagePage = 0x01;
        keyboard.usUsage = 0x06;
        keyboard.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
        keyboard.hwndTarget = window_;
        rawInputAvailable_ = RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard)) != FALSE;
        hookAvailable_ = StartKeyboardHook();
        captureAvailable_ = rawInputAvailable_ || hookAvailable_;
        if (!captureAvailable_) {
            MessageBoxW(window_, L"无法初始化键盘采集。程序仍可打开，但不会记录按键。",
                        kWindowTitle, MB_OK | MB_ICONWARNING);
        } else if (!rawInputAvailable_) {
            MessageBoxW(window_, L"无法注册 Raw Input，将使用兼容采集模式。",
                        kWindowTitle, MB_OK | MB_ICONWARNING);
        } else if (!hookAvailable_) {
            MessageBoxW(window_,
                        L"系统组合键补偿初始化失败。普通按键仍会统计，但部分 Win 组合键可能遗漏。",
                        kWindowTitle, MB_OK | MB_ICONWARNING);
        }

        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        AddTrayIcon();
        SetTimer(window_, kTimerId, 1000, nullptr);
        lastSaveTick_ = GetTickCount64();

        ShowWindow(window_, showCommand);
        UpdateWindow(window_);
        return true;
    }

    HWND window() const noexcept { return window_; }

private:
    enum class Period : int { Today = 0, SevenDays = 1, ThirtyDays = 2, Custom = 3 };
    enum class Palette : int { SquareRoot = 0, Linear = 1, Logarithmic = 2, Quantile = 3 };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        Application* application = reinterpret_cast<Application*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            application = static_cast<Application*>(create->lpCreateParams);
            application->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(application));
        }
        if (application) {
            return application->HandleMessage(message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
            trayAdded_ = false;
            AddTrayIcon();
            return 0;
        }
        switch (message) {
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return HitTestNonClient(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        case WM_NCPAINT:
            return 0;
        case WM_NCACTIVATE:
            return TRUE;
        case WM_NCLBUTTONDBLCLK:
            if (wParam == HTCAPTION) return 0;
            return DefWindowProcW(window_, message, wParam, lParam);
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0u) == SC_MINIMIZE) {
                ShowWindow(window_, SW_HIDE);
                ShowTrayHintOnce();
                return 0;
            }
            if ((wParam & 0xFFF0u) == SC_MAXIMIZE) return 0;
            return DefWindowProcW(window_, message, wParam, lParam);
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window_, &paint);
            Render();
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (renderTarget_) {
                RECT client{};
                GetClientRect(window_, &client);
                renderTarget_->Resize(D2D1::SizeU(static_cast<UINT>(client.right),
                                                  static_cast<UINT>(client.bottom)));
            }
            RecalculateCanvasTransform();
            PositionDatePickers();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_SIZING:
            ConstrainWindowSizing(wParam, reinterpret_cast<RECT*>(lParam));
            return TRUE;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            DiscardDeviceResources();
            PositionDatePickers();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = MulDiv(static_cast<int>(kMinimumCanvasWidth),
                                            static_cast<int>(dpi_), 96);
            info->ptMinTrackSize.y = keypulse::WindowHeightForWidth(
                info->ptMinTrackSize.x, TitleBarHeightInPixels(),
                static_cast<int>(kCanvasWidth), CurrentContentDesignHeight());
            return 0;
        }
        case WM_INPUT:
            HandleRawInput(reinterpret_cast<HRAWINPUT>(lParam),
                           static_cast<DWORD>(GetMessageTime()));
            return DefWindowProcW(window_, message, wParam, lParam);
        case kHookKeyboardMessage:
            HandleHookInput(LOWORD(wParam), HIWORD(wParam) != 0,
                            static_cast<DWORD>(lParam));
            return 0;
        case WM_INPUT_DEVICE_CHANGE:
            if (wParam == GIDC_REMOVAL) {
                inputMerger_.Clear();
                KillTimer(window_, kHookFlushTimerId);
            }
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wParam == kTimerId) {
                HandleTimer();
            } else if (wParam == kHookFlushTimerId) {
                FlushHookInput();
            }
            return 0;
        case WM_LBUTTONDOWN:
            HandleClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE:
            HandleMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSELEAVE:
            mouseTracking_ = false;
            if (hoveredKey_ != -1 || hoveredPalette_ != -1 || minimizeHovered_ ||
                compactButtonHovered_) {
                hoveredKey_ = -1;
                hoveredPalette_ = -1;
                minimizeHovered_ = false;
                compactButtonHovered_ = false;
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && paletteMenuOpen_) {
                paletteMenuOpen_ = false;
                hoveredPalette_ = -1;
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            return DefWindowProcW(window_, message, wParam, lParam);
        case WM_NOTIFY:
            HandleNotify(reinterpret_cast<NMHDR*>(lParam));
            return 0;
        case WM_COMMAND:
            HandleCommand(LOWORD(wParam));
            return 0;
        case kTrayMessage:
            HandleTrayMessage(LOWORD(lParam));
            return 0;
        case WM_CLOSE:
            if (trayAdded_ && !exitRequested_) {
                ShowWindow(window_, SW_HIDE);
                ShowTrayHintOnce();
            } else {
                DestroyWindow(window_);
            }
            return 0;
        case WM_QUERYENDSESSION:
            store_.Save();
            return TRUE;
        case WM_ENDSESSION:
            if (wParam) store_.Save();
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kTimerId);
            KillTimer(window_, kHookFlushTimerId);
            StopKeyboardHook();
            store_.Save();
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    LRESULT HitTestNonClient(int screenX, int screenY) const {
        POINT point{screenX, screenY};
        ScreenToClient(window_, &point);
        RECT client{};
        GetClientRect(window_, &client);
        const int border = std::max(4, MulDiv(7, static_cast<int>(dpi_), 96));
        const bool left = point.x < border;
        const bool right = point.x >= client.right - border;
        const bool top = point.y < border;
        const bool bottom = point.y >= client.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;

        const auto [dipX, dipY] = ClientPixelsToDips(point.x, point.y);
        if (dipY >= 0.0f && dipY <= kTitleBarHeight) {
            if (Contains(MinimizeButtonRectangle(), dipX, dipY) ||
                Contains(CompactButtonRectangle(), dipX, dipY) ||
                Contains(HeaderStatusRectangle(), dipX, dipY)) {
                return HTCLIENT;
            }
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    int TitleBarHeightInPixels() const noexcept {
        return MulDiv(static_cast<int>(kTitleBarHeight), static_cast<int>(dpi_), 96);
    }

    int CurrentContentDesignHeight() const noexcept {
        return static_cast<int>(CurrentCanvasHeight() - kTitleBarHeight);
    }

    void ConstrainWindowSizing(WPARAM edge, RECT* rectangle) const noexcept {
        if (!rectangle) return;
        keypulse::PixelRectangle constrained{
            rectangle->left, rectangle->top, rectangle->right, rectangle->bottom};
        keypulse::ConstrainSizingRectangle(
            constrained, static_cast<keypulse::SizingEdge>(edge),
            TitleBarHeightInPixels(), static_cast<int>(kCanvasWidth),
            CurrentContentDesignHeight());
        rectangle->left = constrained.left;
        rectangle->top = constrained.top;
        rectangle->right = constrained.right;
        rectangle->bottom = constrained.bottom;
    }

    HRESULT CreatePngConverter(UINT resourceId, REFWICPixelFormatGUID pixelFormat,
                               IWICFormatConverter** result) {
        if (!result) return E_POINTER;
        *result = nullptr;
        const HRSRC resource = FindResourceW(instance_, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (!resource) return HRESULT_FROM_WIN32(GetLastError());
        const HGLOBAL loaded = LoadResource(instance_, resource);
        const DWORD size = SizeofResource(instance_, resource);
        auto* bytes = static_cast<BYTE*>(LockResource(loaded));
        if (!loaded || !bytes || size == 0) return E_FAIL;

        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        HRESULT value = wicFactory_->CreateStream(stream.GetAddressOf());
        if (SUCCEEDED(value)) value = stream->InitializeFromMemory(bytes, size);
        if (SUCCEEDED(value)) {
            value = wicFactory_->CreateDecoderFromStream(stream.Get(), nullptr,
                                                         WICDecodeMetadataCacheOnLoad,
                                                         decoder.GetAddressOf());
        }
        if (SUCCEEDED(value)) value = decoder->GetFrame(0, frame.GetAddressOf());
        if (SUCCEEDED(value)) value = wicFactory_->CreateFormatConverter(converter.GetAddressOf());
        if (SUCCEEDED(value)) {
            value = converter->Initialize(frame.Get(), pixelFormat, WICBitmapDitherTypeNone,
                                          nullptr, 0.0, WICBitmapPaletteTypeCustom);
        }
        if (SUCCEEDED(value)) *result = converter.Detach();
        return value;
    }

    HICON CreateIconFromPngResource(UINT resourceId, bool invertRgb) {
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(CreatePngConverter(resourceId, GUID_WICPixelFormat32bppBGRA,
                                      converter.GetAddressOf()))) {
            return nullptr;
        }
        UINT width = 0;
        UINT height = 0;
        if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) return nullptr;

        BITMAPV5HEADER header{};
        header.bV5Size = sizeof(header);
        header.bV5Width = static_cast<LONG>(width);
        header.bV5Height = -static_cast<LONG>(height);
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5RedMask = 0x00FF0000;
        header.bV5GreenMask = 0x0000FF00;
        header.bV5BlueMask = 0x000000FF;
        header.bV5AlphaMask = 0xFF000000;

        void* pixels = nullptr;
        const HDC screen = GetDC(nullptr);
        const HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                               DIB_RGB_COLORS, &pixels, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!color || !pixels) {
            if (color) DeleteObject(color);
            return nullptr;
        }

        const UINT stride = width * 4;
        if (FAILED(converter->CopyPixels(nullptr, stride, stride * height,
                                         static_cast<BYTE*>(pixels)))) {
            DeleteObject(color);
            return nullptr;
        }
        if (invertRgb) {
            auto* bgra = static_cast<BYTE*>(pixels);
            for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height; ++pixel) {
                BYTE* channel = bgra + pixel * 4;
                if (channel[3] != 0) {
                    channel[0] = static_cast<BYTE>(255u - channel[0]);
                    channel[1] = static_cast<BYTE>(255u - channel[1]);
                    channel[2] = static_cast<BYTE>(255u - channel[2]);
                }
            }
        }
        const std::size_t maskStride = ((static_cast<std::size_t>(width) + 15u) / 16u) * 2u;
        const std::vector<BYTE> maskPixels(maskStride * height, 0);
        const HBITMAP mask = CreateBitmap(static_cast<int>(width), static_cast<int>(height),
                                          1, 1, maskPixels.data());
        if (!mask) {
            DeleteObject(color);
            return nullptr;
        }

        ICONINFO information{};
        information.fIcon = TRUE;
        information.hbmColor = color;
        information.hbmMask = mask;
        const HICON icon = CreateIconIndirect(&information);
        DeleteObject(mask);
        DeleteObject(color);
        return icon;
    }

    bool CreateTextFormats() {
        auto create = [this](float size, DWRITE_FONT_WEIGHT weight,
                             DWRITE_TEXT_ALIGNMENT alignment,
                             ComPtr<IDWriteTextFormat>& target) {
            if (FAILED(dwriteFactory_->CreateTextFormat(
                    L"Microsoft YaHei UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", target.ReleaseAndGetAddressOf()))) {
                return false;
            }
            target->SetTextAlignment(alignment);
            target->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            return true;
        };

        return create(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, titleFormat_) &&
               create(16.0f, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_LEADING, sectionFormat_) &&
               create(48.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, totalFormat_) &&
               create(15.0f, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_CENTER, tabFormat_) &&
               create(11.5f, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_CENTER, keyLabelFormat_) &&
               create(13.0f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER, keyCountFormat_) &&
               create(12.5f, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_LEADING, smallFormat_) &&
               create(12.5f, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_CENTER, smallCenterFormat_);
    }

    bool EnsureDeviceResources() {
        if (renderTarget_) return true;
        RECT client{};
        GetClientRect(window_, &client);
        const D2D1_SIZE_U size = D2D1::SizeU(std::max(1L, client.right), std::max(1L, client.bottom));
        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(),
            static_cast<float>(dpi_), static_cast<float>(dpi_));
        if (FAILED(d2dFactory_->CreateHwndRenderTarget(
                properties, D2D1::HwndRenderTargetProperties(window_, size),
                renderTarget_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        if (FAILED(renderTarget_->CreateSolidColorBrush(Rgb(0x000000),
                                                         brush_.ReleaseAndGetAddressOf()))) {
            renderTarget_.Reset();
            return false;
        }
        RecalculateCanvasTransform();
        return true;
    }

    void DiscardDeviceResources() {
        brush_.Reset();
        renderTarget_.Reset();
    }

    float CurrentCanvasHeight() const noexcept {
        return compactMode_ ? kCompactCanvasHeight : kNormalCanvasHeight;
    }

    float ClientWidthInDips() const noexcept {
        if (!window_) return kCanvasWidth;
        RECT client{};
        GetClientRect(window_, &client);
        return static_cast<float>(client.right) * 96.0f / static_cast<float>(dpi_);
    }

    void RecalculateCanvasTransform() {
        if (!window_) return;
        RECT client{};
        GetClientRect(window_, &client);
        const float widthInDips = static_cast<float>(client.right) * 96.0f / static_cast<float>(dpi_);
        const float heightInDips = static_cast<float>(client.bottom) * 96.0f / static_cast<float>(dpi_);
        const float contentHeight = CurrentCanvasHeight() - kTitleBarHeight;
        const float availableContentHeight = std::max(0.0f, heightInDips - kTitleBarHeight);
        canvasScale_ = std::max(0.01f, std::min(widthInDips / kCanvasWidth,
                                               availableContentHeight / contentHeight));
        canvasOffsetX_ = (widthInDips - kCanvasWidth * canvasScale_) * 0.5f;
        canvasOffsetY_ = kTitleBarHeight +
            (availableContentHeight - contentHeight * canvasScale_) * 0.5f -
            kTitleBarHeight * canvasScale_;
    }

    void SetBrush(D2D1_COLOR_F color) { brush_->SetColor(color); }

    void FillRounded(const D2D1_RECT_F& rectangle, float radius, D2D1_COLOR_F color) {
        SetBrush(color);
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rectangle, radius, radius), brush_.Get());
    }

    void StrokeRounded(const D2D1_RECT_F& rectangle, float radius, D2D1_COLOR_F color,
                       float width = 1.0f) {
        SetBrush(color);
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(rectangle, radius, radius),
                                            brush_.Get(), width);
    }

    void DrawText(const std::wstring& text, const D2D1_RECT_F& rectangle,
                  IDWriteTextFormat* format, D2D1_COLOR_F color) {
        SetBrush(color);
        renderTarget_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
                                 rectangle, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawPanel(const D2D1_RECT_F& rectangle) {
        auto shadow = rectangle;
        shadow.top += 2.0f;
        shadow.bottom += 2.0f;
        FillRounded(shadow, 10.0f, Rgb(0xDDE1E6, 0.42f));
        FillRounded(rectangle, 10.0f, Rgb(0xFFFFFF));
        StrokeRounded(rectangle, 10.0f, Rgb(0xDDE1E6));
    }

    static const wchar_t* PaletteName(Palette palette) {
        switch (palette) {
        case Palette::SquareRoot: return L"平方根色阶";
        case Palette::Linear: return L"线性色阶";
        case Palette::Logarithmic: return L"对数色阶";
        case Palette::Quantile: return L"分位数色阶";
        }
        return L"平方根色阶";
    }

    D2D1_COLOR_F HeatColor(std::uint64_t count, std::uint64_t maximum,
                           const std::vector<std::uint64_t>& sortedCounts) const {
        if (count == 0 || maximum == 0) return Rgb(0xFAFBFC);
        float normalized = 0.0f;
        const float ratio = static_cast<float>(count) / static_cast<float>(maximum);
        switch (palette_) {
        case Palette::SquareRoot:
            normalized = std::sqrt(ratio);
            break;
        case Palette::Linear:
            normalized = ratio;
            break;
        case Palette::Logarithmic:
            normalized = std::log1p(static_cast<float>(count)) /
                         std::log1p(static_cast<float>(maximum));
            break;
        case Palette::Quantile:
            if (sortedCounts.size() <= 1) {
                normalized = 1.0f;
            } else {
                const auto first = std::lower_bound(sortedCounts.begin(), sortedCounts.end(), count);
                const auto last = std::upper_bound(sortedCounts.begin(), sortedCounts.end(), count);
                const float midpoint = (static_cast<float>(first - sortedCounts.begin()) +
                                        static_cast<float>(last - sortedCounts.begin() - 1)) * 0.5f;
                normalized = midpoint / static_cast<float>(sortedCounts.size() - 1);
            }
            break;
        }
        const auto blue = Rgb(0xA9D0F4);
        const auto yellow = Rgb(0xFFE09A);
        const auto orange = Rgb(0xFF9448);
        return normalized < 0.55f ? Mix(blue, yellow, normalized / 0.55f)
                                  : Mix(yellow, orange, (normalized - 0.55f) / 0.45f);
    }

    void Render() {
        if (!EnsureDeviceResources()) return;

        RecalculateCanvasTransform();
        renderTarget_->BeginDraw();
        renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
        renderTarget_->Clear(Rgb(0xF4F6F8));
        DrawHeader();
        renderTarget_->SetTransform(
            D2D1::Matrix3x2F::Scale(canvasScale_, canvasScale_) *
            D2D1::Matrix3x2F::Translation(canvasOffsetX_, canvasOffsetY_));

        if (!compactMode_) DrawSummary();
        DrawKeyboard();
        if (!compactMode_) DrawFooter();
        if (!compactMode_ && paletteMenuOpen_) DrawPaletteMenu();

        const HRESULT result = renderTarget_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            DiscardDeviceResources();
        }
    }

    void DrawHeader() {
        const float width = ClientWidthInDips();
        FillRounded(D2D1::RectF(0, 0, width, kTitleBarHeight), 0, Rgb(0xFFFFFF));
        SetBrush(Rgb(0xE2E5E9));
        renderTarget_->DrawLine(D2D1::Point2F(0, kTitleBarHeight - 0.5f),
                                D2D1::Point2F(width, kTitleBarHeight - 0.5f),
                                brush_.Get(), 1.0f);
        DrawText(L"键频", D2D1::RectF(18, 0, 150, kTitleBarHeight),
                 titleFormat_.Get(), Rgb(0x24272B));

        const bool active = captureAvailable_ && !paused_;
        const auto status = HeaderStatusRectangle();
        FillRounded(D2D1::RectF(status.left + 8, 13, status.left + 20, 25), 6,
                    active ? Rgb(0x43C463) : Rgb(0xF3A340));
        DrawText(active ? L"正在统计" : (captureAvailable_ ? L"已暂停" : L"采集不可用"),
                 D2D1::RectF(status.left + 30, 0, status.right, kTitleBarHeight),
                 smallFormat_.Get(), Rgb(0x5D636B));

        const auto compact = CompactButtonRectangle();
        if (compactButtonHovered_) {
            FillRounded(compact, 0, Rgb(0xE4E8EC));
            SetBrush(Rgb(0xC8CFD6));
            renderTarget_->DrawLine(D2D1::Point2F(compact.left, compact.bottom - 0.5f),
                                    D2D1::Point2F(compact.right, compact.bottom - 0.5f),
                                    brush_.Get(), 1.0f);
            renderTarget_->DrawLine(D2D1::Point2F(compact.left + 0.5f, compact.top + 6),
                                    D2D1::Point2F(compact.left + 0.5f, compact.bottom - 6),
                                    brush_.Get(), 1.0f);
        }
        DrawText(compactMode_ ? L"▼" : L"▲", compact,
                 smallCenterFormat_.Get(), Rgb(0x555C63));

        const auto minimize = MinimizeButtonRectangle();
        if (minimizeHovered_) {
            FillRounded(minimize, 0, Rgb(0xE4E8EC));
            SetBrush(Rgb(0xC8CFD6));
            renderTarget_->DrawLine(D2D1::Point2F(minimize.left, minimize.bottom - 0.5f),
                                    D2D1::Point2F(minimize.right, minimize.bottom - 0.5f),
                                    brush_.Get(), 1.0f);
            renderTarget_->DrawLine(D2D1::Point2F(minimize.left + 0.5f, minimize.top + 7),
                                    D2D1::Point2F(minimize.left + 0.5f, minimize.bottom - 7),
                                    brush_.Get(), 1.0f);
        }
        SetBrush(Rgb(0x555C63));
        renderTarget_->DrawLine(D2D1::Point2F(minimize.left + 16, kTitleBarHeight * 0.5f),
                                D2D1::Point2F(minimize.right - 16, kTitleBarHeight * 0.5f),
                                brush_.Get(), 1.2f);
    }

    void DrawSummary() {
        DrawText(L"按键总次数", D2D1::RectF(44, 56, 350, 89), sectionFormat_.Get(), Rgb(0x35393E));
        DrawText(FormatCount(snapshot_.total), D2D1::RectF(42, 82, 600, 158),
                 totalFormat_.Get(), Rgb(0x2F3338));

        static constexpr std::array<const wchar_t*, 4> labels{L"今天", L"7 天", L"30 天", L"自定义"};
        for (int index = 0; index < 4; ++index) {
            const auto rectangle = TabRectangle(index);
            if (index == static_cast<int>(period_)) {
                FillRounded(rectangle, 6.0f, Rgb(0xEEF5FC));
                StrokeRounded(rectangle, 6.0f, Rgb(0xAFCBE5));
            } else {
                FillRounded(rectangle, 6.0f, Rgb(0xFFFFFF));
                StrokeRounded(rectangle, 6.0f, Rgb(0xD9DDE2));
            }
            DrawText(labels[static_cast<std::size_t>(index)], rectangle, tabFormat_.Get(),
                     index == static_cast<int>(period_) ? Rgb(0x2D536F) : Rgb(0x555B62));
        }

        const auto rangeRectangle = D2D1::RectF(797, 112, 1376, 162);
        FillRounded(rangeRectangle, 7.0f, Rgb(0xFBFCFD));
        StrokeRounded(rangeRectangle, 7.0f, Rgb(0xDEE2E6));
        DrawCalendarGlyph(D2D1::RectF(815, 126, 833, 145));
        if (period_ == Period::Custom) {
            DrawText(L"—", D2D1::RectF(1054, 113, 1080, 157), smallCenterFormat_.Get(), Rgb(0x858B92));
        } else {
            const auto [firstDay, lastDay] = SelectedRange();
            std::wstring range = keypulse::FormatLocalDay(firstDay) + L" 00:00    —    " +
                                 keypulse::FormatLocalDay(lastDay) + L" 00:00";
            DrawText(range, D2D1::RectF(850, 113, 1360, 159), smallFormat_.Get(), Rgb(0x666C73));
        }
    }

    void DrawCalendarGlyph(const D2D1_RECT_F& rectangle) {
        StrokeRounded(rectangle, 2.0f, Rgb(0x737A82), 1.3f);
        SetBrush(Rgb(0x737A82));
        renderTarget_->DrawLine(D2D1::Point2F(rectangle.left, rectangle.top + 5),
                                D2D1::Point2F(rectangle.right, rectangle.top + 5), brush_.Get(), 1.2f);
        renderTarget_->DrawLine(D2D1::Point2F(rectangle.left + 4, rectangle.top - 2),
                                D2D1::Point2F(rectangle.left + 4, rectangle.top + 4), brush_.Get(), 1.5f);
        renderTarget_->DrawLine(D2D1::Point2F(rectangle.right - 4, rectangle.top - 2),
                                D2D1::Point2F(rectangle.right - 4, rectangle.top + 4), brush_.Get(), 1.5f);
    }

    D2D1_RECT_F DisplayedKeyRectangle(const KeyDefinition& key) const {
        auto rectangle = key.rectangle;
        if (compactMode_) {
            rectangle.top -= 124.0f;
            rectangle.bottom -= 124.0f;
        }
        return rectangle;
    }

    void DrawKeyboard() {
        const float offsetY = compactMode_ ? -124.0f : 0.0f;
        auto panel = D2D1::RectF(24, 178, 1376, 569);
        panel.top += offsetY;
        panel.bottom += offsetY;
        DrawPanel(panel);

        std::uint64_t maximum = 0;
        std::vector<std::uint64_t> sortedCounts;
        sortedCounts.reserve(keys_.size());
        for (const auto& key : keys_) {
            if (key.code < keypulse::kKeySlotCount) {
                const auto count = snapshot_.counts[key.code];
                maximum = std::max(maximum, count);
                if (count != 0) sortedCounts.push_back(count);
            }
        }
        std::sort(sortedCounts.begin(), sortedCounts.end());

        for (std::size_t index = 0; index < keys_.size(); ++index) {
            const auto& key = keys_[index];
            const std::uint64_t count = key.code < keypulse::kKeySlotCount
                                            ? snapshot_.counts[key.code] : 0;
            const auto rectangle = DisplayedKeyRectangle(key);
            FillRounded(rectangle, 5.0f, HeatColor(count, maximum, sortedCounts));
            StrokeRounded(rectangle, 5.0f,
                          static_cast<int>(index) == hoveredKey_ ? Rgb(0xE88B36) : Rgb(0xD5DAE0),
                          static_cast<int>(index) == hoveredKey_ ? 1.8f : 1.0f);

            const float height = rectangle.bottom - rectangle.top;
            const auto labelRectangle = D2D1::RectF(rectangle.left + 2, rectangle.top + 3,
                                                     rectangle.right - 2,
                                                     rectangle.top + height * 0.53f);
            const auto countRectangle = D2D1::RectF(rectangle.left + 2,
                                                     rectangle.top + height * 0.47f,
                                                     rectangle.right - 2, rectangle.bottom - 2);
            DrawText(key.label, labelRectangle, keyLabelFormat_.Get(), Rgb(0x33383E));
            DrawText(FormatCount(count), countRectangle, keyCountFormat_.Get(), Rgb(0x252A2F));
        }
    }

    void DrawFooter() {
        DrawText(L"低频", D2D1::RectF(48, 577, 98, 641), smallFormat_.Get(), Rgb(0x555B62));

        const std::array<D2D1_COLOR_F, 7> legend{
            Rgb(0xA9D0F4), Rgb(0xC0D9F0), Rgb(0xD8E4EA), Rgb(0xF5F4E9),
            Rgb(0xFFE09A), Rgb(0xFFC875), Rgb(0xFF9448)};
        for (std::size_t index = 0; index < legend.size(); ++index) {
            FillRounded(D2D1::RectF(100.0f + static_cast<float>(index) * 27.0f, 594,
                                    123.0f + static_cast<float>(index) * 27.0f, 617),
                        3.0f, legend[index]);
        }
        DrawText(L"高频", D2D1::RectF(296, 577, 350, 641), smallFormat_.Get(), Rgb(0x555B62));

        const auto paletteButton = PaletteButtonRectangle();
        FillRounded(paletteButton, 5.0f, paletteMenuOpen_ ? Rgb(0xEEF5FC) : Rgb(0xFFFFFF));
        StrokeRounded(paletteButton, 5.0f,
                      paletteMenuOpen_ ? Rgb(0x9FC2E2) : Rgb(0xD5DAE0));
        DrawText(PaletteName(palette_),
                 D2D1::RectF(paletteButton.left + 13, paletteButton.top,
                             paletteButton.right - 32, paletteButton.bottom),
                 smallFormat_.Get(), Rgb(0x4C535A));
        SetBrush(Rgb(0x6F767D));
        renderTarget_->DrawLine(D2D1::Point2F(paletteButton.right - 20, paletteButton.top + 17),
                                D2D1::Point2F(paletteButton.right - 15, paletteButton.top + 22),
                                brush_.Get(), 1.3f);
        renderTarget_->DrawLine(D2D1::Point2F(paletteButton.right - 15, paletteButton.top + 22),
                                D2D1::Point2F(paletteButton.right - 10, paletteButton.top + 17),
                                brush_.Get(), 1.3f);

        const bool saveOk = store_.last_error().empty();
        FillRounded(D2D1::RectF(1120, 599, 1132, 611), 6,
                    saveOk ? Rgb(0x43C463) : Rgb(0xE75C55));
        std::wstring status;
        if (!saveOk) {
            status = store_.last_error();
        } else if (store_.dirty()) {
            status = L"有新数据，等待安全写入";
        } else {
            status = L"数据已安全写入 · " + std::to_wstring(store_.day_count()) + L" 天";
        }
        DrawText(status, D2D1::RectF(1141, 578, 1376, 635), smallFormat_.Get(), Rgb(0x6D747C));
    }

    D2D1_RECT_F HeaderStatusRectangle() const {
        const float width = ClientWidthInDips();
        return D2D1::RectF(width - 262.0f, 0, width - 92.0f, kTitleBarHeight);
    }

    D2D1_RECT_F CompactButtonRectangle() const {
        const float width = ClientWidthInDips();
        return D2D1::RectF(width - 84.0f, 0, width - 42.0f, kTitleBarHeight);
    }

    D2D1_RECT_F MinimizeButtonRectangle() const {
        const float width = ClientWidthInDips();
        return D2D1::RectF(width - 42.0f, 0, width, kTitleBarHeight);
    }

    D2D1_RECT_F PaletteButtonRectangle() const {
        return D2D1::RectF(402, 584, 580, 625);
    }

    D2D1_RECT_F PaletteItemRectangle(int index) const {
        const float top = 420.0f + static_cast<float>(index) * 39.0f;
        return D2D1::RectF(402, top, 580, top + 39.0f);
    }

    void DrawPaletteMenu() {
        const auto menu = D2D1::RectF(402, 419, 580, 577);
        auto shadow = menu;
        shadow.left += 2.0f;
        shadow.top += 3.0f;
        shadow.right += 2.0f;
        shadow.bottom += 3.0f;
        FillRounded(shadow, 6.0f, Rgb(0xB9C0C8, 0.42f));
        FillRounded(menu, 6.0f, Rgb(0xFFFFFF));
        StrokeRounded(menu, 6.0f, Rgb(0xCDD3D9));

        for (int index = 0; index < 4; ++index) {
            const auto item = PaletteItemRectangle(index);
            if (index == hoveredPalette_) {
                FillRounded(item, 3.0f, Rgb(0xF1F5F8));
            } else if (index == static_cast<int>(palette_)) {
                FillRounded(item, 3.0f, Rgb(0xEEF5FC));
            }
            DrawText(PaletteName(static_cast<Palette>(index)),
                     D2D1::RectF(item.left + 13, item.top, item.right - 10, item.bottom),
                     smallFormat_.Get(), Rgb(0x42484F));
            if (index != 3) {
                SetBrush(Rgb(0xEDF0F3));
                renderTarget_->DrawLine(D2D1::Point2F(item.left + 8, item.bottom),
                                        D2D1::Point2F(item.right - 8, item.bottom),
                                        brush_.Get(), 1.0f);
            }
        }
    }

    D2D1_RECT_F TabRectangle(int index) const {
        constexpr float left = 797.0f;
        constexpr float width = 144.75f;
        return D2D1::RectF(left + width * static_cast<float>(index), 60,
                           left + width * static_cast<float>(index + 1), 101);
    }

    std::pair<std::int32_t, std::int32_t> SelectedRange() const {
        const auto today = keypulse::TodayLocalDay();
        switch (period_) {
        case Period::Today: return {today, today + 1};
        case Period::SevenDays: return {today - 6, today + 1};
        case Period::ThirtyDays: return {today - 29, today + 1};
        case Period::Custom: return {customFirstDay_, customLastDayInclusive_ + 1};
        }
        return {today, today + 1};
    }

    void RefreshSnapshot() {
        const auto [firstDay, lastDay] = SelectedRange();
        snapshot_ = store_.Query(firstDay, lastDay);
    }

    bool StartKeyboardHook() {
        hookContext_.instance = instance_;
        hookContext_.targetWindow = window_;
        hookContext_.readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!hookContext_.readyEvent) return false;

        hookThread_ = CreateThread(nullptr, 0, KeyboardHookThread, &hookContext_, 0, nullptr);
        if (!hookThread_) {
            CloseHandle(hookContext_.readyEvent);
            hookContext_.readyEvent = nullptr;
            return false;
        }
        const DWORD waitResult = WaitForSingleObject(hookContext_.readyEvent, 5'000);
        CloseHandle(hookContext_.readyEvent);
        hookContext_.readyEvent = nullptr;
        if (waitResult == WAIT_OBJECT_0 && hookContext_.hook) return true;

        if (hookContext_.threadId != 0) {
            PostThreadMessageW(hookContext_.threadId, WM_QUIT, 0, 0);
        }
        WaitForSingleObject(hookThread_, 2'000);
        CloseHandle(hookThread_);
        hookThread_ = nullptr;
        return false;
    }

    void StopKeyboardHook() {
        if (!hookThread_) return;
        if (hookContext_.threadId != 0) {
            PostThreadMessageW(hookContext_.threadId, WM_QUIT, 0, 0);
        }
        WaitForSingleObject(hookThread_, 2'000);
        CloseHandle(hookThread_);
        hookThread_ = nullptr;
        hookAvailable_ = false;
    }

    void RecordKeyPress(std::uint16_t keyCode) {
        if (paused_ || !captureAvailable_ || keyCode >= keypulse::kKeySlotCount) return;
        const auto today = keypulse::TodayLocalDay();
        store_.Increment(keyCode, today);
        const auto [firstDay, lastDay] = SelectedRange();
        if (today >= firstDay && today < lastDay) {
            ++snapshot_.counts[keyCode];
            ++snapshot_.total;
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void HandleRawInput(HRAWINPUT handle, DWORD messageTime) {
        UINT size = 0;
        if (GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
            return;
        }
        std::vector<std::uint8_t> buffer(size);
        if (GetRawInputData(handle, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) {
            return;
        }
        const auto* input = reinterpret_cast<const RAWINPUT*>(buffer.data());
        if (input->header.dwType != RIM_TYPEKEYBOARD || paused_ || !rawInputAvailable_) return;

        const RAWKEYBOARD& keyboard = input->data.keyboard;
        if (keyboard.VKey == 0xFF || keyboard.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE) {
            return;
        }

        const auto keyCode = NormalizeKeyCode(
            keyboard.VKey, keyboard.MakeCode, (keyboard.Flags & RI_KEY_E0) != 0,
            (keyboard.Flags & RI_KEY_E1) != 0);
        if (!keyCode) return;

        const bool keyDown = (keyboard.Flags & RI_KEY_BREAK) == 0;
        const bool shouldCount = inputMerger_.ObserveRaw(
            *keyCode, keyDown, messageTime, GetTickCount64());
        if (hookAvailable_ && keyDown) {
            if (!inputMerger_.has_pending()) KillTimer(window_, kHookFlushTimerId);
        }
        if (shouldCount) RecordKeyPress(*keyCode);
    }

    void HandleHookInput(std::uint16_t keyCode, bool keyDown, DWORD messageTime) {
        if (paused_ || !hookAvailable_) return;
        const DWORD age = GetTickCount() - messageTime;
        if (keyDown && age > 500) return;
        if (inputMerger_.ObserveHook(keyCode, keyDown, messageTime, GetTickCount64())) {
            SetTimer(window_, kHookFlushTimerId, 25, nullptr);
        }
    }

    void FlushHookInput() {
        if (paused_ || !hookAvailable_) {
            inputMerger_.Clear();
            KillTimer(window_, kHookFlushTimerId);
            return;
        }
        for (const std::uint16_t keyCode : inputMerger_.TakeReady(GetTickCount64())) {
            RecordKeyPress(keyCode);
        }
        if (!inputMerger_.has_pending()) KillTimer(window_, kHookFlushTimerId);
    }

    void TogglePaused() {
        paused_ = !paused_;
        inputMerger_.Clear();
        KillTimer(window_, kHookFlushTimerId);
        UpdateTrayTooltip();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void HandleTimer() {
        const auto today = keypulse::TodayLocalDay();
        if (today != knownToday_) {
            knownToday_ = today;
            RefreshSnapshot();
        }

        const ULONGLONG now = GetTickCount64();
        if (store_.dirty() && now - lastSaveTick_ >= 5ull * 60ull * 1000ull) {
            store_.Save();
            lastSaveTick_ = now;
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ToggleCompactMode() {
        compactMode_ = !compactMode_;
        paletteMenuOpen_ = false;
        hoveredPalette_ = -1;
        hoveredKey_ = -1;
        compactButtonHovered_ = false;

        RECT windowRectangle{};
        GetWindowRect(window_, &windowRectangle);
        const int windowWidth = windowRectangle.right - windowRectangle.left;
        const int targetHeight = keypulse::WindowHeightForWidth(
            windowWidth, TitleBarHeightInPixels(), static_cast<int>(kCanvasWidth),
            CurrentContentDesignHeight());
        SetWindowPos(window_, nullptr, 0, 0,
                     windowWidth, targetHeight,
                     SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
        RecalculateCanvasTransform();
        PositionDatePickers();
        InvalidateRect(window_, nullptr, FALSE);
    }

    std::pair<float, float> ClientPixelsToCanvas(int x, int y) const {
        const auto [dipX, dipY] = ClientPixelsToDips(x, y);
        return {(dipX - canvasOffsetX_) / canvasScale_, (dipY - canvasOffsetY_) / canvasScale_};
    }

    std::pair<float, float> ClientPixelsToDips(int x, int y) const noexcept {
        return {static_cast<float>(x) * 96.0f / static_cast<float>(dpi_),
                static_cast<float>(y) * 96.0f / static_cast<float>(dpi_)};
    }

    void HandleClick(int x, int y) {
        const auto [dipX, dipY] = ClientPixelsToDips(x, y);
        if (dipY >= 0.0f && dipY <= kTitleBarHeight) {
            if (Contains(CompactButtonRectangle(), dipX, dipY)) {
                ToggleCompactMode();
                return;
            }
            if (Contains(MinimizeButtonRectangle(), dipX, dipY)) {
                paletteMenuOpen_ = false;
                ShowWindow(window_, SW_HIDE);
                ShowTrayHintOnce();
                return;
            }
            if (Contains(HeaderStatusRectangle(), dipX, dipY)) {
                TogglePaused();
            }
            return;
        }

        const auto [canvasX, canvasY] = ClientPixelsToCanvas(x, y);
        if (Contains(PaletteButtonRectangle(), canvasX, canvasY)) {
            paletteMenuOpen_ = !paletteMenuOpen_;
            hoveredPalette_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (paletteMenuOpen_) {
            for (int index = 0; index < 4; ++index) {
                if (Contains(PaletteItemRectangle(index), canvasX, canvasY)) {
                    palette_ = static_cast<Palette>(index);
                    paletteMenuOpen_ = false;
                    hoveredPalette_ = -1;
                    InvalidateRect(window_, nullptr, FALSE);
                    return;
                }
            }
            paletteMenuOpen_ = false;
            hoveredPalette_ = -1;
        }
        for (int index = 0; index < 4; ++index) {
            if (Contains(TabRectangle(index), canvasX, canvasY)) {
                period_ = static_cast<Period>(index);
                RefreshSnapshot();
                PositionDatePickers();
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
        }
    }

    void HandleMouseMove(int x, int y) {
        if (!mouseTracking_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
            TrackMouseEvent(&tracking);
            mouseTracking_ = true;
        }
        const auto [dipX, dipY] = ClientPixelsToDips(x, y);
        const auto [canvasX, canvasY] = ClientPixelsToCanvas(x, y);
        const bool inTitleBar = dipY >= 0.0f && dipY <= kTitleBarHeight;
        const bool minimizeHovered = inTitleBar &&
            Contains(MinimizeButtonRectangle(), dipX, dipY);
        const bool compactButtonHovered = inTitleBar &&
            Contains(CompactButtonRectangle(), dipX, dipY);
        int paletteHovered = -1;
        if (!inTitleBar && paletteMenuOpen_) {
            for (int index = 0; index < 4; ++index) {
                if (Contains(PaletteItemRectangle(index), canvasX, canvasY)) {
                    paletteHovered = index;
                    break;
                }
            }
        }
        int hovered = -1;
        if (!inTitleBar && paletteHovered == -1) {
            for (std::size_t index = 0; index < keys_.size(); ++index) {
                if (Contains(DisplayedKeyRectangle(keys_[index]), canvasX, canvasY)) {
                    hovered = static_cast<int>(index);
                    break;
                }
            }
        }
        if (hovered != hoveredKey_ || paletteHovered != hoveredPalette_ ||
            minimizeHovered != minimizeHovered_ ||
            compactButtonHovered != compactButtonHovered_) {
            hoveredKey_ = hovered;
            hoveredPalette_ = paletteHovered;
            minimizeHovered_ = minimizeHovered;
            compactButtonHovered_ = compactButtonHovered;
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void CreateDatePickers() {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_DATE_CLASSES};
        InitCommonControlsEx(&controls);
        const DWORD style = WS_CHILD | WS_TABSTOP | DTS_SHORTDATEFORMAT;
        firstDatePicker_ = CreateWindowExW(0, DATETIMEPICK_CLASSW, nullptr, style,
                                           0, 0, 100, 30, window_, nullptr, instance_, nullptr);
        lastDatePicker_ = CreateWindowExW(0, DATETIMEPICK_CLASSW, nullptr, style,
                                          0, 0, 100, 30, window_, nullptr, instance_, nullptr);
        pickerFont_ = CreateFontW(-MulDiv(13, static_cast<int>(dpi_), 96), 0, 0, 0, FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                  L"Microsoft YaHei UI");
        for (HWND picker : {firstDatePicker_, lastDatePicker_}) {
            SendMessageW(picker, WM_SETFONT, reinterpret_cast<WPARAM>(pickerFont_), TRUE);
            DateTime_SetFormat(picker, L"yyyy-MM-dd");
        }
        SetPickerDates();
    }

    void SetPickerDates() {
        updatingPickers_ = true;
        auto first = SystemTimeForDay(customFirstDay_);
        auto last = SystemTimeForDay(customLastDayInclusive_);
        DateTime_SetSystemtime(firstDatePicker_, GDT_VALID, &first);
        DateTime_SetSystemtime(lastDatePicker_, GDT_VALID, &last);
        updatingPickers_ = false;
    }

    void PositionDatePickers() {
        if (!firstDatePicker_ || !lastDatePicker_) return;
        const bool visible = !compactMode_ && period_ == Period::Custom;
        const auto position = [this, visible](HWND picker, const D2D1_RECT_F& rectangle) {
            const float dipLeft = canvasOffsetX_ + rectangle.left * canvasScale_;
            const float dipTop = canvasOffsetY_ + rectangle.top * canvasScale_;
            const float dipWidth = (rectangle.right - rectangle.left) * canvasScale_;
            const float dipHeight = (rectangle.bottom - rectangle.top) * canvasScale_;
            SetWindowPos(picker, HWND_TOP,
                         static_cast<int>(std::lround(dipLeft * dpi_ / 96.0f)),
                         static_cast<int>(std::lround(dipTop * dpi_ / 96.0f)),
                         static_cast<int>(std::lround(dipWidth * dpi_ / 96.0f)),
                         static_cast<int>(std::lround(dipHeight * dpi_ / 96.0f)),
                         SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        };
        position(firstDatePicker_, D2D1::RectF(846, 121, 1049, 153));
        position(lastDatePicker_, D2D1::RectF(1080, 121, 1355, 153));
    }

    void HandleNotify(NMHDR* header) {
        if (!header || header->code != DTN_DATETIMECHANGE || updatingPickers_ ||
            (header->hwndFrom != firstDatePicker_ && header->hwndFrom != lastDatePicker_)) {
            return;
        }
        const auto* change = reinterpret_cast<const NMDATETIMECHANGE*>(header);
        if (change->dwFlags != GDT_VALID) return;
        const auto selectedDay = DayForSystemTime(change->st);
        if (header->hwndFrom == firstDatePicker_) {
            customFirstDay_ = selectedDay;
            if (customLastDayInclusive_ < customFirstDay_) {
                customLastDayInclusive_ = customFirstDay_;
                SetPickerDates();
            }
        } else {
            customLastDayInclusive_ = selectedDay;
            if (customFirstDay_ > customLastDayInclusive_) {
                customFirstDay_ = customLastDayInclusive_;
                SetPickerDates();
            }
        }
        RefreshSnapshot();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void AddTrayIcon() {
        if (!window_) return;
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window_;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon = smallIcon_ ? smallIcon_
                                : LoadIconW(instance_, MAKEINTRESOURCEW(IDI_KEYPULSE));
        wcscpy_s(data.szTip, paused_ ? L"KeyPulse · 已暂停" : L"KeyPulse · 正在统计");
        trayAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (trayAdded_) {
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }
    }

    void RemoveTrayIcon() {
        if (!trayAdded_) return;
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window_;
        data.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayAdded_ = false;
    }

    void UpdateTrayTooltip() {
        if (!trayAdded_) return;
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window_;
        data.uID = kTrayIconId;
        data.uFlags = NIF_TIP;
        wcscpy_s(data.szTip, paused_ ? L"KeyPulse · 已暂停" : L"KeyPulse · 正在统计");
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void ShowTrayHintOnce() {
        if (trayHintShown_ || !trayAdded_) return;
        trayHintShown_ = true;
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window_;
        data.uID = kTrayIconId;
        data.uFlags = NIF_INFO;
        wcscpy_s(data.szInfoTitle, L"KeyPulse 仍在统计");
        wcscpy_s(data.szInfo, L"窗口已隐藏到通知区域。右键托盘图标可暂停或退出。");
        data.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void HandleTrayMessage(UINT event) {
        if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) {
            ShowMainWindow();
            return;
        }
        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            POINT cursor{};
            GetCursorPos(&cursor);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开 KeyPulse");
            AppendMenuW(menu, MF_STRING | (paused_ ? MF_CHECKED : 0), kCommandPause, L"暂停统计");
            AppendMenuW(menu, MF_STRING | (compactMode_ ? MF_CHECKED : 0),
                        kCommandCompact, L"精简显示");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING | (keypulse::IsStartupEnabled() ? MF_CHECKED : 0),
                        kCommandStartup, L"开机启动");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCommandExit, L"退出");
            SetForegroundWindow(window_);
            const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                 cursor.x, cursor.y, 0, window_, nullptr);
            DestroyMenu(menu);
            if (command != 0) HandleCommand(command);
        }
    }

    void HandleCommand(UINT command) {
        switch (command) {
        case kCommandOpen:
            ShowMainWindow();
            break;
        case kCommandPause:
            TogglePaused();
            break;
        case kCommandCompact:
            ToggleCompactMode();
            break;
        case kCommandStartup:
            if (!keypulse::SetStartupEnabled(!keypulse::IsStartupEnabled())) {
                MessageBoxW(window_, L"无法更新开机启动设置。", kWindowTitle,
                            MB_OK | MB_ICONERROR);
            }
            break;
        case kCommandExit:
            exitRequested_ = true;
            DestroyWindow(window_);
            break;
        default:
            break;
        }
    }

    void ShowMainWindow() {
        ShowWindow(window_, SW_SHOW);
        if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
    }

    void AddKey(std::wstring label, std::uint16_t code, float x, float y, float width,
                float height = 52.0f) {
        keys_.push_back({std::move(label), code, D2D1::RectF(x, y, x + width, y + height)});
    }

    void InitializeKeys() {
        constexpr float normal = 52.0f;
        constexpr float gap = 6.0f;
        constexpr float x0 = 44.0f;
        constexpr float yFunction = 196.0f;
        constexpr float yNumber = 257.0f;
        constexpr float yTab = 317.0f;
        constexpr float yCaps = 377.0f;
        constexpr float yShift = 437.0f;
        constexpr float yBottom = 497.0f;

        AddKey(L"Esc", Scan(0x01), x0, yFunction, 56, 46);
        const std::array<std::uint16_t, 12> fCodes{
            Scan(0x3B), Scan(0x3C), Scan(0x3D), Scan(0x3E), Scan(0x3F), Scan(0x40),
            Scan(0x41), Scan(0x42), Scan(0x43), Scan(0x44), Scan(0x57), Scan(0x58)};
        float x = 126.0f;
        for (std::size_t index = 0; index < fCodes.size(); ++index) {
            AddKey(L"F" + std::to_wstring(index + 1), fCodes[index], x, yFunction, 50, 46);
            x += 64.0f;
            if (index == 3 || index == 7) x += 15.0f;
        }
        AddKey(L"PrtSc", E0(0x37), 936, yFunction, normal, 46);
        AddKey(L"ScrLk", Scan(0x46), 994, yFunction, normal, 46);
        AddKey(L"Pause", E1(0x45), 1052, yFunction, normal, 46);

        const std::array<std::pair<const wchar_t*, std::uint16_t>, 13> numberRow{
            std::pair{L"`", Scan(0x29)}, std::pair{L"1", Scan(0x02)},
            std::pair{L"2", Scan(0x03)}, std::pair{L"3", Scan(0x04)},
            std::pair{L"4", Scan(0x05)}, std::pair{L"5", Scan(0x06)},
            std::pair{L"6", Scan(0x07)}, std::pair{L"7", Scan(0x08)},
            std::pair{L"8", Scan(0x09)}, std::pair{L"9", Scan(0x0A)},
            std::pair{L"0", Scan(0x0B)}, std::pair{L"−", Scan(0x0C)},
            std::pair{L"=", Scan(0x0D)}};
        x = x0;
        for (const auto& [label, code] : numberRow) {
            AddKey(label, code, x, yNumber, normal);
            x += normal + gap;
        }
        AddKey(L"Backspace", Scan(0x0E), x, yNumber, 112);

        AddKey(L"Ins", E0(0x52), 936, yNumber, normal);
        AddKey(L"Home", E0(0x47), 994, yNumber, normal);
        AddKey(L"PgUp", E0(0x49), 1052, yNumber, normal);
        AddKey(L"Num", Scan(0x45), 1130, yNumber, normal);
        AddKey(L"/", E0(0x35), 1188, yNumber, normal);
        AddKey(L"*", Scan(0x37), 1246, yNumber, normal);
        AddKey(L"−", Scan(0x4A), 1304, yNumber, normal);

        AddKey(L"Tab", Scan(0x0F), x0, yTab, 78);
        const std::array<std::pair<const wchar_t*, std::uint16_t>, 12> qRow{
            std::pair{L"Q", Scan(0x10)}, std::pair{L"W", Scan(0x11)},
            std::pair{L"E", Scan(0x12)}, std::pair{L"R", Scan(0x13)},
            std::pair{L"T", Scan(0x14)}, std::pair{L"Y", Scan(0x15)},
            std::pair{L"U", Scan(0x16)}, std::pair{L"I", Scan(0x17)},
            std::pair{L"O", Scan(0x18)}, std::pair{L"P", Scan(0x19)},
            std::pair{L"[", Scan(0x1A)}, std::pair{L"]", Scan(0x1B)}};
        x = x0 + 84;
        for (const auto& [label, code] : qRow) {
            AddKey(label, code, x, yTab, normal);
            x += normal + gap;
        }
        AddKey(L"\\", Scan(0x2B), x, yTab, 86);
        AddKey(L"Del", E0(0x53), 936, yTab, normal);
        AddKey(L"End", E0(0x4F), 994, yTab, normal);
        AddKey(L"PgDn", E0(0x51), 1052, yTab, normal);
        AddKey(L"7", Scan(0x47), 1130, yTab, normal);
        AddKey(L"8", Scan(0x48), 1188, yTab, normal);
        AddKey(L"9", Scan(0x49), 1246, yTab, normal);
        AddKey(L"+", Scan(0x4E), 1304, yTab, normal, 112);

        AddKey(L"Caps", Scan(0x3A), x0, yCaps, 92);
        const std::array<std::pair<const wchar_t*, std::uint16_t>, 11> aRow{
            std::pair{L"A", Scan(0x1E)}, std::pair{L"S", Scan(0x1F)},
            std::pair{L"D", Scan(0x20)}, std::pair{L"F", Scan(0x21)},
            std::pair{L"G", Scan(0x22)}, std::pair{L"H", Scan(0x23)},
            std::pair{L"J", Scan(0x24)}, std::pair{L"K", Scan(0x25)},
            std::pair{L"L", Scan(0x26)}, std::pair{L";", Scan(0x27)},
            std::pair{L"'", Scan(0x28)}};
        x = x0 + 98;
        for (const auto& [label, code] : aRow) {
            AddKey(label, code, x, yCaps, normal);
            x += normal + gap;
        }
        AddKey(L"Enter", Scan(0x1C), x, yCaps, 130);
        AddKey(L"4", Scan(0x4B), 1130, yCaps, normal);
        AddKey(L"5", Scan(0x4C), 1188, yCaps, normal);
        AddKey(L"6", Scan(0x4D), 1246, yCaps, normal);

        AddKey(L"Shift", Scan(0x2A), x0, yShift, 112);
        const std::array<std::pair<const wchar_t*, std::uint16_t>, 10> zRow{
            std::pair{L"Z", Scan(0x2C)}, std::pair{L"X", Scan(0x2D)},
            std::pair{L"C", Scan(0x2E)}, std::pair{L"V", Scan(0x2F)},
            std::pair{L"B", Scan(0x30)}, std::pair{L"N", Scan(0x31)},
            std::pair{L"M", Scan(0x32)}, std::pair{L",", Scan(0x33)},
            std::pair{L".", Scan(0x34)}, std::pair{L"/", Scan(0x35)}};
        x = x0 + 118;
        for (const auto& [label, code] : zRow) {
            AddKey(label, code, x, yShift, normal);
            x += normal + gap;
        }
        AddKey(L"Shift", Scan(0x36), x, yShift, 168);
        AddKey(L"↑", E0(0x48), 994, yShift, normal);
        AddKey(L"1", Scan(0x4F), 1130, yShift, normal);
        AddKey(L"2", Scan(0x50), 1188, yShift, normal);
        AddKey(L"3", Scan(0x51), 1246, yShift, normal);
        AddKey(L"Enter", E0(0x1C), 1304, yShift, normal, 112);

        x = x0;
        AddKey(L"Ctrl", Scan(0x1D), x, yBottom, 72); x += 78;
        AddKey(L"Win", E0(0x5B), x, yBottom, 60); x += 66;
        AddKey(L"Alt", Scan(0x38), x, yBottom, 60); x += 66;
        AddKey(L"Space", Scan(0x39), x, yBottom, 380); x += 386;
        AddKey(L"Alt", E0(0x38), x, yBottom, 60); x += 66;
        AddKey(L"Win", E0(0x5C), x, yBottom, 60); x += 66;
        AddKey(L"Menu", E0(0x5D), x, yBottom, 60); x += 66;
        AddKey(L"Ctrl", E0(0x1D), x, yBottom, 72);
        AddKey(L"←", E0(0x4B), 936, yBottom, normal);
        AddKey(L"↓", E0(0x50), 994, yBottom, normal);
        AddKey(L"→", E0(0x4D), 1052, yBottom, normal);
        AddKey(L"0", Scan(0x52), 1130, yBottom, 110);
        AddKey(L".", Scan(0x53), 1246, yBottom, normal);
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND firstDatePicker_ = nullptr;
    HWND lastDatePicker_ = nullptr;
    HFONT pickerFont_ = nullptr;
    HICON largeIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
    UINT dpi_ = 96;
    UINT taskbarCreatedMessage_ = 0;
    HANDLE hookThread_ = nullptr;
    KeyboardHookThreadContext hookContext_{};

    keypulse::StatisticsStore store_;
    keypulse::Snapshot snapshot_;
    std::vector<KeyDefinition> keys_;
    Period period_ = Period::Today;
    std::int32_t knownToday_ = 0;
    std::int32_t customFirstDay_ = 0;
    std::int32_t customLastDayInclusive_ = 0;

    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IWICImagingFactory> wicFactory_;
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> sectionFormat_;
    ComPtr<IDWriteTextFormat> totalFormat_;
    ComPtr<IDWriteTextFormat> tabFormat_;
    ComPtr<IDWriteTextFormat> keyLabelFormat_;
    ComPtr<IDWriteTextFormat> keyCountFormat_;
    ComPtr<IDWriteTextFormat> smallFormat_;
    ComPtr<IDWriteTextFormat> smallCenterFormat_;

    float canvasScale_ = 1.0f;
    float canvasOffsetX_ = 0.0f;
    float canvasOffsetY_ = 0.0f;
    int hoveredKey_ = -1;
    int hoveredPalette_ = -1;
    Palette palette_ = Palette::SquareRoot;
    bool mouseTracking_ = false;
    bool minimizeHovered_ = false;
    bool compactButtonHovered_ = false;
    bool compactMode_ = false;
    bool paletteMenuOpen_ = false;
    bool updatingPickers_ = false;
    bool paused_ = false;
    bool captureAvailable_ = true;
    bool rawInputAvailable_ = true;
    bool hookAvailable_ = false;
    bool trayAdded_ = false;
    bool trayHintShown_ = false;
    bool exitRequested_ = false;
    ULONGLONG lastSaveTick_ = 0;
    keypulse::KeyEventMerger inputMerger_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\KeyPulse.KeyboardFrequency.Singleton.v1");
    if (!singleInstance) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            ShowWindow(existing, SW_SHOW);
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(singleInstance);
        return 0;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int exitCode = 0;
    {
        Application application(instance);
        if (!application.Initialize(showCommand)) {
            MessageBoxW(nullptr, L"KeyPulse 初始化失败。请确认系统支持 Direct2D 和 DirectWrite。",
                        kWindowTitle, MB_OK | MB_ICONERROR);
            exitCode = 2;
        } else {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            exitCode = static_cast<int>(message.wParam);
        }
    }
    if (SUCCEEDED(comResult)) CoUninitialize();
    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    return exitCode;
}
