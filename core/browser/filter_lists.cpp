#include "core/browser/filter_lists.hpp"
#include "common/config.hpp"
#include "common/platform.hpp"
#include <algorithm>
#include <ctime>
#include <fstream>
#include <openssl/evp.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <toml++/toml.hpp>
namespace aura {
namespace {
constexpr size_t list_limit = 8 * 1024 * 1024, custom_limit = 1024 * 1024;
const std::array<std::string, 2> urls = {
    "https://raw.githubusercontent.com/easylist/easylist/gh-pages/easylist.txt",
    "https://raw.githubusercontent.com/easylist/easylist/gh-pages/easyprivacy.txt"};
std::string bounded_read(const std::filesystem::path &path, size_t limit) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        throw std::runtime_error("Cannot read the filter list file.");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot read the filter list file.");
    std::string bytes;
    char chunk[32768];
    while (in) {
        in.read(chunk, sizeof chunk);
        if (bytes.size() + size_t(in.gcount()) > limit)
            throw std::runtime_error("The filter list exceeds its size limit.");
        bytes.append(chunk, size_t(in.gcount()));
    }
    if (!in.eof())
        throw std::runtime_error("Cannot read the filter list file.");
    return bytes;
}
void write_file(const std::filesystem::path &path, const std::string &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("Cannot save filter lists. Previous rules are kept.");
#ifndef _WIN32
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
#endif
    out.write(bytes.data(), std::streamsize(bytes.size()));
    out.flush();
    if (!out)
        throw std::runtime_error("Cannot save filter lists. Previous rules are kept.");
}
std::string digest(const std::string &bytes) {
    unsigned char hash[32];
    unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), hash, &size, EVP_sha256(), nullptr) != 1 ||
        size != 32)
        throw std::runtime_error("Cannot verify the filter list checksum.");
    std::string text;
    constexpr char hex[] = "0123456789abcdef";
    for (auto c : hash) {
        text += hex[c >> 4];
        text += hex[c & 15];
    }
    return text;
}
bool generation_name(const std::string &name) {
    return name.starts_with("generation-") && name.size() > 11 && name.size() < 44 &&
           std::all_of(name.begin() + 11, name.end(), [](char c) { return c >= '0' && c <= '9'; });
}
void bounded_lines(const std::string &rules, size_t maximum, size_t line_limit = 4096) {
    size_t count = 0, start = 0;
    while (start < rules.size()) {
        auto end = rules.find('\n', start);
        if (end == std::string::npos)
            end = rules.size();
        if (++count > maximum || end - start > line_limit)
            throw std::runtime_error("The filter list has too many or excessively long rules.");
        start = end + 1;
    }
}
std::string compose(const FilterLists::Settings &settings,
                    const std::array<std::string, 2> &lists) {
    std::string rules;
    for (size_t i = 0; i < 2; ++i)
        if (settings.enabled[i]) {
            rules += lists[i];
            rules += '\n';
        }
    return rules + settings.custom;
}
std::string normalized_custom(std::string text) {
    if (text.size() > custom_limit * 2)
        throw std::runtime_error("Custom filters exceed 1 MB.");
    if (text.starts_with("\xEF\xBB\xBF"))
        text.erase(0, 3);
    bounded_lines(text, 20002);
    std::set<std::string> seen;
    std::string result, line;
    std::istringstream input(text);
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t\r"), last = line.find_last_not_of(" \t\r");
        if (first == std::string::npos)
            continue;
        auto rule = line.substr(first, last - first + 1);
        if (seen.insert(rule).second)
            result += rule + '\n';
    }
    if (result.size() > custom_limit)
        throw std::runtime_error("Custom filters exceed 1 MB.");
    bounded_lines(result, 10000);
    if (Protection::inspect(result).skipped)
        throw std::runtime_error(
            "Some custom rules are invalid or unsupported. Previous rules are kept.");
    return result;
}
FilterLists::Settings read_settings(const std::filesystem::path &directory,
                                    std::array<std::string, 2> &lists) {
    auto state = toml::parse(bounded_read(directory / "state.toml", 65536));
    FilterLists::Settings settings;
    for (size_t i = 0; i < 2; ++i) {
        const auto key = std::to_string(i);
        lists[i] =
            FilterLists::validate_download(bounded_read(directory / (key + ".txt"), list_limit));
        if (digest(lists[i]) != state["sha" + key].value_or(std::string{}))
            throw std::runtime_error("Saved filter cache is damaged. Bundled lists are active.");
        settings.enabled[i] = state["enabled" + key].value_or(true);
        settings.updated[i] = state["updated" + key].value_or(0LL);
    }
    settings.custom = normalized_custom(bounded_read(directory / "custom.txt", custom_limit));
    if (digest(settings.custom) != state["custom_sha"].value_or(std::string{}))
        throw std::runtime_error("Saved filter cache is damaged. Bundled lists are active.");
    return settings;
}
class Fetch final : public CefURLRequestClient {
  public:
    explicit Fetch(std::shared_ptr<FilterTransfer> transfer) : transfer_(std::move(transfer)) {}
    void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
        const auto response = request->GetResponse();
        transfer_->ok =
            request->GetRequestStatus() == UR_SUCCESS && response && response->GetStatus() == 200;
        transfer_->complete = true;
    }
    void OnDownloadData(CefRefPtr<CefURLRequest> request, const void *data, size_t size) override {
        if (transfer_->bytes.size() + size > list_limit) {
            request->Cancel();
            return;
        }
        try {
            transfer_->bytes.append(static_cast<const char *>(data), size);
        } catch (...) {
            request->Cancel();
        }
    }
    void OnDownloadProgress(CefRefPtr<CefURLRequest> request, int64_t, int64_t total) override {
        if (total > int64_t(list_limit))
            request->Cancel();
    }
    void OnUploadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
    bool GetAuthCredentials(bool, const CefString &, int, const CefString &, const CefString &,
                            CefRefPtr<CefAuthCallback>) override {
        return false;
    }

  private:
    std::shared_ptr<FilterTransfer> transfer_;
    IMPLEMENT_REFCOUNTING(Fetch);
};
} // namespace
std::string FilterLists::validate_download(std::string bytes) {
    if (bytes.size() > list_limit)
        throw std::runtime_error("The filter list exceeds its size limit.");
    if (bytes.starts_with("\xEF\xBB\xBF"))
        bytes.erase(0, 3);
    if (!bytes.starts_with("[Adblock Plus "))
        throw std::runtime_error("The server did not return an Adblock filter list.");
    bounded_lines(bytes, 200000, 32768);
    const auto stats = Protection::inspect(bytes);
    if (stats.network + stats.cosmetic < 100)
        throw std::runtime_error("The downloaded filter list is incomplete.");
    return bytes;
}
FilterLists::FilterLists(std::filesystem::path profile, std::filesystem::path resources,
                         std::shared_ptr<Protection> protection)
    : root_(std::move(profile) / "filter-lists"), protection_(std::move(protection)) {
    std::filesystem::create_directories(root_);
#ifndef _WIN32
    std::filesystem::permissions(root_, std::filesystem::perms::owner_all);
#endif
    bundled_ = {bounded_read(resources / "protection/easylist.txt", list_limit),
                bounded_read(resources / "protection/easyprivacy.txt", list_limit)};
    lists_ = bundled_;
    stats = protection_->rule_stats();
    try {
        auto state = toml::parse(bounded_read(root_ / "check.toml", 65536));
        last_attempt_ = state["last_attempt"].value_or(0LL);
        if (last_attempt_ > std::time(nullptr))
            last_attempt_ = 0;
    } catch (const std::exception &) {
    }
    if (std::filesystem::exists(root_ / "active.toml"))
        try {
            auto active = toml::parse(bounded_read(root_ / "active.toml", 65536));
            auto name = active["generation"].value_or(std::string{});
            if (!generation_name(name))
                throw std::runtime_error("Invalid filter cache path.");
            std::array<std::string, 2> saved_lists;
            auto saved = read_settings(root_ / name, saved_lists);
            auto prepared = Protection::prepare(compose(saved, saved_lists));
            protection_->install(prepared);
            stats = prepared->stats;
            settings_ = std::move(saved);
            lists_ = std::move(saved_lists);
            status = "Saved filter lists are active.";
        } catch (const std::exception &) {
            status = "Saved filter cache is damaged. Bundled lists are active.";
        }
    // Select the startup source before compiling. A cached profile must not
    // build the entire bundled engine only to discard it for the saved engine.
    if (!protection_->ready())
        protection_->install(Protection::prepare(compose(settings_, bundled_)));
    stats = protection_->rule_stats();
}
FilterLists::~FilterLists() {
    shutdown();
}
bool FilterLists::busy() const {
    return phase_ != Phase::Idle;
}
bool FilterLists::due(long long now) const {
    if (!now)
        now = std::time(nullptr);
    if (busy() || (last_attempt_ > 0 && now >= last_attempt_ && now - last_attempt_ < 3600))
        return false;
    for (size_t i = 0; i < 2; ++i)
        if (settings_.enabled[i] && (settings_.updated[i] <= 0 || settings_.updated[i] > now ||
                                     now - settings_.updated[i] >= 86400))
            return true;
    return false;
}
void FilterLists::check() {
    if (busy())
        return;
    if (!settings_.enabled[0] && !settings_.enabled[1]) {
        status = "Enable a list before checking for updates.";
        return;
    }
    last_attempt_ = std::time(nullptr);
    try {
        std::ostringstream text;
        text << toml::table{{"last_attempt", last_attempt_}};
        write_file(root_ / "check.toml.tmp", text.str());
        replace_file(root_ / "check.toml.tmp", root_ / "check.toml");
        downloaded_ = lists_;
        downloading_ = 0;
        discarded_ = false;
        phase_ = Phase::Download;
        status = "Downloading filter lists...";
        fetch_next();
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
void FilterLists::fetch_next() {
    while (downloading_ < 2 && !settings_.enabled[downloading_])
        ++downloading_;
    if (downloading_ == 2) {
        auto desired = settings_;
        for (size_t i = 0; i < 2; ++i)
            if (desired.enabled[i])
                desired.updated[i] = std::time(nullptr);
        request_ = nullptr;
        transfer_.reset();
        start_build(std::move(desired), std::move(downloaded_));
        return;
    }
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    if (fetch_override_) {
        transfer_ = fetch_override_(urls[downloading_]);
        return;
    }
    transfer_ = std::make_shared<FilterTransfer>();
    auto request = CefRequest::Create();
    request->SetURL(urls[downloading_]);
    request->SetFlags(UR_FLAG_DISABLE_CACHE | UR_FLAG_STOP_ON_REDIRECT | UR_FLAG_NO_RETRY_ON_5XX);
    request_ = CefURLRequest::Create(request, new Fetch(transfer_), nullptr);
    if (!request_)
        throw std::runtime_error("Cannot download filter lists. Previous rules are kept.");
}
FilterLists::Build FilterLists::build(const std::filesystem::path &root, Settings settings,
                                      std::array<std::string, 2> lists,
                                      const std::filesystem::path &import) {
    if (!import.empty())
        settings.custom += "\n" + normalized_custom(bounded_read(import, custom_limit));
    settings.custom = normalized_custom(std::move(settings.custom));
    for (auto &list : lists)
        list = validate_download(std::move(list));
    auto prepared = Protection::prepare(compose(settings, lists));
    const auto directory =
        root / ("generation-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("Cannot save filter lists. Previous rules are kept.");
    try {
#ifndef _WIN32
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
#endif
        toml::table state{{"schema", 1}, {"custom_sha", digest(settings.custom)}};
        for (size_t i = 0; i < 2; ++i) {
            auto key = std::to_string(i);
            write_file(directory / (key + ".txt"), lists[i]);
            state.insert("enabled" + key, settings.enabled[i]);
            state.insert("updated" + key, settings.updated[i]);
            state.insert("sha" + key, digest(lists[i]));
        }
        write_file(directory / "custom.txt", settings.custom);
        std::ostringstream data;
        data << state;
        write_file(directory / "state.toml", data.str());
        return {std::move(settings), std::move(lists), std::move(prepared), directory};
    } catch (...) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        throw;
    }
}
void FilterLists::start_build(Settings settings, std::array<std::string, 2> lists,
                              std::filesystem::path import) {
    phase_ = Phase::Build;
    discarded_ = false;
    status = "Compiling filter rules...";
    try {
        work_ = std::async(std::launch::async,
                           [root = root_, settings = std::move(settings), lists = std::move(lists),
                            import = std::move(import)]() mutable {
                               return build(root, std::move(settings), std::move(lists), import);
                           });
    } catch (...) {
        fail("Cannot compile filter rules. Previous rules are kept.");
    }
}
void FilterLists::set_list(size_t index, bool enabled) {
    if (busy() || index >= 2 || settings_.enabled[index] == enabled)
        return;
    auto desired = settings_;
    desired.enabled[index] = enabled;
    start_build(std::move(desired), lists_);
}
void FilterLists::set_custom(std::string rules) {
    if (busy())
        return;
    auto desired = settings_;
    desired.custom = std::move(rules);
    start_build(std::move(desired), lists_);
}
void FilterLists::import_custom(const std::filesystem::path &path) {
    if (busy())
        return;
    start_build(settings_, lists_, path);
}
void FilterLists::restore_bundled() {
    if (busy())
        return;
    auto desired = settings_;
    desired.updated = {0, 0};
    start_build(std::move(desired), bundled_);
}
void FilterLists::cancel() {
    if (!busy())
        return;
    discarded_ = true;
    downloaded_ = {};
    if (request_)
        request_->Cancel();
    request_ = nullptr;
    transfer_.reset();
    if (!work_.valid())
        phase_ = Phase::Idle;
    status = "Filter operation cancelled. Previous rules are kept.";
}
void FilterLists::fail(const std::string &message) {
    cancel();
    phase_ = Phase::Idle;
    status = message;
}
bool FilterLists::tick() {
    if (phase_ == Phase::Download) {
        if (std::chrono::steady_clock::now() >= deadline_) {
            fail("Filter download timed out. Previous rules are kept.");
            return true;
        }
        if (transfer_ && transfer_->complete) {
            if (!transfer_->ok) {
                fail("Cannot download filter lists. Previous rules are kept.");
                return true;
            }
            downloaded_[downloading_] = std::move(transfer_->bytes);
            ++downloading_;
            try {
                fetch_next();
            } catch (const std::exception &e) {
                fail(e.what());
            }
            return true;
        }
    }
    if (work_.valid() && work_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::filesystem::path staging;
        try {
            auto result = work_.get();
            staging = result.staging;
            if (!discarded_) {
                std::string previous;
                try {
                    previous = toml::parse(bounded_read(root_ / "active.toml", 65536))["generation"]
                                   .value_or(std::string{});
                } catch (...) {
                }
                auto name = utf8_path(staging.filename());
                std::ostringstream data;
                data << toml::table{{"generation", name}};
                write_file(root_ / "active.toml.tmp", data.str());
                replace_file(root_ / "active.toml.tmp", root_ / "active.toml");
                protection_->install(result.prepared);
                stats = result.prepared->stats;
                settings_ = std::move(result.settings);
                lists_ = std::move(result.lists);
                ++revision;
                status = "Filter rules applied. Reload pages to recheck network requests.";
                staging.clear();
                std::error_code error;
                for (std::filesystem::directory_iterator it(root_, error), end; !error && it != end;
                     it.increment(error)) {
                    auto file = utf8_path(it->path().filename());
                    std::error_code cleanup;
                    if (generation_name(file) && file != name && file != previous &&
                        it->is_directory(cleanup) && !it->is_symlink(cleanup))
                        std::filesystem::remove_all(it->path(), cleanup);
                }
            }
        } catch (const std::filesystem::filesystem_error &) {
            status = "Cannot save filter lists. Previous rules are kept.";
        } catch (const std::exception &e) {
            if (!discarded_)
                status = e.what();
        }
        if (!staging.empty()) {
            std::error_code error;
            std::filesystem::remove_all(staging, error);
        }
        phase_ = Phase::Idle;
        return true;
    }
    return false;
}
void FilterLists::shutdown() {
    cancel();
    if (work_.valid())
        try {
            auto result = work_.get();
            std::error_code error;
            std::filesystem::remove_all(result.staging, error);
        } catch (...) {
        }
    phase_ = Phase::Idle;
}
std::string FilterLists::report() const {
    return "{\"busy\":" + std::string(busy() ? "true" : "false") +
           ",\"revision\":" + std::to_string(revision) + ",\"status\":" + json_quote(status) +
           ",\"network\":" + std::to_string(stats.network) +
           ",\"cosmetic\":" + std::to_string(stats.cosmetic) +
           ",\"skipped\":" + std::to_string(stats.skipped) +
           ",\"easylist\":" + (settings_.enabled[0] ? "true" : "false") +
           ",\"easyprivacy\":" + (settings_.enabled[1] ? "true" : "false") +
           ",\"updated_easylist\":" + std::to_string(settings_.updated[0]) +
           ",\"updated_easyprivacy\":" + std::to_string(settings_.updated[1]) + "}";
}
} // namespace aura
