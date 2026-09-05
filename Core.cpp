#include "Core.h"

#include <windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

namespace accentime {
namespace {

struct HandleCloser {
    void operator()(HANDLE value) const noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct ServiceCloser {
    void operator()(SC_HANDLE value) const noexcept {
        if (value) CloseServiceHandle(value);
    }
};
using UniqueService = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ServiceCloser>;

struct RegCloser {
    void operator()(HKEY value) const noexcept {
        if (value) RegCloseKey(value);
    }
};
using UniqueReg = std::unique_ptr<std::remove_pointer_t<HKEY>, RegCloser>;

struct State {
    fs::path target;
    fs::path backup;
    std::uint32_t color{};
    bool follow{};
    std::wstring phase;
    std::wstring created;
};

thread_local bool gWatcherContext = false;

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return "AccentIME operation failed.";
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

[[noreturn]] void Fail(const std::wstring& message) {
    throw std::runtime_error(Utf8(message));
}

std::wstring WidenError(const std::exception& error) {
    const std::string text = error.what();
    if (text.empty()) return L"AccentIME operation failed.";
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return L"AccentIME operation failed.";
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool EqualsNoCase(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool StartsWithPath(const fs::path& child, const fs::path& parent) {
    auto childText = Lower(fs::weakly_canonical(child).wstring());
    auto parentText = Lower(fs::weakly_canonical(parent).wstring());
    if (!parentText.empty() && parentText.back() != L'\\') parentText.push_back(L'\\');
    return childText.rfind(parentText, 0) == 0;
}

std::wstring ReadRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    DWORD bytes = 0;
    auto status = RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
    std::vector<wchar_t> value(bytes / sizeof(wchar_t));
    status = RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes);
    if (status != ERROR_SUCCESS) return {};
    return value.data();
}

fs::path ProgramDataRoot() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw))) {
        Fail(L"Unable to locate ProgramData.");
    }
    fs::path result(raw);
    CoTaskMemFree(raw);
    return result / L"AccentIME";
}

fs::path ActiveStatePath() {
    return ProgramDataRoot() / L"Active" / L"state.ini";
}

fs::path InstallRoot(std::wstring* version = nullptr) {
    constexpr wchar_t key[] = L"SOFTWARE\\Tencent\\WeType";
    auto install = ReadRegistryString(HKEY_LOCAL_MACHINE, key, L"InstallPath");
    auto installedVersion = ReadRegistryString(HKEY_LOCAL_MACHINE, key, L"Version");
    if (install.empty()) {
        const auto base = ReadRegistryString(HKEY_LOCAL_MACHINE, key, L"InstallDir");
        if (!base.empty() && !installedVersion.empty()) install = (fs::path(base) / installedVersion).wstring();
    }
    if (version) *version = installedVersion;
    if (install.empty()) Fail(L"WeType installation was not found.");
    return fs::path(install);
}

fs::path TargetPath(std::wstring* version = nullptr) {
    return InstallRoot(version) / L"flutter_datas" / L"business_data" / L"app.so";
}

std::wstring Timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t value[64]{};
    swprintf_s(value, L"%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond);
    return value;
}

std::wstring NewGuid() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) Fail(L"Unable to create recovery identifier.");
    wchar_t text[64]{};
    if (StringFromGUID2(guid, text, static_cast<int>(std::size(text))) == 0) {
        Fail(L"Unable to format recovery identifier.");
    }
    std::wstring result(text);
    result.erase(std::remove_if(result.begin(), result.end(), [](wchar_t ch) {
        return ch == L'{' || ch == L'}' || ch == L'-';
    }), result.end());
    return result;
}

std::wstring StateText(const State& state) {
    std::wostringstream output;
    output << L"Schema=1\r\n"
           << L"Product=AccentIME\r\n"
           << L"Version=" << kSupportedVersion << L"\r\n"
           << L"Target=" << state.target.wstring() << L"\r\n"
           << L"Backup=" << state.backup.wstring() << L"\r\n"
           << L"Color=" << FormatColor(state.color) << L"\r\n"
           << L"Follow=" << (state.follow ? 1 : 0) << L"\r\n"
           << L"Phase=" << state.phase << L"\r\n"
           << L"Created=" << state.created << L"\r\n";
    return output.str();
}

