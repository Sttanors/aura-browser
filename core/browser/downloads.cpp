#include "common/config.hpp"
#include "common/platform.hpp"
#include "core/browser/browser.hpp"
#include "core/browser/download_hash.hpp"
#include "include/cef_task.h"
#include "ui/browser_ui.hpp"
#include <algorithm>
namespace aura {
namespace {
class DownloadTask final : public CefTask {
  public:
    explicit DownloadTask(std::function<void()> action) : action_(std::move(action)) {}
    void Execute() override { action_(); }

  private:
    std::function<void()> action_;
    IMPLEMENT_REFCOUNTING(DownloadTask);
};
} // namespace
bool BrowserController::download_visible(const Download &d) const {
    return private_window ? d.private_mode && d.source_window == window_uid
                          : !d.private_mode || d.source_window == window_uid;
}
std::vector<std::size_t> BrowserController::visible_downloads() const {
    std::vector<std::size_t> result;
    for (size_t i = 0; i < downloads.size(); ++i)
        if (download_visible(downloads[i]))
            result.push_back(i);
    return result;
}
void BrowserController::retry_download(std::size_t index) {
    if (index >= downloads.size() || !download_visible(downloads[index]))
        return;
    const auto d = downloads[index];
    if ((!d.cancelled && !d.interrupted) || d.url.empty() ||
        (!d.private_mode && !profile.container(d.container)))
        return;
    for (auto *window : state_->windows)
        if (window->window_uid == d.source_window)
            if (auto t = window->tab(d.source_tab); t && !t->closing && t->browser &&
                                                    t->private_tab == d.private_mode &&
                                                    t->container == d.container) {
                t->browser->GetHost()->StartDownload(d.url);
                return;
            }
    // A replacement tab supplies the original request context if its source was closed.
    const int next = next_id_;
    new_tab("about:blank", false, false, d.private_mode, workspace, 0, d.container);
    if (auto t = tab(next)) {
        t->pending_url.clear();
        t->pending_download_url = d.url;
        begin_pending_download(t->id);
    }
}
void BrowserController::begin_pending_download(int id) {
    auto t = tab(id);
    if (!t || !t->browser || !t->site_tools_ready || !t->page.loaded ||
        t->pending_download_url.empty() || t->closing)
        return;
    auto url = std::move(t->pending_download_url);
    t->pending_download_url.clear();
    auto weak = std::weak_ptr<void>(callback_lifetime);
    defer([this, weak, id, url] {
        if (weak.expired())
            return;
        if (auto t = tab(id); t && t->browser && !t->closing)
            t->browser->GetHost()->StartDownload(url);
    });
}
void BrowserController::verify_download(std::size_t index) {
    if (index >= downloads.size() || !download_visible(downloads[index]) ||
        !downloads[index].complete || downloads[index].checksum_status == "hashing")
        return;
    const auto id = downloads[index].id;
    const auto owner = downloads[index].source_window;
    BrowserDialog dialog;
    dialog.owned = true;
    dialog.prompt = true;
    dialog.origin = tr("Verify SHA-256");
    dialog.message = tr("Paste the expected 64-digit SHA-256. Leave empty to calculate only.");
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    dialog.action = [this, weak, id, owner](bool accept, const std::string &input) {
        if (weak.expired() || !accept)
            return;
        auto it = std::find_if(downloads.begin(), downloads.end(), [&](const auto &d) {
            return d.id == id && d.source_window == owner;
        });
        if (it == downloads.end() || !it->complete || it->checksum_status == "hashing")
            return;
        const auto expected = normalize_sha256(input);
        if (!input.empty() && expected.empty()) {
            verify_download(size_t(it - downloads.begin()));
            if (!dialogs.empty())
                dialogs.back().input.set(input, true);
            ui->toast(tr("Use exactly 64 hexadecimal characters."));
            return;
        }
        it->checksum_status = "hashing";
        it->sha256.clear();
        const auto generation = ++it->checksum_generation;
        const auto path = path_from_utf8(it->path);
        auto state = std::weak_ptr<BrowserState>(state_);
        changed();
        const bool posted = CefPostTask(
            TID_FILE_USER_VISIBLE,
            new DownloadTask([state, weak, this, id, owner, generation, path, expected] {
                std::string digest, error;
                try {
                    digest = download_sha256(path);
                } catch (const std::exception &) {
                    error = "Could not hash this file. Check that it exists and is unchanged.";
                }
                CefPostTask(
                    TID_UI, new DownloadTask([state, weak, this, id, owner, generation, digest,
                                              error, expected] {
                        auto shared = state.lock();
                        if (!shared)
                            return;
                        auto it = std::find_if(
                            shared->downloads.begin(), shared->downloads.end(),
                            [&](const auto &d) { return d.id == id && d.source_window == owner; });
                        if (it == shared->downloads.end() || it->checksum_generation != generation)
                            return;
                        it->sha256 = digest;
                        it->checksum_status = !error.empty()       ? "error"
                                              : expected.empty()   ? "computed"
                                              : digest == expected ? "match"
                                                                   : "mismatch";
                        for (auto *window : shared->windows)
                            window->changed();
                        if (weak.expired())
                            return;
                        if (!error.empty()) {
                            ui->toast(tr(error));
                            return;
                        }
                        BrowserDialog result;
                        result.owned = true;
                        result.prompt = true;
                        result.origin = tr(expected.empty()     ? "SHA-256 calculated"
                                           : digest == expected ? "SHA-256 matches"
                                                                : "SHA-256 does not match");
                        result.message = tr("Calculated SHA-256. Select the value to copy.");
                        result.input.set(digest, true);
                        dialogs.push_back(std::move(result));
                        changed();
                    }));
            }));
        if (!posted) {
            it->checksum_status = "error";
            changed();
        }
    };
    dialogs.push_back(std::move(dialog));
    changed();
}
} // namespace aura
