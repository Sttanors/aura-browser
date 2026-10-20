#include "core/browser/updates.hpp"
#include "common/config.hpp"
#include "common/platform.hpp"
#include "common/update_key.hpp"
#include "include/cef_parser.h"
#include <algorithm>
#include <array>
#include <ctime>
#include <fstream>
#include <openssl/rand.h>
#include <toml++/toml.hpp>
#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#else
#include <spawn.h>
extern char **environ;
#endif
namespace aura {
namespace {
constexpr const char *release_index =
    "https://api.github.com/repos/Sttanors/aura-browser/releases?per_page=20";
std::string channel(const std::string &tag) {
    return "https://github.com/Sttanors/aura-browser/releases/download/" + tag + "/";
}
constexpr const char *network_error = "Could not check for updates. Try again later.";
} // namespace
class UpdateFetch final : public CefURLRequestClient {
  public:
    explicit UpdateFetch(std::weak_ptr<UpdateTransfer> transfer) : transfer_(std::move(transfer)) {}
    void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
        if (auto s = transfer_.lock()) {
            auto response = request->GetResponse();
            s->ok = request->GetRequestStatus() == UR_SUCCESS && response &&
                    response->GetStatus() == 200 &&
                    response->GetURL().ToString().starts_with("https://");
            if (s->file.is_open()) {
                s->file.flush();
                s->ok = s->ok && bool(s->file);
                s->file.close();
            }
            s->complete = true;
        }
    }
    void OnDownloadData(CefRefPtr<CefURLRequest> request, const void *data, size_t size) override {
        if (auto s = transfer_.lock()) {
            if (size > static_cast<unsigned long long>(s->limit - s->received)) {
                request->Cancel();
                return;
            }
            s->received += size;
            if (s->file.is_open()) {
                s->file.write(static_cast<const char *>(data), std::streamsize(size));
                if (!s->file)
                    request->Cancel();
            } else
                s->bytes.append(static_cast<const char *>(data), size);
        }
    }
    void OnDownloadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
    void OnUploadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
    bool GetAuthCredentials(bool, const CefString &, int, const CefString &, const CefString &,
                            CefRefPtr<CefAuthCallback>) override {
        return false;
    }