void WriteUtf16Atomic(const fs::path& path, const std::wstring& text) {
    fs::create_directories(path.parent_path());
    const auto temporary = path.wstring() + L".tmp-" + NewGuid();
    UniqueHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) Fail(L"Unable to create state file.");
    const std::uint16_t bom = 0xFEFF;
    DWORD written = 0;
    if (!WriteFile(file.get(), &bom, sizeof(bom), &written, nullptr) || written != sizeof(bom)) {
        file.reset();
        DeleteFileW(temporary.c_str());
        Fail(L"Unable to write state file BOM.");
    }
    const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    if (!WriteFile(file.get(), text.data(), bytes, &written, nullptr) || written != bytes ||
        !FlushFileBuffers(file.get())) {
        file.reset();
        DeleteFileW(temporary.c_str());
        Fail(L"Unable to persist state file.");
    }
    file.reset();
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        Fail(L"Unable to commit state file.");
    }
}

std::map<std::wstring, std::wstring> ReadUtf16Map(const fs::path& path) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) Fail(L"Recovery state is unavailable.");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 2 || size.QuadPart > 1024 * 1024) {
        Fail(L"Recovery state has an invalid size.");
    }
    std::vector<std::uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
        read != bytes.size() || bytes[0] != 0xFF || bytes[1] != 0xFE) {
        Fail(L"Recovery state is not valid UTF-16LE.");
    }
    std::wstring text(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
    std::map<std::wstring, std::wstring> values;
    std::wistringstream lines(text);
    std::wstring line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        const auto separator = line.find(L'=');
        if (separator != std::wstring::npos) values[line.substr(0, separator)] = line.substr(separator + 1);
    }
    return values;
}

State LoadState() {
    const auto values = ReadUtf16Map(ActiveStatePath());
    if (values.at(L"Schema") != L"1" || values.at(L"Product") != L"AccentIME" ||
        values.at(L"Version") != kSupportedVersion) {
        Fail(L"Recovery state is not compatible with this version.");
    }
    State state;
    state.target = values.at(L"Target");
    state.backup = values.at(L"Backup");
    if (!ParseColor(values.at(L"Color"), state.color)) Fail(L"Recovery state color is invalid.");
    state.follow = values.at(L"Follow") == L"1";
    state.phase = values.at(L"Phase");
    state.created = values.at(L"Created");
    return state;
}

void SaveState(const State& state) {
    WriteUtf16Atomic(ActiveStatePath(), StateText(state));
}

void CopyDurable(const fs::path& source, const fs::path& destination, bool failIfExists) {
    fs::create_directories(destination.parent_path());
    if (!CopyFileW(source.c_str(), destination.c_str(), failIfExists ? TRUE : FALSE)) {
        Fail(L"Unable to copy a protected file.");
    }
    UniqueHandle file(CreateFileW(destination.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE || !FlushFileBuffers(file.get())) {
        Fail(L"Unable to flush a protected file to disk.");
    }
}

void WriteColorField(HANDLE file, std::uint64_t fieldOffset, std::uint32_t rgb) {
    const auto replacement = EncodeColor(rgb);
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(fieldOffset);
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) Fail(L"Unable to seek a color field.");
    DWORD written = 0;
    if (!WriteFile(file, replacement.data(), static_cast<DWORD>(replacement.size()), &written, nullptr) ||
        written != replacement.size()) {
        Fail(L"Unable to persist a color field.");
    }
}

template <size_t Size>
void WriteField(HANDLE file, std::uint64_t fieldOffset,
                const std::array<std::uint8_t, Size>& replacement) {
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(fieldOffset);
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) Fail(L"Unable to seek a patch field.");
    DWORD written = 0;
    if (!WriteFile(file, replacement.data(), static_cast<DWORD>(replacement.size()), &written, nullptr) ||
        written != replacement.size()) {
        Fail(L"Unable to persist a patch field.");
    }
}

