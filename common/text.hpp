#pragma once
#include <string>
#include <string_view>
namespace aura {
// Locale-independent case matching for English/Russian. Other UTF-8 is preserved.
inline std::string fold_text(std::string_view input) {
    std::string value(input);
    for (size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c >= 'A' && c <= 'Z')
            value[i] += 32;
        else if (c == 0xd0 && i + 1 < value.size()) {
            const auto next = static_cast<unsigned char>(value[i + 1]);
            if (next >= 0x90 && next <= 0x9f)
                value[++i] = char(next + 32);
            else if (next >= 0xa0 && next <= 0xaf) {
                value[i] = char(0xd1);
                value[++i] = char(next - 32);
            } else if (next == 0x81) {
                value[i] = char(0xd1);
                value[++i] = char(0x91);
            }
        }
    }
    return value;
}
} // namespace aura
