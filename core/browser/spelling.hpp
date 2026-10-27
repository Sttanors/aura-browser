#pragma once
#include <cstddef>
#include <string>
#include <vector>
namespace aura {
inline constexpr std::size_t spelling_word_limit = 500;
bool valid_spelling_word(const std::string &word);
bool add_spelling_word(std::vector<std::string> &words, const std::string &word);
} // namespace aura