void PatchInPlace(const fs::path& path, std::uint32_t rgb) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) Fail(L"Unable to open the staged snapshot.");
    const auto voice = VoiceGradient(rgb);
    WriteColorField(file.get(), kColorOffset, rgb);
    WriteColorField(file.get(), kVoiceGradientTopOffset, voice[0]);
    WriteColorField(file.get(), kVoiceGradientBottomOffset, voice[1]);
    WriteField(file.get(), kVoiceBubbleColorCodeOffset, kPatchedVoiceBubbleColorCode);
    WriteField(file.get(), kVoiceBubbleRadiusOperandOffset, kPatchedVoiceBubbleRadiusOperand);
    WriteField(file.get(), kBubbleTailPainterCodeOffset, kPatchedBubbleTailPainterCode);
    if (!FlushFileBuffers(file.get())) Fail(L"Unable to flush the staged snapshot.");
}

template <size_t Size>
std::array<std::uint8_t, Size> ReadField(const fs::path& path, std::uint64_t fieldOffset) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) Fail(L"Unable to open app.so.");
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(fieldOffset);
    if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN)) Fail(L"Unable to seek app.so.");
    std::array<std::uint8_t, Size> bytes{};
    DWORD read = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
        read != bytes.size()) Fail(L"Unable to read a patch field.");
    return bytes;
}

void VerifyOriginal(const fs::path& path) {
    if (!fs::exists(path) || !EqualsNoCase(Sha256File(path), kOriginalSha256) ||
        ReadField<5>(path, kColorOffset) != kOriginalColorBytes ||
        ReadField<5>(path, kVoiceGradientTopOffset) != kOriginalVoiceGradientTopBytes ||
        ReadField<5>(path, kVoiceGradientBottomOffset) != kOriginalVoiceGradientBottomBytes ||
        ReadField<8>(path, kVoiceBubbleColorCodeOffset) != kOriginalVoiceBubbleColorCode ||
        ReadField<4>(path, kVoiceBubbleRadiusOperandOffset) != kOriginalVoiceBubbleRadiusOperand ||
        ReadField<5>(path, kBubbleTailPainterCodeOffset) != kOriginalBubbleTailPainterCode) {
        Fail(L"app.so is not the verified WeType 2.1.3.18 original.");
    }
}

void AtomicReplace(const fs::path& target, const fs::path& replacement) {
    const auto rollback = target.wstring() + L".accentime-rollback-" + NewGuid();
    if (!ReplaceFileW(target.c_str(), replacement.c_str(), rollback.c_str(),
                      REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        DeleteFileW(replacement.c_str());
        Fail(L"Windows could not atomically replace app.so.");
    }
    DeleteFileW(rollback.c_str());
}

void WaitForServiceState(SC_HANDLE service, DWORD desired, DWORD timeoutMs) {
    const auto start = GetTickCount64();
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    while (GetTickCount64() - start < timeoutMs) {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) break;
        if (status.dwCurrentState == desired) return;
        Sleep(200);
    }
    Fail(desired == SERVICE_STOPPED ? L"WeType service did not stop in time."
                                    : L"WeType service did not start in time.");
}

void StopService() {
    UniqueService manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) Fail(L"Unable to open Service Control Manager.");
    UniqueService service(OpenServiceW(manager.get(), L"WeType Management Service",
                                      SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service) Fail(L"Unable to open WeType Management Service.");
    SERVICE_STATUS status{};
    if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &status)) {
        const auto error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) Fail(L"Unable to stop WeType Management Service.");
    }
    WaitForServiceState(service.get(), SERVICE_STOPPED, 15000);
}

void StartService() noexcept {
    UniqueService manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) return;
    UniqueService service(OpenServiceW(manager.get(), L"WeType Management Service",
                                      SERVICE_START | SERVICE_QUERY_STATUS));
    if (!service) return;
    if (!StartServiceW(service.get(), 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) return;
    try { WaitForServiceState(service.get(), SERVICE_RUNNING, 15000); } catch (...) { }
}

bool IsWeTypeName(const std::wstring& name) {
    const auto lower = Lower(name);
    return lower == L"wetype_renderer.exe" || lower == L"wetype_server.exe" ||
           lower == L"wetype_update.exe" || lower == L"wetype_service.exe";
}

