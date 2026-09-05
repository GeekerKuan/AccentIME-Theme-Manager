#include "Core.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr int kEditColor = 1001;
constexpr int kChooseColor = 1002;
constexpr int kSystemColor = 1003;
constexpr int kFollowWindows = 1004;
constexpr int kApply = 1005;
constexpr int kRestore = 1006;
constexpr int kPreview = 1007;
constexpr int kStatus = 1008;
constexpr int kDetail = 1009;

HFONT gFont = nullptr;
HFONT gTitleFont = nullptr;
HBRUSH gPreviewBrush = nullptr;
std::uint32_t gColor = 0x0078D4;

void SetFont(HWND control) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
}

HWND AddControl(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style,
                int x, int y, int width, int height, int id = 0) {
    const auto control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
                                         x, y, width, height, parent,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                         GetModuleHandleW(nullptr), nullptr);
    SetFont(control);
    return control;
}

void SetColor(HWND window, std::uint32_t rgb) {
    gColor = rgb & 0xFFFFFFu;
    const auto text = accentime::FormatColor(gColor);
    SetWindowTextW(GetDlgItem(window, kEditColor), text.c_str());
    if (gPreviewBrush) DeleteObject(gPreviewBrush);
    gPreviewBrush = CreateSolidBrush(RGB((gColor >> 16) & 0xFF, (gColor >> 8) & 0xFF, gColor & 0xFF));
    InvalidateRect(GetDlgItem(window, kPreview), nullptr, TRUE);
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

bool RunElevated(const std::wstring& arguments) {
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.hwnd = nullptr;
    info.lpVerb = L"runas";
    const auto executable = accentime::ExecutablePath().wstring();
    info.lpFile = executable.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) return false;
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    return exitCode == 0;
}

void RefreshStatus(HWND window) {
    const auto status = accentime::QueryStatus();
    std::wstring title = L"微信输入法 ";
    title += status.installedVersion.empty() ? L"未检测到" : status.installedVersion;
    title += status.supported ? L"  ·  已支持" : L"  ·  不受支持";
    SetWindowTextW(GetDlgItem(window, kStatus), title.c_str());
    SetWindowTextW(GetDlgItem(window, kDetail), status.detail.c_str());
    CheckDlgButton(window, kFollowWindows, status.followWindows ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(window, kApply), status.supported);
    EnableWindow(GetDlgItem(window, kRestore), status.active);
    if (status.active) {
        std::uint32_t rgb = 0;
        if (accentime::ParseColor(status.currentColor, rgb)) SetColor(window, rgb);
    }
}

