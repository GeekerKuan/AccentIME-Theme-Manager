#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace accentime {

inline constexpr wchar_t kProductName[] = L"AccentIME Theme Manager";
inline constexpr wchar_t kSupportedVersion[] = L"2.1.3.18";
inline constexpr wchar_t kOriginalSha256[] = L"370A675508F6EFCA0D3B41233ADA82AF62312F3A10EE978653F8636EDE25FBFB";
inline constexpr std::uint64_t kColorOffset = 0x12419A;
inline constexpr std::uint64_t kVoiceGradientTopOffset = 0x124132;
inline constexpr std::uint64_t kVoiceGradientBottomOffset = 0x124181;
inline constexpr std::uint64_t kVoiceBubbleColorCodeOffset = 0xA36540;
inline constexpr std::uint64_t kVoiceBubbleRadiusOperandOffset = 0xA37EA5;
inline constexpr std::uint64_t kBubbleTailPainterCodeOffset = 0x7A3E04;
inline constexpr std::array<std::uint8_t, 5> kOriginalColorBytes{0x6F, 0x50, 0x02, 0xB8, 0xC0};
inline constexpr std::array<std::uint8_t, 5> kOriginalVoiceGradientTopBytes{0x0E, 0x29, 0x03, 0xB8, 0xC0};
inline constexpr std::array<std::uint8_t, 5> kOriginalVoiceGradientBottomBytes{0x11, 0x11, 0x0F, 0xB9, 0xC0};
inline constexpr std::array<std::uint8_t, 8> kOriginalVoiceBubbleColorCode{
    0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x10};
inline constexpr std::array<std::uint8_t, 8> kPatchedVoiceBubbleColorCode{
    0x49, 0x8B, 0x87, 0x4F, 0xAF, 0x00, 0x00, 0xC3};
inline constexpr std::array<std::uint8_t, 4> kOriginalVoiceBubbleRadiusOperand{
    0x07, 0x99, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, 4> kPatchedVoiceBubbleRadiusOperand{
    0xA7, 0x82, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, 5> kOriginalBubbleTailPainterCode{
    0x55, 0x48, 0x89, 0xE5, 0x48};
inline constexpr std::array<std::uint8_t, 5> kPatchedBubbleTailPainterCode{
    0x49, 0x8B, 0x46, 0x68, 0xC3};

struct OperationResult {
    bool ok{};
    std::wstring message;
};

struct ProductStatus {
    bool supported{};
    bool active{};
    bool followWindows{};
    std::wstring installedVersion;
    std::wstring currentColor;
    std::wstring detail;
};

std::array<std::uint8_t, 5> EncodeColor(std::uint32_t rgb);
std::array<std::uint32_t, 2> VoiceGradient(std::uint32_t rgb);
std::uint32_t ReadWindowsAccent();
std::wstring FormatColor(std::uint32_t rgb);
bool ParseColor(const std::wstring& text, std::uint32_t& rgb);
std::wstring Sha256File(const std::filesystem::path& path);

void GeneratePatchedCopy(
    const std::filesystem::path& original,
    const std::filesystem::path& output,
    std::uint32_t rgb);
void ValidatePatchedCopy(
    const std::filesystem::path& original,
    const std::filesystem::path& patched,
    std::uint32_t rgb);

ProductStatus QueryStatus();
OperationResult ApplyColor(std::uint32_t rgb, bool followWindows);
OperationResult RestoreOriginal(bool disableFollow);
OperationResult SetFollowMode(bool enabled);

int WatchWindowsAccent();
int UninstallCleanup();
bool IsProcessElevated();
std::filesystem::path ExecutablePath();

}  // namespace accentime