void StopProcesses(const fs::path& installRoot) {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) Fail(L"Unable to enumerate WeType processes.");
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.get(), &entry)) return;
    do {
        if (!IsWeTypeName(entry.szExeFile)) continue;
        UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                                         FALSE, entry.th32ProcessID));
        if (!process) continue;
        std::vector<wchar_t> path(32768);
        DWORD length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)) continue;
        if (!StartsWithPath(fs::path(std::wstring(path.data(), length)), installRoot)) continue;
        if (!TerminateProcess(process.get(), 0)) Fail(L"Unable to stop a WeType process.");
        WaitForSingleObject(process.get(), 5000);
    } while (Process32NextW(snapshot.get(), &entry));
}

void StopWeType(const fs::path& installRoot) {
    StopService();
    StopProcesses(installRoot);
    Sleep(250);
}

std::wstring QuoteArgument(const std::wstring& value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

DWORD RunHidden(const std::wstring& commandLine) {
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return GetLastError();
    UniqueHandle thread(process.hThread);
    UniqueHandle handle(process.hProcess);
    WaitForSingleObject(handle.get(), 30000);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(handle.get(), &exitCode);
    return exitCode;
}

void ConfigureTask(bool enabled) {
    wchar_t systemDirectory[MAX_PATH]{};
    GetSystemDirectoryW(systemDirectory, MAX_PATH);
    const fs::path schtasks = fs::path(systemDirectory) / L"schtasks.exe";
    constexpr wchar_t taskName[] = L"AccentIME Theme Watcher";
    if (!enabled) {
        RunHidden(QuoteArgument(schtasks.wstring()) + L" /End /TN " + QuoteArgument(taskName));
        RunHidden(QuoteArgument(schtasks.wstring()) + L" /Delete /TN " + QuoteArgument(taskName) + L" /F");
        return;
    }
    const auto action = QuoteArgument(ExecutablePath().wstring()) + L" --watch";
    const auto create = QuoteArgument(schtasks.wstring()) + L" /Create /TN " + QuoteArgument(taskName) +
        L" /SC ONLOGON /RL HIGHEST /TR " + QuoteArgument(action) + L" /F";
    if (RunHidden(create) != ERROR_SUCCESS) Fail(L"Unable to register the Windows accent watcher task.");
    RunHidden(QuoteArgument(schtasks.wstring()) + L" /Run /TN " + QuoteArgument(taskName));
}

void WriteManifest(const fs::path& directory, const State& state) {
    WriteUtf16Atomic(directory / L"recovery.ini", StateText(state));
}

void RemoveActiveState() {
    std::error_code ignored;
    fs::remove(ActiveStatePath(), ignored);
}

void ValidatePatchedCopyImpl(const fs::path& original, const fs::path& patched,
                             std::uint32_t rgb, bool patchVoice, bool patchBubble) {
    const auto selection = EncodeColor(rgb);
    const auto voiceColors = VoiceGradient(rgb);
    const auto voiceTop = patchVoice ? EncodeColor(voiceColors[0]) : kOriginalVoiceGradientTopBytes;
    const auto voiceBottom = patchVoice ? EncodeColor(voiceColors[1]) : kOriginalVoiceGradientBottomBytes;
    UniqueHandle source(CreateFileW(original.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    UniqueHandle target(CreateFileW(patched.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (source.get() == INVALID_HANDLE_VALUE || target.get() == INVALID_HANDLE_VALUE) {
        Fail(L"Unable to validate the staged snapshot.");
    }
    LARGE_INTEGER sourceSize{}, targetSize{};
    if (!GetFileSizeEx(source.get(), &sourceSize) || !GetFileSizeEx(target.get(), &targetSize) ||
        sourceSize.QuadPart != targetSize.QuadPart) Fail(L"The staged snapshot changed file size.");
    std::vector<std::uint8_t> sourceBytes(64 * 1024);
    std::vector<std::uint8_t> targetBytes(64 * 1024);
    std::uint64_t absolute = 0;
    while (true) {
        DWORD sourceCount = 0, targetCount = 0;
        if (!ReadFile(source.get(), sourceBytes.data(), static_cast<DWORD>(sourceBytes.size()), &sourceCount, nullptr) ||
            !ReadFile(target.get(), targetBytes.data(), static_cast<DWORD>(targetBytes.size()), &targetCount, nullptr) ||
            sourceCount != targetCount) Fail(L"Unable to compare the staged snapshot.");
        if (!sourceCount) break;
        for (DWORD index = 0; index < sourceCount; ++index, ++absolute) {
            auto expectedByte = sourceBytes[index];
            if (absolute >= kColorOffset && absolute < kColorOffset + selection.size()) {
                expectedByte = selection[static_cast<size_t>(absolute - kColorOffset)];
            } else if (absolute >= kVoiceGradientTopOffset &&
                       absolute < kVoiceGradientTopOffset + voiceTop.size()) {
                expectedByte = voiceTop[static_cast<size_t>(absolute - kVoiceGradientTopOffset)];
            } else if (absolute >= kVoiceGradientBottomOffset &&
                       absolute < kVoiceGradientBottomOffset + voiceBottom.size()) {
                expectedByte = voiceBottom[static_cast<size_t>(absolute - kVoiceGradientBottomOffset)];
            } else if (patchBubble && absolute >= kVoiceBubbleColorCodeOffset &&
                       absolute < kVoiceBubbleColorCodeOffset + kPatchedVoiceBubbleColorCode.size()) {
                expectedByte = kPatchedVoiceBubbleColorCode[
                    static_cast<size_t>(absolute - kVoiceBubbleColorCodeOffset)];
            } else if (patchBubble && absolute >= kVoiceBubbleRadiusOperandOffset &&
                       absolute < kVoiceBubbleRadiusOperandOffset + kPatchedVoiceBubbleRadiusOperand.size()) {
                expectedByte = kPatchedVoiceBubbleRadiusOperand[
                    static_cast<size_t>(absolute - kVoiceBubbleRadiusOperandOffset)];
            } else if (patchBubble && absolute >= kBubbleTailPainterCodeOffset &&
                       absolute < kBubbleTailPainterCodeOffset + kPatchedBubbleTailPainterCode.size()) {
                expectedByte = kPatchedBubbleTailPainterCode[
                    static_cast<size_t>(absolute - kBubbleTailPainterCodeOffset)];
            }
            if (targetBytes[index] != expectedByte) {
                Fail(L"The staged snapshot contains a non-whitelisted change.");
            }
        }
    }
}

bool ValidateCurrentOrOriginal(const State& state) {
    if (EqualsNoCase(Sha256File(state.target), kOriginalSha256)) return true;
    try {
        ValidatePatchedCopy(state.backup, state.target, state.color);
        return true;
    } catch (...) {
        try {
            ValidatePatchedCopyImpl(state.backup, state.target, state.color, true, false);
            return true;
        } catch (...) {
            try {
                ValidatePatchedCopyImpl(state.backup, state.target, state.color, false, false);
                return true;
            } catch (...) {
                return false;
            }
        }
    }
}

void EmergencyRestore(const State& state) noexcept {
    try {
        if (!fs::exists(state.backup)) return;
        const auto staged = state.target.parent_path() /
            (L".app.so.accentime-emergency-" + NewGuid());
        CopyDurable(state.backup, staged, true);
        AtomicReplace(state.target, staged);
        RemoveActiveState();
    } catch (...) {
        // The permanent backup and recovery manifest remain available for manual recovery.
    }
}

OperationResult ErrorResult(const std::exception& error) {
    return {false, WidenError(error)};
}

void AppendWatcherLog(const std::wstring& message) noexcept {
    try {
        const auto path = ProgramDataRoot() / L"watcher.log";
        fs::create_directories(path.parent_path());
        std::wofstream stream(path, std::ios::app);
        stream << Timestamp() << L" " << message << L"\n";
    } catch (...) { }
}

}  // namespace

std::array<std::uint8_t, 5> EncodeColor(std::uint32_t rgb) {
    const std::int32_t value = static_cast<std::int32_t>(0xFF000000u | (rgb & 0xFFFFFFu));
    std::array<std::uint8_t, 5> result{};
    auto remaining = value;
    for (size_t index = 0; index < 3; ++index) {
        result[index] = static_cast<std::uint8_t>(remaining & 0x7F);
        remaining >>= 7;
    }
    if (remaining < -64 || remaining > 63) Fail(L"Color cannot be encoded safely.");
    result[3] = static_cast<std::uint8_t>(0xC0 + remaining);
    result[4] = 0xC0;
    return result;
}

std::array<std::uint32_t, 2> VoiceGradient(std::uint32_t rgb) {
    rgb &= 0xFFFFFFu;
    const auto tint = [rgb](std::uint32_t neutral, unsigned accentPercent) {
        const auto mix = [accentPercent](std::uint32_t base, std::uint32_t accent) {
            return (base * (100u - accentPercent) + accent * accentPercent + 50u) / 100u;
        };
        return (mix((neutral >> 16) & 0xFFu, (rgb >> 16) & 0xFFu) << 16) |
               (mix((neutral >> 8) & 0xFFu, (rgb >> 8) & 0xFFu) << 8) |
               mix(neutral & 0xFFu, rgb & 0xFFu);
    };

    // WinUI 3-inspired dark material: a neutral surface receives a restrained
    // amount of the selected Windows accent. The original Flutter microphone,
    // VAD animation and result-state rendering remain untouched and retain
    // high contrast even when the selected accent itself is very bright.
    return {tint(0x303030u, 18), tint(0x242424u, 10)};
}

std::uint32_t ReadWindowsAccent() {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    const auto status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                                     L"AccentColor", RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    if (status != ERROR_SUCCESS) Fail(L"Windows accent color is unavailable.");
    const auto red = value & 0xFFu;
    const auto green = (value >> 8) & 0xFFu;
    const auto blue = (value >> 16) & 0xFFu;
    return (red << 16) | (green << 8) | blue;
}

std::wstring FormatColor(std::uint32_t rgb) {
    wchar_t text[16]{};
    swprintf_s(text, L"#%06X", rgb & 0xFFFFFFu);
    return text;
}

bool ParseColor(const std::wstring& text, std::uint32_t& rgb) {
    auto value = text;
    if (!value.empty() && value.front() == L'#') value.erase(value.begin());
    if (value.size() != 6 || !std::all_of(value.begin(), value.end(), [](wchar_t ch) {
        return std::iswxdigit(ch) != 0;
    })) return false;
    wchar_t* end = nullptr;
    const auto parsed = std::wcstoul(value.c_str(), &end, 16);
    if (!end || *end != L'\0' || parsed > 0xFFFFFFu) return false;
    rgb = static_cast<std::uint32_t>(parsed);
    return true;
}

std::wstring Sha256File(const fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        Fail(L"Unable to initialize SHA-256.");
    }
    DWORD objectLength = 0;
    DWORD returned = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &returned, 0) < 0 || objectLength == 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        Fail(L"Unable to query SHA-256 requirements.");
    }
    std::vector<std::uint8_t> object(objectLength);
    std::array<std::uint8_t, 32> digest{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        Fail(L"Unable to create SHA-256 context.");
    }
    try {
        UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE) Fail(L"Unable to read a protected file.");
        std::vector<std::uint8_t> buffer(1024 * 1024);
        DWORD count = 0;
        for (;;) {
            if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
                Fail(L"Unable to finish reading a protected file.");
            }
            if (!count) break;
            if (BCryptHashData(hash, buffer.data(), count, 0) < 0) Fail(L"SHA-256 update failed.");
        }
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
            Fail(L"SHA-256 finalization failed.");
        }
    } catch (...) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    static constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring text;
    text.reserve(64);
    for (const auto byte : digest) {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0F]);
    }
    return text;
}

