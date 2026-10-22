#include "common/config.hpp"
#include "core/browser/browser.hpp"
#include "include/cef_request_context_handler.h"
#include "ui/browser_ui.hpp"
#include <algorithm>
namespace aura {
namespace {
class ContainerContextHandler final : public CefRequestContextHandler {
  public:
    ContainerContextHandler(std::weak_ptr<BrowserState> state, int id)
        : state_(std::move(state)), id_(id) {}
    void OnRequestContextInitialized(CefRefPtr<CefRequestContext>) override {
        if (auto state = state_.lock())
            state->container_contexts_ready.insert(id_);
    }

  private:
    std::weak_ptr<BrowserState> state_;
    int id_;
    IMPLEMENT_REFCOUNTING(ContainerContextHandler);
};
} // namespace
CefRefPtr<CefRequestContext> BrowserController::request_context(int id, bool private_tab) {
    if (private_tab) {
        if (!private_context_) {
            CefRequestContextSettings settings;
            CefString(&settings.accept_language_list) = profile.site_language;
            private_context_ = CefRequestContext::CreateContext(settings, nullptr);
        }
        return private_context_;
    }
    if (!id)
        return CefRequestContext::GetGlobalContext();
    if (!profile.container(id))
        return nullptr;
    auto &context = state_->container_contexts[id];
    if (!context) {
        try {
            const auto path = profile.container_data_directory(id);
            std::filesystem::create_directories(path);
            CefRequestContextSettings settings;
            CefString(&settings.cache_path) = utf8_path(path);
            CefString(&settings.accept_language_list) = profile.site_language;
            settings.persist_session_cookies = true;
            context =
                CefRequestContext::CreateContext(settings, new ContainerContextHandler(state_, id));
        } catch (const std::exception &) {
            ui->toast(tr("Could not open this container's storage."));
            return nullptr;
        }
    }
    return context;
}
bool BrowserController::container_in_use(int id) const {
    for (const auto *window : state_->windows)
        for (const auto &t : window->tabs)
            if (t->container == id && (!t->page.closed || t->popup_pending))
                return true;
    return false;
}
void BrowserController::remove_container(int id) {
    if (!id || container_in_use(id)) {
        ui->toast(tr("Close this container's tabs in all windows first."));
        return;
    }
    if (!profile.remove_container(id))
        return;
    for (auto *other : state_->windows)
        std::erase_if(other->recently_closed_, [&](const auto &t) { return t.container == id; });
    for (auto &saved : state_->previous_windows)
        remove_container_tabs(saved, id);
    std::erase_if(state_->previous_windows, [](const auto &w) { return w.tabs.empty(); });
    for (auto &request : state_->pending_windows)
        if (request.saved)
            remove_container_tabs(*request.saved, id);
    ui->toast(tr("Container removed. Site data will be cleared on the next launch."));
    changed(true);
}
} // namespace aura
