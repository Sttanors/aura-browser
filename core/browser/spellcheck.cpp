#include "common/config.hpp"
#include "core/browser/browser.hpp"
#include "core/browser/spelling.hpp"
#include "include/cef_parser.h"
#include "ui/browser_ui.hpp"
#include <algorithm>
namespace aura {
std::string BrowserController::spelling_status() const {
    auto context = CefRequestContext::GetGlobalContext();
    if (!context)
        return "null";
    auto enabled = context->GetPreference("browser.enable_spellchecking");
    auto remote = context->GetPreference("spellcheck.use_spelling_service");
    auto languages = context->GetPreference("spellcheck.dictionaries");
    return "{\"enabled\":" + std::string(enabled && enabled->GetBool() ? "true" : "false") +
           ",\"remote\":" + (remote && remote->GetBool() ? "true" : "false") + ",\"languages\":" +
           (languages ? CefWriteJSON(languages, JSON_WRITER_DEFAULT).ToString() : "null") +
           ",\"saved_words\":" + std::to_string(profile.spelling_words.size()) +
           ",\"private_words\":" + std::to_string(state_->private_spelling_words.size()) + "}";
}
void BrowserController::prepare_spelling_resources() {
    // Chrome uses DIR_USER_DATA/Dictionaries on Linux and DIR_EXE/Dictionaries on Windows.
    // The runtime includes the latter; seed the writable root before CEF starts on Linux.
    try {
        const auto source = resources / "Dictionaries";
        for (const char *name : {"en-US-10-2.bdic", "ru-RU-3-0.bdic"}) {
            if (!std::filesystem::is_regular_file(source / name))
                return;
#ifndef _WIN32
            auto destination = profile.directory / "cache/Dictionaries";
            std::filesystem::create_directories(destination);
            std::filesystem::copy_file(source / name, destination / name,
                                       std::filesystem::copy_options::overwrite_existing);
#endif
        }
        state_->spelling_resources_ready = true;
    } catch (const std::exception &) {
        state_->spelling_resources_ready = false;
    }
}
void BrowserController::configure_spelling(CefRefPtr<CefRequestContext> context) {
    if (!context)
        return;
    auto set = [&](const char *name, CefRefPtr<CefValue> value) {
        CefString error;
        if (!context->HasPreference(name) || !context->SetPreference(name, value, error))
            ++audit["spelling_preference_errors"];
    };
    auto remote = CefValue::Create();
    remote->SetBool(false);
    set("spellcheck.use_spelling_service", remote);
    auto languages = CefListValue::Create();
    size_t n = 0;
    if (profile.spellcheck_languages != "en")
        languages->SetString(n++, "ru-RU");
    if (profile.spellcheck_languages != "ru")
        languages->SetString(n++, "en-US");
    auto value = CefValue::Create();
    value->SetList(languages);
    set("spellcheck.dictionaries", value);
    auto enabled = CefValue::Create();
    enabled->SetBool(profile.spellcheck && state_->spelling_resources_ready);
    set("browser.enable_spellchecking", enabled);
}
std::vector<std::string> BrowserController::spelling_words(bool private_mode) const {
    auto words = profile.spelling_words;
    if (private_mode)
        for (const auto &word : state_->private_spelling_words)
            if (std::find(words.begin(), words.end(), word) == words.end())
                words.push_back(word);
    return words;
}
namespace {
std::string words_json(const std::vector<std::string> &words) {
    std::string json = "[";
    for (const auto &word : words) {
        if (json.size() > 1)
            json += ',';
        json += json_quote(word);
    }
    return json + ']';
}
} // namespace
void BrowserController::setup_spelling(Tab &t) {
    if (safe_mode())
        return;
    const auto words = words_json(spelling_words(t.private_tab));
    const int id = t.id;
    const auto generation = ++t.spelling_generation;
    auto weak = std::weak_ptr<void>(callback_lifetime);
    t.devtools->call(
        "Page.addScriptToEvaluateOnNewDocument",
        "{\"worldName\":\"Aura Spelling\",\"runImmediately\":true,\"source\":" +
            json_quote(read_resource(resources / "protection/spelling.js") +
                       "\nglobalThis.__auraSpellingApply(" + words + ");") +
            "}",
        [this, id, weak, generation](bool success, const std::string &result) {
            if (weak.expired())
                return;
            if (auto t = tab(id); t && success) {
                auto v = CefParseJSON(result, JSON_PARSER_RFC);
                if (!v || v->GetType() != VTYPE_DICTIONARY)
                    return;
                const auto script = v->GetDictionary()->GetString("identifier").ToString();
                if (generation != t->spelling_generation) {
                    if (t->devtools && !script.empty())
                        t->devtools->call("Page.removeScriptToEvaluateOnNewDocument",
                                          "{\"identifier\":" + json_quote(script) + "}");
                    return;
                }
                t->spelling_script = script;
                update_spelling(*t, false);
            }
        });
}
void BrowserController::update_spelling(Tab &t, bool future) {
    if (safe_mode())
        return;
    if (!t.devtools)
        return;
    const auto words = words_json(spelling_words(t.private_tab));
    const int id = t.id;
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    for (int context : t.spelling_contexts)
        t.devtools->call(
            "Runtime.evaluate",
            "{\"contextId\":" + std::to_string(context) +
                ",\"returnByValue\":true,\"expression\":" +
                json_quote("globalThis.__auraSpellingApply?.(" + words + ") === true") + "}",
            [this, id, weak, context](bool ok, const std::string &result) {
                if (weak.expired())
                    return;
                auto v = CefParseJSON(result, JSON_PARSER_RFC);
                auto d = v && v->GetType() == VTYPE_DICTIONARY
                             ? v->GetDictionary()->GetDictionary("result")
                             : nullptr;
                if (auto t = tab(id);
                    t && t->spelling_contexts.contains(context) && ok && d && d->GetBool("value"))
                    t->spelling_supported = true;
            });
    if (future) {
        if (!t.spelling_script.empty())
            t.devtools->call("Page.removeScriptToEvaluateOnNewDocument",
                             "{\"identifier\":" + json_quote(t.spelling_script) + "}");
        t.spelling_script.clear();
        setup_spelling(t);
    }
}
bool BrowserController::change_spelling_word(const std::string &word, bool add, bool private_mode) {
    if (safe_mode())
        return false;
    if (!valid_spelling_word(word))
        return false;
    if (private_mode) {
        bool alive = false;
        for (const auto *window : state_->windows)
            for (const auto &t : window->tabs)
                alive |= t->private_tab && !t->page.closed && !t->closing;
        if (!alive)
            return false;
    }
    auto &words = private_mode ? state_->private_spelling_words : profile.spelling_words;
    const auto all = spelling_words(private_mode);
    if (add && std::find(all.begin(), all.end(), word) != all.end())
        return false;
    if (add ? !add_spelling_word(words, word) : !std::erase(words, word))
        return false;
    for (auto *window : state_->windows)
        for (auto &t : window->tabs)
            if (!private_mode || t->private_tab)
                window->update_spelling(*t);
    changed(!private_mode);
    return true;
}
} // namespace aura