void GeneratePatchedCopy(const fs::path& original, const fs::path& output, std::uint32_t rgb) {
    VerifyOriginal(original);
    CopyDurable(original, output, true);
    try {
        PatchInPlace(output, rgb);
        ValidatePatchedCopy(original, output, rgb);
    } catch (...) {
        DeleteFileW(output.c_str());
        throw;
    }
}

void ValidatePatchedCopy(const fs::path& original, const fs::path& patched, std::uint32_t rgb) {
    ValidatePatchedCopyImpl(original, patched, rgb, true, true);
}

ProductStatus QueryStatus() {
    ProductStatus status;
    try {
        const auto target = TargetPath(&status.installedVersion);
        status.supported = status.installedVersion == kSupportedVersion && fs::exists(target);
        const auto statePath = ActiveStatePath();
        if (fs::exists(statePath)) {
            const auto state = LoadState();
            status.active = true;
            status.followWindows = state.follow;
            status.currentColor = FormatColor(state.color);
            status.detail = L"已应用可恢复补丁，原文件备份完整保留。";
        } else {
            status.currentColor = L"#00A86F";
            status.detail = status.supported ? L"已检测到经过验证的兼容版本。" : L"未安装微信输入法，或当前版本暂不受支持。";
        }
    } catch (const std::exception& error) {
        status.detail = WidenError(error);
    }
    return status;
}

