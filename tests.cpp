#include "Core.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <type_traits>

namespace fs = std::filesystem;

int wmain(int argc, wchar_t** argv) {
    try {
        if (accentime::EncodeColor(0x00A86F) != accentime::kOriginalColorBytes) {
            throw std::runtime_error("original encoding mismatch");
        }
        const auto white = accentime::EncodeColor(0xFFFFFF);
        const std::array<std::uint8_t, 5> expectedWhite{0x7F, 0x7F, 0x7F, 0xBF, 0xC0};
        if (white != expectedWhite) throw std::runtime_error("white encoding mismatch");
        std::uint32_t parsed = 0;
        if (!accentime::ParseColor(L"#123456", parsed) || parsed != 0x123456 ||
            accentime::ParseColor(L"#12345Z", parsed)) throw std::runtime_error("color parser mismatch");
        const auto voiceBlue = accentime::VoiceGradient(0x0078D4);
        if (voiceBlue[0] != 0x273D4Eu || voiceBlue[1] != 0x202C36u) {
            throw std::runtime_error("voice gradient mismatch");
        }
        const auto voiceWhite = accentime::VoiceGradient(0xFFFFFF);
        if (voiceWhite[0] > 0x606060 || voiceWhite[1] > 0x606060) {
            throw std::runtime_error("voice contrast clamp mismatch");
        }
        if (argc == 3) {
            const fs::path source(argv[1]);
            const fs::path output(argv[2]);
            accentime::GeneratePatchedCopy(source, output, 0x0078D4);
            accentime::ValidatePatchedCopy(source, output, 0x0078D4);
            if (fs::file_size(source) != fs::file_size(output)) throw std::runtime_error("file size changed");
            std::ifstream patched(output, std::ios::binary);
            if (!patched) throw std::runtime_error("patched copy unavailable");
            const auto check = [&patched](std::uint64_t offset, const auto& expected) {
                using Value = std::decay_t<decltype(expected)>;
                Value actual{};
                patched.seekg(static_cast<std::streamoff>(offset));
                patched.read(reinterpret_cast<char*>(actual.data()),
                             static_cast<std::streamsize>(actual.size()));
                if (actual != expected) throw std::runtime_error("binary patch mismatch");
            };
            check(accentime::kVoiceBubbleColorCodeOffset, accentime::kPatchedVoiceBubbleColorCode);
            check(accentime::kVoiceBubbleRadiusOperandOffset, accentime::kPatchedVoiceBubbleRadiusOperand);
            check(accentime::kBubbleTailPainterCodeOffset, accentime::kPatchedBubbleTailPainterCode);
        }
        std::cout << "AccentIME native core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
