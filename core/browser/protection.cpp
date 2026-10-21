#include "core/browser/protection.hpp"
#include "common/config.hpp"
#include "core/browser/profile.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
extern "C" {
void *aura_adblock_new(const char *);
void aura_adblock_free(void *);
bool aura_adblock_stats(const char *, uint64_t *, uint64_t *, uint64_t *);
bool aura_adblock_check(void *, const char *, const char *, const char *);
int aura_adblock_decision(void *, const char *, const char *, const char *, const char *, char **);
char *aura_adblock_cosmetics(void *, const char *, const char *, const char *);
void aura_adblock_string_free(char *);
}
namespace aura {
std::string read_resource(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
std::string json_quote(const std::string &value) {
    return "\"" + json_escape(value) + "\"";
}
namespace {
std::string site_host(const std::string &url) {
    auto host = url_host(url);
    if (auto at = host.find_last_of('@'); at != std::string::npos)
        host.erase(0, at + 1);
    if (host.starts_with("[")) {
        auto end = host.find(']');
        if (end != std::string::npos)
            host.resize(end + 1);
    } else if (auto colon = host.find(':'); colon != std::string::npos)
        host.resize(colon);
    for (auto &c : host)
        c = std::tolower(static_cast<unsigned char>(c));
    if (host.ends_with('.'))
        host.pop_back();
    return host;
}
std::vector<std::string> domains(std::string text) {
    for (auto &c : text) {
        if (c == ',' || c == ';')
            c = ' ';
        else
            c = std::tolower(static_cast<unsigned char>(c));
    }
    std::istringstream in(text);
    std::vector<std::string> out;
    std::string word;
    while (in >> word)
        out.push_back(word);
    return out;
}
} // namespace
bool site_exception(const std::string &url, const std::string &exceptions) {
    auto host = site_host(url);
    for (const auto &d : domains(exceptions))
        if (host == d || (host.size() > d.size() && host.ends_with("." + d)))
            return true;
    return false;
}
std::string toggle_site_exception(const std::string &url, const std::string &exceptions) {
    auto list = domains(exceptions);
    auto host = site_host(url);
    if (site_exception(url, exceptions))
        std::erase_if(list, [&](const auto &d) { return host == d || host.ends_with("." + d); });
    else if (!host.empty())
        list.push_back(host);
    std::string out;
    for (const auto &d : list) {
        if (!out.empty())
            out += ", ";
        out += d;
    }
    return out;
}
Protection::Protection(const std::filesystem::path &root) {
    auto rules = read_resource(root / "protection/easylist.txt");
    if (rules.empty())
        return;
    rules += '\n';
    rules += read_resource(root / "protection/easyprivacy.txt");
    install(prepare(rules));
}
Protection::Prepared::~Prepared() {
    if (handle)
        aura_adblock_free(handle);
}
Protection::RuleStats Protection::inspect(const std::string &rules) {
    RuleStats stats;
    if (rules.find('\0') != std::string::npos ||
        !aura_adblock_stats(rules.c_str(), &stats.network, &stats.cosmetic, &stats.skipped))
        throw std::runtime_error("Filter rules must be valid UTF-8 text.");
    return stats;
}
std::shared_ptr<Protection::Prepared> Protection::prepare(const std::string &rules) {
    auto result = std::make_shared<Prepared>();
    result->stats = inspect(rules);
    result->handle = aura_adblock_new(rules.c_str());
    if (!result->handle)
        throw std::runtime_error("Cannot compile the filter rules.");
    return result;
}
void Protection::install(std::shared_ptr<Prepared> rules) {
    if (!rules || !rules->handle)
        throw std::runtime_error("Invalid filter engine.");
    std::shared_ptr<Prepared> previous;
    {
        std::lock_guard lock(mutex_);
        previous = std::move(compiled_);
        compiled_ = std::move(rules);
        engine_ = compiled_->handle;
    }
}
Protection::~Protection() = default;
void Protection::configure(bool enabled, bool cosmetics, std::string exceptions) {
    std::lock_guard lock(mutex_);
    enabled_ = enabled;
    cosmetics_ = cosmetics;
    exceptions_ = std::move(exceptions);
}
bool Protection::active(const std::string &url) const {
    std::lock_guard lock(mutex_);
    return engine_ && enabled_ && !site_exception(url, exceptions_);
}
Protection::Decision Protection::decide(const std::string &url, const std::string &source, int type,
                                        const std::string &method) {
    // Never block top-level navigation, browser/extension pages or local files.
    if (type == 0 || !(url.starts_with("http://") || url.starts_with("https://")) ||
        !(source.starts_with("http://") || source.starts_with("https://")))
        return {};
    static const char *types[] = {"document", "subdocument", "stylesheet", "script", "image",
                                  "font",     "other",       "object",     "media",  "other",
                                  "other",    "other",       "image",      "xhr",    "ping",
                                  "other",    "other",       "other",      "other",  "other"};
    std::lock_guard lock(mutex_);
    if (!engine_ || !enabled_ || site_exception(source, exceptions_))
        return {};
    char *rewritten = nullptr;
    const int result = aura_adblock_decision(engine_, url.c_str(), source.c_str(),
                                             type >= 0 && type < 20 ? types[type] : "other",
                                             method.c_str(), &rewritten);
    Decision decision;
    if (result >= 1 && result <= 4) {
        decision.action = static_cast<Action>(result);
        ++blocked;
    }
    if (rewritten) {
        std::string changed(rewritten);
        aura_adblock_string_free(rewritten);
        const auto base = url.substr(0, url.find_first_of("?#"));
        if (decision.action == Action::Allow && changed != url && changed.size() <= 32768 &&
            changed.substr(0, changed.find_first_of("?#")) == base) {
            decision.rewritten = std::move(changed);
            ++rewritten_requests;
        }
    }
    return decision;
}
bool Protection::block(const std::string &url, const std::string &source, int type) {
    return decide(url, source, type).action != Action::Allow;
}
std::string Protection::selectors(const std::string &url, const std::string &classes,
                                  const std::string &ids) {
    std::lock_guard lock(mutex_);
    if (!engine_ || !enabled_ || !cosmetics_ || site_exception(url, exceptions_))
        return "[]";
    char *result = aura_adblock_cosmetics(engine_, url.c_str(), classes.c_str(), ids.c_str());
    std::string out = result ? result : "[]";
    aura_adblock_string_free(result);
    return out;
}
} // namespace aura
