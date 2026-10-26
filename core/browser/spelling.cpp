#include "core/browser/spelling.hpp"
#include <algorithm>
#include <cstdint>
namespace aura {
bool valid_spelling_word(const std::string &word) {
    if (word.empty() || word.size() > 99)
        return false;
    for (size_t i = 0; i < word.size();) {
        const auto first = static_cast<unsigned char>(word[i++]);
        uint32_t cp = first;
        int count = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            cp = first & 31;
            count = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            cp = first & 15;
            count = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            cp = first & 7;
            count = 3;
            minimum = 0x10000;
        } else if (first >= 0x80)
            return false;
        if (i + count > word.size())
            return false;
        for (int n = 0; n < count; ++n) {
            const auto c = static_cast<unsigned char>(word[i++]);
            if ((c & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp <= 0x20 ||
            (cp >= 0x7f && cp <= 0xa0) || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200f) ||
            (cp >= 0x2028 && cp <= 0x202f) || (cp >= 0x205f && cp <= 0x206f) || cp == 0x3000 ||
            cp == 0xfeff)
            return false;
    }
    return true;
}
bool add_spelling_word(std::vector<std::string> &words, const std::string &word) {
    if (!valid_spelling_word(word) || words.size() >= spelling_word_limit ||
        std::find(words.begin(), words.end(), word) != words.end())
        return false;
    words.push_back(word);
    std::sort(words.begin(), words.end());
    return true;
}
} // namespace aura