OperationResult ApplyColor(std::uint32_t rgb, bool followWindows) {
    try {
        if (!IsProcessElevated()) Fail(L"Administrator privileges are required.");
        std::wstring version;
        const auto target = TargetPath(&version);
        const auto installRoot = InstallRoot();
        if (version != kSupportedVersion) Fail(L"Installed WeType version is not supported.");
        StopWeType(installRoot);
        State state;
        State previousState;
        bool hadActive = false;
        bool replaced = false;
        try {
            if (fs::exists(ActiveStatePath())) {
                state = LoadState();
                previousState = state;
                hadActive = true;
                if (fs::weakly_canonical(state.target) != fs::weakly_canonical(target) ||
                    !fs::exists(state.backup) || !StartsWithPath(state.backup, ProgramDataRoot()) ||
                    !ValidateCurrentOrOriginal(state)) {
                    Fail(L"Existing recovery state is unsafe; no file was replaced.");
                }
                VerifyOriginal(state.backup);
            } else {
                VerifyOriginal(target);
                const auto recoveryDirectory = ProgramDataRoot() / L"Backups" /
                    (Timestamp() + L"-" + NewGuid());
                state.target = target;
                state.backup = recoveryDirectory / L"app.so.original";
                state.created = Timestamp();
                state.phase = L"BackupCreated";
                CopyDurable(target, state.backup, true);
                VerifyOriginal(state.backup);
            }
            state.color = rgb & 0xFFFFFFu;
            state.follow = followWindows;
            const auto staged = target.parent_path() /
                (L".app.so.accentime-stage-" + NewGuid());
            GeneratePatchedCopy(state.backup, staged, state.color);
            state.phase = L"Prepared";
            SaveState(state);
            WriteManifest(state.backup.parent_path(), state);
            try {
                AtomicReplace(target, staged);
                replaced = true;
            } catch (...) {
                DeleteFileW(staged.c_str());
                throw;
            }
            state.phase = L"Applied";
            SaveState(state);
            WriteManifest(state.backup.parent_path(), state);
        } catch (...) {
            if (replaced) {
                EmergencyRestore(state);
            } else if (hadActive) {
                try {
                    SaveState(previousState);
                    WriteManifest(previousState.backup.parent_path(), previousState);
                } catch (...) { }
            } else {
                RemoveActiveState();
            }
            StartService();
            throw;
        }
        StartService();
        if (!gWatcherContext) {
            if (followWindows) {
                try {
                    ConfigureTask(true);
                } catch (...) {
                    auto saved = LoadState();
                    saved.follow = false;
                    SaveState(saved);
                    WriteManifest(saved.backup.parent_path(), saved);
                    return {true, L"Color applied, but Windows accent following could not be enabled."};
                }
            } else {
                ConfigureTask(false);
            }
        }
        return {true, L"Theme color applied: " + FormatColor(rgb)};
    } catch (const std::exception& error) {
        return ErrorResult(error);
    }
}