  private:
    std::weak_ptr<UpdateTransfer> transfer_;
    IMPLEMENT_REFCOUNTING(UpdateFetch);
};
UpdateManager::UpdateManager(std::filesystem::path profile)
    : public_key_(update_public_key), profile_(std::move(profile)) {
    try {
        auto root = toml::parse_file(utf8_path(profile_ / "updates.toml"));
        highest_sequence_ = std::max(browser_release_sequence, root["sequence"].value_or(0LL));
        last_check_ = root["last_check"].value_or(0LL);
    } catch (const std::exception &) {
        highest_sequence_ = browser_release_sequence;
    }
}
UpdateManager::~UpdateManager() {
    shutdown();
}
bool UpdateManager::busy() const {
    return phase_ != Phase::Idle;
}
int UpdateManager::download_percent() const {
    return download_percent_;
}
bool UpdateManager::due() const {
    const auto now = static_cast<long long>(std::time(nullptr));
    return !busy() && ready_file.empty() && (now < last_check_ || now - last_check_ >= 86400);
}
void UpdateManager::save_state() const {
    const auto path = profile_ / "updates.toml.tmp";
    {
        std::ofstream out(path);
        out << toml::table{{"sequence", highest_sequence_}, {"last_check", last_check_}};
        out.flush();
        if (!out)
            throw std::runtime_error("Cannot save update state.");
    }
    replace_file(path, profile_ / "updates.toml");
}
std::string UpdateManager::installation_kind() const {
#ifdef _WIN32
    return std::filesystem::exists(executable_directory() / "unins000.exe") ? "setup" : "portable";
#else
    if (std::filesystem::exists(executable_directory() / ".aura-installed"))
        return "run";
    if (executable_directory() == "/opt/aura-browser")
        return "system";
    return "portable";
#endif
}
void UpdateManager::fetch(const std::string &url, long long limit,
                          const std::filesystem::path &file) {
    deadline_ = std::chrono::steady_clock::now() +
                (file.empty() ? std::chrono::seconds(60) : std::chrono::seconds(1800));
    if (fetch_override_) {
        transfer_ = fetch_override_(url, limit, file);
        return;
    }
    transfer_ = std::make_shared<UpdateTransfer>();
    transfer_->limit = limit;
    if (!file.empty()) {
        transfer_->file.open(file, std::ios::binary | std::ios::trunc);
        if (!transfer_->file)
            throw std::runtime_error("Cannot create the update download.");
    }
    auto req = CefRequest::Create();
    req->SetURL(url);
    req->SetFlags(UR_FLAG_DISABLE_CACHE);
    request_ = CefURLRequest::Create(req, new UpdateFetch(transfer_), nullptr);
    if (!request_)
        throw std::runtime_error(network_error);
}
void UpdateManager::fail(const std::string &message) {
    cancel();
    status = message;
}
void UpdateManager::check() {
    if (busy() || !ready_file.empty())
        return;
    release.reset();
    artifact_.reset();
    last_check_ = std::time(nullptr);
    status = "Checking the signed update channel...";
    try {
        save_state();
        phase_ = Phase::Index;
        fetch(release_index, 512 * 1024);
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
void UpdateManager::download() {
    if (busy() || !release || !ready_file.empty())
        return;
    if (release->expires <= static_cast<long long>(std::time(nullptr))) {
        release.reset();
        status = "The update metadata has expired. Check again.";
        return;
    }
    const auto kind = installation_kind();
    if (kind == "system") {
        status = "Update this installation with your Linux package manager.";
        return;
    }
#ifdef _WIN32
    const std::string platform = "windows-x64";
#else
    const std::string platform = "linux-x64";
#endif
    auto it = std::find_if(release->artifacts.begin(), release->artifacts.end(),
                           [&](const auto &a) { return a.platform == platform && a.kind == kind; });
    if (it == release->artifacts.end()) {
        status = "This release has no update for this installation.";
        return;
    }
    try {
        artifact_ = *it;
        const auto root = profile_ / "updates";
        std::filesystem::create_directories(root);
        std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        std::array<unsigned char, 16> random{};
        if (RAND_bytes(random.data(), random.size()) != 1)
            throw std::runtime_error("Cannot create the update download.");
        std::string name;
        for (auto c : random) {
            name += "0123456789abcdef"[c >> 4];
            name += "0123456789abcdef"[c & 15];
        }
        auto staging = root / name;
        if (!std::filesystem::create_directory(staging))
            throw std::runtime_error("Cannot create the update download.");
        std::filesystem::permissions(staging, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        temporary_ = staging / artifact_->filename;
        download_percent_ = 0;
        phase_ = Phase::Download;
        status = "Downloading the update...";
        fetch(release->download_url(*artifact_), artifact_->size, temporary_);
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
bool UpdateManager::tick() {
    if (phase_ == Phase::Idle)
        return false;
    try {
        if (phase_ == Phase::Verify) {
            if (verification_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return false;
            verification_.get();
            ready_file = temporary_;
            temporary_.clear();
            phase_ = Phase::Idle;
            status = "The update is verified and ready.";
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline_)
            throw std::runtime_error(network_error);
        if (!transfer_ || !transfer_->complete) {
            if (phase_ == Phase::Download && transfer_) {
                const int progress = int(transfer_->received * 100 / std::max(1LL, transfer_->limit));
                if (progress != download_percent_) {
                    download_percent_ = progress;
                    return true;
                }
            }
            return false;
        }
        if (!transfer_->ok)
            throw std::runtime_error(network_error);
        auto bytes = std::move(transfer_->bytes);
        request_ = nullptr;
        transfer_.reset();
        if (phase_ == Phase::Index) {
            auto value = CefParseJSON(bytes, JSON_PARSER_RFC);
            if (!value || value->GetType() != VTYPE_LIST)
                throw std::runtime_error(network_error);
            auto list = value->GetList();
            selected_tag_.clear();
            const bool previews = browser_version.find("preview") != std::string_view::npos;
            for (size_t i = 0; i < list->GetSize(); ++i) {
                auto entry = list->GetDictionary(i);
                if (!entry || entry->GetBool("draft") ||
                    (!previews && entry->GetBool("prerelease")))
                    continue;
                const auto tag = entry->GetString("tag_name").ToString();
                if (tag.size() < 2 || tag.size() > 161 || tag.front() != 'v' ||
                    !std::all_of(tag.begin(), tag.end(), [](unsigned char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
                    }))
                    continue;
                selected_tag_ = tag;
                break;
            }
            if (selected_tag_.empty())
                throw std::runtime_error(network_error);
            phase_ = Phase::Manifest;
            fetch(channel(selected_tag_) + "aura-update.toml", 65536);
        } else if (phase_ == Phase::Manifest) {
            manifest_ = std::move(bytes);
            phase_ = Phase::Signature;
            fetch(channel(selected_tag_) + "aura-update.toml.sig", 64);
        } else if (phase_ == Phase::Signature) {
            auto candidate =
                verify_update_manifest(manifest_, bytes, public_key_, std::time(nullptr));
            if (candidate.tag != selected_tag_)
                throw std::runtime_error("Update verification failed. Nothing was installed.");
            if (candidate.sequence < highest_sequence_)
                throw std::runtime_error("An outdated update manifest was rejected.");
            highest_sequence_ = candidate.sequence;
            save_state();
            phase_ = Phase::Idle;
            if (candidate.sequence > browser_release_sequence) {
                release = std::move(candidate);
                status = "A new Aura release is available.";
            } else
                status = "Aura is up to date on this channel.";
        } else if (phase_ == Phase::Download) {
            phase_ = Phase::Verify;
            status = "Verifying the downloaded update...";
            verification_ =
                std::async(std::launch::async, [path = temporary_, artifact = *artifact_] {
                    verify_update_artifact(path, artifact);
                });
        }
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return true;
}
void UpdateManager::cancel() {
    if (request_)
        request_->Cancel();
    request_ = nullptr;
    transfer_.reset();
    if (verification_.valid())
        verification_.wait();
    if (!temporary_.empty()) {
        std::error_code error;
        std::filesystem::remove(temporary_, error);
        std::filesystem::remove(temporary_.parent_path(), error);
        temporary_.clear();
    }
    phase_ = Phase::Idle;
    status = "Update operation cancelled.";
}
void UpdateManager::shutdown() {
    cancel();
}
bool UpdateManager::launch_installer() const {
    if (ready_file.empty() || !artifact_ || !release ||
        release->expires <= static_cast<long long>(std::time(nullptr)))
        return false;
    // Recheck after shutdown: a downloaded executable must never bypass verification.
    try {
        verify_update_artifact(ready_file, *artifact_);
    } catch (const std::exception &) {
        return false;
    }
#ifdef _WIN32
    if (installation_kind() != "setup")
        return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", ready_file.c_str(), nullptr,
                                                   nullptr, SW_SHOWNORMAL)) > 32;
#else
    if (installation_kind() != "run")
        return false;
    auto path = utf8_path(ready_file);
    char shell[] = "bash";
    char *args[] = {shell, path.data(), nullptr};
    pid_t pid = 0;
    return posix_spawnp(&pid, shell, nullptr, nullptr, args, environ) == 0;
#endif
}
bool UpdateManager::open_download_folder() const {
    if (ready_file.empty())
        return false;
    const auto folder = ready_file.parent_path();
#ifdef _WIN32
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr,
                                                   nullptr, SW_SHOWNORMAL)) > 32;
#else
    auto path = utf8_path(folder);
    char command[] = "xdg-open";
    char *args[] = {command, path.data(), nullptr};
    pid_t pid = 0;
    return posix_spawnp(&pid, command, nullptr, nullptr, args, environ) == 0;
#endif
}
} // namespace aura
