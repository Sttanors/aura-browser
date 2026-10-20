#pragma once
#include <algorithm>
#include <string>
#include <string_view>
namespace aura {
class TextEditor {
  public:
    std::string value;
    std::size_t caret = 0, anchor = 0;
    void set(std::string text, bool select = false) {
        value = std::move(text);
        caret = value.size();
        anchor = select ? 0 : caret;
    }
    std::size_t begin() const {
        return std::min(caret, anchor);
    }
    std::size_t end() const {
        return std::max(caret, anchor);
    }
    std::string selected() const {
        return value.substr(begin(), end() - begin());
    }
    void select_all() {
        anchor = 0;
        caret = value.size();
    }
    void insert(std::string_view text) {
        const auto pos = begin();
        value.replace(pos, end() - pos, text);
        caret = anchor = pos + text.size();
    }
    void erase(bool backwards) {
        if (caret != anchor) {
            insert("");
            return;
        }
        if (backwards && caret) {
            anchor = previous(caret);
            insert("");
        } else if (!backwards && caret < value.size()) {
            anchor = next(caret);
            insert("");
        }
    }
    void move(int direction, bool selecting = false, bool word = false) {
        if (!selecting && caret != anchor && !word)
            caret = direction < 0 ? begin() : end();
        else if (direction < 0) {
            caret = previous(caret);
            if (word)
                while (caret && value[caret - 1] != ' ')
                    caret = previous(caret);
        } else {
            caret = next(caret);
            if (word)
                while (caret < value.size() && value[caret] != ' ')
                    caret = next(caret);
        }
        if (!selecting)
            anchor = caret;
    }
    void edge(bool last, bool selecting = false) {
        caret = last ? value.size() : 0;
        if (!selecting)
            anchor = caret;
    }

  private:
    std::size_t previous(std::size_t p) const {
        if (!p)
            return 0;
        --p;
        while (p && (static_cast<unsigned char>(value[p]) & 0xC0) == 0x80)
            --p;
        return p;
    }
    std::size_t next(std::size_t p) const {
        if (p >= value.size())
            return value.size();
        ++p;
        while (p < value.size() && (static_cast<unsigned char>(value[p]) & 0xC0) == 0x80)
            ++p;
        return p;
    }
};
} // namespace aura