OperationResult RestoreOriginal(bool disableFollow) {
    try {
        if (!IsProcessElevated()) Fail(L"Administrator privileges are required.");
        if (disableFollow) ConfigureTask(false);
        if (!fs::exists(ActiveStatePath())) return {true, L"No active patch was found."};
        const auto state = LoadState();
        const auto installRoot = InstallRoot();
        const auto expectedTarget = TargetPath();
        if (fs::weakly_canonical(state.target) != fs::weakly_canonical(expectedTarget) ||
            !StartsWithPath(state.backup, ProgramDataRoot()) || !fs::exists(state.backup)) {
            Fail(L"Recovery state points outside the allowed locations.");
        }
        VerifyOriginal(state.backup);
        StopWeType(installRoot);
        try {
            if (!EqualsNoCase(Sha256File(state.target), kOriginalSha256)) {
                try {
                    ValidatePatchedCopy(state.backup, state.target, state.color);
                } catch (...) {
                    try {
                        // Accept 0.3/0.4 state during upgrade and restore.
                        ValidatePatchedCopyImpl(state.backup, state.target, state.color, true, false);
                    } catch (...) {
                        // Accept candidate-only 0.2.x state during upgrade and restore.
                        ValidatePatchedCopyImpl(state.backup, state.target, state.color, false, false);
                    }
                }
                const auto staged = state.target.parent_path() /
                    (L".app.so.accentime-restore-" + NewGuid());
                CopyDurable(state.backup, staged, true);
                AtomicReplace(state.target, staged);
            }
            RemoveActiveState();
        } catch (...) {
            StartService();
            throw;
        }
        StartService();
        return {true, L"The original WeType file was restored."};
    } catch (const std::exception& error) {
        return ErrorResult(error);
    }
}