void ApplyFromUi(HWND window) {
    wchar_t value[32]{};
    GetWindowTextW(GetDlgItem(window, kEditColor), value, static_cast<int>(std::size(value)));
    std::uint32_t rgb = 0;
    if (!accentime::ParseColor(value, rgb)) {
        MessageBoxW(window, L"请输入 #RRGGBB 格式的颜色。", accentime::kProductName, MB_OK | MB_ICONWARNING);
        return;
    }
    const bool follow = IsDlgButtonChecked(window, kFollowWindows) == BST_CHECKED;
    if (follow) {
        try {
            rgb = accentime::ReadWindowsAccent();
            SetColor(window, rgb);
        } catch (...) {
            MessageBoxW(window, L"无法读取 Windows 强调色。", accentime::kProductName, MB_OK | MB_ICONERROR);
            return;
        }
    }
    const auto arguments = L"--apply " + Quote(accentime::FormatColor(rgb)) +
        L" --follow " + (follow ? L"1" : L"0");
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const bool ok = RunElevated(arguments);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    RefreshStatus(window);
    if (ok) {
        MessageBoxW(window, L"候选项强调色和 WinUI 3 风格语音界面已应用。若界面未立即刷新，请重新登录 Windows。",
                    accentime::kProductName, MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(window, L"操作未完成。若你取消了 UAC，这是预期结果；否则请查看上一个错误提示。",
                    accentime::kProductName, MB_OK | MB_ICONWARNING);
    }
}

void RestoreFromUi(HWND window) {
    if (MessageBoxW(window, L"恢复微信输入法原文件并关闭自动跟随？", accentime::kProductName,
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const bool ok = RunElevated(L"--restore");
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    RefreshStatus(window);
    if (ok) {
        MessageBoxW(window, L"原文件已恢复。", accentime::kProductName, MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(window, L"恢复未完成。若你取消了 UAC，这是预期结果；否则请查看上一个错误提示。",
                    accentime::kProductName, MB_OK | MB_ICONWARNING);
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            NONCLIENTMETRICSW metrics{sizeof(metrics)};
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
            wcscpy_s(metrics.lfMessageFont.lfFaceName, L"Segoe UI");
            metrics.lfMessageFont.lfHeight = -16;
            gFont = CreateFontIndirectW(&metrics.lfMessageFont);
            metrics.lfMessageFont.lfHeight = -24;
            metrics.lfMessageFont.lfWeight = FW_SEMIBOLD;
            gTitleFont = CreateFontIndirectW(&metrics.lfMessageFont);

            const auto title = AddControl(window, L"STATIC", L"AccentIME Theme Manager", SS_LEFT,
                                          28, 22, 560, 34);
            SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(gTitleFont), TRUE);
            AddControl(window, L"STATIC", L"微信输入法候选项与 WinUI 3 风格语音输入界面", SS_LEFT,
                       30, 60, 560, 24);
            AddControl(window, L"STATIC", L"正在检测…", SS_LEFT,
                       30, 104, 560, 24, kStatus);
            AddControl(window, L"STATIC", L"", SS_LEFT,
                       30, 132, 560, 24, kDetail);

            AddControl(window, L"STATIC", L"", SS_OWNERDRAW | WS_BORDER,
                       30, 180, 54, 38, kPreview);
            AddControl(window, L"EDIT", L"#0078D4", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                       98, 182, 130, 34, kEditColor);
            AddControl(window, L"BUTTON", L"选择颜色…", WS_TABSTOP | BS_PUSHBUTTON,
                       244, 180, 130, 38, kChooseColor);
            AddControl(window, L"BUTTON", L"读取系统色", WS_TABSTOP | BS_PUSHBUTTON,
                       390, 180, 140, 38, kSystemColor);
            AddControl(window, L"BUTTON", L"跟随 Windows 强调色（事件驱动）",
                       WS_TABSTOP | BS_AUTOCHECKBOX, 30, 244, 360, 30, kFollowWindows);
            AddControl(window, L"STATIC", L"语音背景与识别气泡使用深色材质；保留原生动画与状态切换。",
                       SS_LEFT, 52, 278, 500, 24);
            AddControl(window, L"BUTTON", L"应用", WS_TABSTOP | BS_DEFPUSHBUTTON,
                       330, 326, 120, 42, kApply);
            AddControl(window, L"BUTTON", L"恢复原文件", WS_TABSTOP | BS_PUSHBUTTON,
                       464, 326, 120, 42, kRestore);
            try {
                SetColor(window, accentime::ReadWindowsAccent());
            } catch (...) {
                SetColor(window, 0x00A86F);
            }
            RefreshStatus(window);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kChooseColor: {
                    static COLORREF custom[16]{};
                    CHOOSECOLORW chooser{sizeof(chooser)};
                    chooser.hwndOwner = window;
                    chooser.lpCustColors = custom;
                    chooser.rgbResult = RGB((gColor >> 16) & 0xFF, (gColor >> 8) & 0xFF, gColor & 0xFF);
                    chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
                    if (ChooseColorW(&chooser)) {
                        SetColor(window, (GetRValue(chooser.rgbResult) << 16) |
                                         (GetGValue(chooser.rgbResult) << 8) |
                                         GetBValue(chooser.rgbResult));
                        CheckDlgButton(window, kFollowWindows, BST_UNCHECKED);
                    }
                    return 0;
                }
                case kSystemColor:
                    try { SetColor(window, accentime::ReadWindowsAccent()); } catch (...) { }
                    return 0;
                case kApply:
                    ApplyFromUi(window);
                    return 0;
                case kRestore:
                    RestoreFromUi(window);
                    return 0;
            }
            break;
        case WM_DRAWITEM: {
            const auto draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw->CtlID == kPreview) {
                FillRect(draw->hDC, &draw->rcItem, gPreviewBrush);
                FrameRect(draw->hDC, &draw->rcItem, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
                return TRUE;
            }
            break;
        }
        case WM_DESTROY:
            if (gPreviewBrush) DeleteObject(gPreviewBrush);
            if (gFont) DeleteObject(gFont);
            if (gTitleFont) DeleteObject(gTitleFont);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int RunGui(HINSTANCE instance, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const wchar_t className[] = L"AccentIME.ThemeManager.Window";
    WNDCLASSEXW type{sizeof(type)};
    type.style = CS_HREDRAW | CS_VREDRAW;
    type.lpfnWndProc = WindowProcedure;
    type.hInstance = instance;
    type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    type.lpszClassName = className;
    type.hIconSm = type.hIcon;
    if (!RegisterClassExW(&type)) return 2;
    const auto window = CreateWindowExW(0, className, accentime::kProductName,
                                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                        CW_USEDEFAULT, CW_USEDEFAULT, 640, 430,
                                        nullptr, nullptr, instance, nullptr);
    if (!window) return 3;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int show) {
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 10;
    const auto cleanup = [&] { LocalFree(arguments); };
    if (count > 1) {
        const std::wstring mode = arguments[1];
        if (mode == L"--watch") {
            cleanup();
            return accentime::WatchWindowsAccent();
        }
        if (mode == L"--uninstall-cleanup") {
            cleanup();
            return accentime::UninstallCleanup();
        }
        if (mode == L"--apply" && count == 5 && std::wstring(arguments[3]) == L"--follow") {
            std::uint32_t rgb = 0;
            if (!accentime::ParseColor(arguments[2], rgb)) {
                cleanup();
                return 11;
            }
            const auto result = accentime::ApplyColor(rgb, std::wstring(arguments[4]) == L"1");
            if (!result.ok) MessageBoxW(nullptr, result.message.c_str(), accentime::kProductName, MB_OK | MB_ICONERROR);
            cleanup();
            return result.ok ? 0 : 12;
        }
        if (mode == L"--restore") {
            const auto result = accentime::RestoreOriginal(true);
            if (!result.ok) MessageBoxW(nullptr, result.message.c_str(), accentime::kProductName, MB_OK | MB_ICONERROR);
            cleanup();
            return result.ok ? 0 : 13;
        }
    }
    cleanup();
    return RunGui(instance, show);
}
