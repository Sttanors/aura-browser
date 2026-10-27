#include "core/browser/profile.hpp"
#include "core/browser/spelling.hpp"
#include <chrono>
#include <gtest/gtest.h>
using namespace aura;
TEST(Spelling, RejectsMalformedUtf8AndInvisibleWordSeparators) {
    for (const auto &word :
         {std::string{}, std::string("two words"), std::string("word\nnext"),
          std::string("x\0y", 3), std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
          std::string("\xf4\x90\x80\x80"), std::string("\xe2\x82"),
          std::string("a\xc2\xa0"
                      "b"),
          std::string("a\xe2\x80\x8b"
                      "b"),
          std::string("a\xe2\x80\xae"
                      "b")})
        EXPECT_FALSE(valid_spelling_word(word));
    EXPECT_TRUE(valid_spelling_word("AuraBrowser"));
    EXPECT_TRUE(valid_spelling_word("Аурабраузер"));
    EXPECT_TRUE(valid_spelling_word("don't"));
    EXPECT_TRUE(valid_spelling_word(std::string(99, 'a')));
    EXPECT_FALSE(valid_spelling_word(std::string(100, 'a')));
}
TEST(Spelling, BoundedUniqueWordsAndDeletionDoNotChangeOtherEntries) {
    std::vector<std::string> words;
    ASSERT_TRUE(add_spelling_word(words, "Аурабраузер"));
    EXPECT_FALSE(add_spelling_word(words, "Аурабраузер"));
    EXPECT_FALSE(add_spelling_word(words, "two words"));
    for (int i = 1; i < int(spelling_word_limit); ++i)
        ASSERT_TRUE(add_spelling_word(words, "word" + std::to_string(i)));
    EXPECT_FALSE(add_spelling_word(words, "overflow"));
    EXPECT_EQ(words.size(), spelling_word_limit);
    std::erase(words, "Аурабраузер");
    EXPECT_TRUE(add_spelling_word(words, "replacement"));
    EXPECT_EQ(words.size(), spelling_word_limit);
}
TEST(Spelling, SavedWordsAndLanguagePreferenceSurviveProfileRoundTrip) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aura-spelling-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Profile p(path);
    ASSERT_TRUE(add_spelling_word(p.spelling_words, "Аурабраузер"));
    ASSERT_TRUE(add_spelling_word(p.spelling_words, "AuraBrowser"));
    const auto spec = find_setting("spellcheck_languages");
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(set_setting(p, *spec, "ru"));
    EXPECT_FALSE(set_setting(p, *spec, "unsupported"));
    p.spellcheck = false;
    p.save();
    Profile q(path);
    q.load();
    EXPECT_EQ(q.spelling_words, p.spelling_words);
    EXPECT_FALSE(q.spellcheck);
    EXPECT_EQ(q.spellcheck_languages, "ru");
    std::erase(q.spelling_words, "AuraBrowser");
    q.save();
    Profile r(path);
    r.load();
    EXPECT_EQ(r.spelling_words, std::vector<std::string>{"Аурабраузер"});
    std::filesystem::remove_all(path);
}