OperationResult SetFollowMode(bool enabled) {
    try {
        if (!IsProcessElevated()) Fail(L"Administrator privileges are required.");
        if (enabled) {
            const auto color = ReadWindowsAccent();
            return ApplyColor(color, true);
        }
        ConfigureTask(false);
        if (fs::exists(ActiveStatePath())) {
            auto state = LoadState();
            state.follow = false;
            SaveState(state);
            WriteManifest(state.backup.parent_path(), state);
        }
        return {true, L"Windows accent following is disabled."};
    } catch (const std::exception& error) {
        return ErrorResult(error);
    }
}

int WatchWindowsAccent() {
    gWatcherContext = true;
    UniqueHandle mutex(CreateMutexW(nullptr, TRUE, L"Local\\AccentIME.ThemeWatcher.Singleton"));
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    HKEY rawKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0,
                      KEY_QUERY_VALUE | KEY_NOTIFY, &rawKey) != ERROR_SUCCESS) return 2;
    UniqueReg key(rawKey);
    UniqueHandle change(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    UniqueHandle stop(CreateEventW(nullptr, TRUE, FALSE, L"Local\\AccentIME.ThemeWatcher.Stop"));
    if (!change || !stop) return 3;
    std::uint32_t last = 0xFFFFFFFFu;
    for (;;) {
        try {
            const auto state = QueryStatus();
            if (!state.followWindows) return 0;
            const auto color = ReadWindowsAccent();
            if (color != last && (!state.active || !EqualsNoCase(state.currentColor, FormatColor(color)))) {
                const auto result = ApplyColor(color, true);
                if (!result.ok) AppendWatcherLog(result.message);
            }
            last = color;
        } catch (const std::exception& error) {
            AppendWatcherLog(WidenError(error));
        }
        if (RegNotifyChangeKeyValue(key.get(), FALSE, REG_NOTIFY_CHANGE_LAST_SET,
                                    change.get(), TRUE) != ERROR_SUCCESS) return 4;
        HANDLE events[]{stop.get(), change.get()};
        const auto wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) return 0;
        if (wait != WAIT_OBJECT_0 + 1) return 5;
        Sleep(500);
    }
}

int UninstallCleanup() {
    ConfigureTask(false);
    const auto restored = RestoreOriginal(false);
    if (!restored.ok) {
        AppendWatcherLog(L"Uninstall cleanup failed: " + restored.message);
        return 1;
    }
    return 0;
}

bool IsProcessElevated() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return false;
    UniqueHandle token(rawToken);
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    return GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &bytes) &&
           elevation.TokenIsElevated != 0;
}

fs::path ExecutablePath() {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) Fail(L"Unable to locate the executable.");
    return fs::path(std::wstring(buffer.data(), length));
}

}  // namespace accentime
