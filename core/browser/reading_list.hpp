#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace aura {
struct ArticleBlock {
    std::string tag, text;
    bool operator==(const ArticleBlock &) const = default;
};
struct SavedArticle {
    int id = 0;
    std::string url, title, byline;
    long long saved = 0;
    bool read = false;
    std::vector<ArticleBlock> blocks;
    bool operator==(const SavedArticle &) const = default;
};
bool valid_article(const SavedArticle &);
std::string offline_article_html(const SavedArticle &, bool dark, int size, int width, bool serif);
class ReadingList {
  public:
    explicit ReadingList(std::filesystem::path directory);
    const std::vector<SavedArticle> &items() const { return items_; }
    const SavedArticle *find(int id) const;
    std::vector<int> search(const std::string &) const;
    bool save(SavedArticle article); // Same URL replaces the copy, preserving read state.
    bool mark(int id, bool read);
    bool remove(int id);
    uint64_t revision() const { return revision_; }
    bool available() const { return available_; }

  private:
    std::filesystem::path directory_;
    std::vector<SavedArticle> items_;
    bool available_ = true;
    uint64_t revision_ = 0;
    int next_id_ = 1;
    bool commit(std::vector<SavedArticle>);
};
} // namespace aura
