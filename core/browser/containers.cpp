#include "common/config.hpp"
#include "core/browser/browser.hpp"
#include "include/cef_parser.h"
#include "include/cef_request_context_handler.h"
#include "ui/browser_ui.hpp"
#include <algorithm>
namespace aura {
namespace {
class ContainerContextHandler final : public CefRequestContextHandler {
  public:
    ContainerContextHandler(std::weak_ptr<BrowserState> state, int id)
        : state_(std::move(state)), id_(id) {}
    void OnRequestContextInitialized(CefRefPtr<CefRequestContext> context) override {
        if (auto state = state_.lock()) {
            if (!state->windows.empty())
                state->windows.front()->configure_permissions(context, "", "", id_, false);
            state->container_contexts_ready.insert(id_);
        }
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
namespace {
std::string rule_host(const std::string &url) {
    CefURLParts parts;
    if (!CefParseURL(url, parts))
        return {};
    auto scheme = CefString(&parts.scheme).ToString();
    if (scheme != "http" && scheme != "https")
        return {};
    auto host = CefString(&parts.host).ToString();
    if (host.find(':') != std::string::npos && !host.starts_with('['))
        host = "[" + host + "]";
    while (host.ends_with('.') && !host.empty())
        host.pop_back();
    return host;
}
} // namespace
bool BrowserController::save_container_rule(const std::string &input, int container,
                                            bool subdomains) {
    // The editor accepts a hostname, not an address that silently discards path/credentials.
    if (input.empty() || input.find_first_of("/@?#\\ \t\r\n") != std::string::npos ||
        (input.starts_with('[') ? !input.ends_with(']') : input.find(':') != std::string::npos))
        return false;
    auto host = rule_host("https://" + input);
    if (!profile.container(container) ||
        !set_container_rule(profile.container_rules, host, container, subdomains))
        return false;
    changed(true);
    return true;
}
bool BrowserController::route_container(int tab_id, const std::string &url,
                                        const std::string &method) {
    auto source = tab(tab_id);
    if (!source || source->private_tab || private_window || source->closing)
        return false;
    const auto rule = match_container_rule(profile.container_rules, rule_host(url));
    if (!rule || rule->container == source->container || !profile.container(rule->container))
        return false;
    if (method != "GET") {
        ui->toast(tr("This form targets another container. Open the site in that container before "
                     "submitting."));
        return true;
    }
    // A single pending request per source tab. The original document remains loaded.
    for (const auto &dialog : dialogs)
        if (dialog.container_route && dialog.tab == tab_id)
            return true;
    BrowserDialog dialog;
    dialog.tab = tab_id;
    dialog.owned = true;
    dialog.container_route = true;
    dialog.origin = tr("Open in another container?");
    dialog.accept_label = tr("Open tab");
    dialog.message = rule_host(url) + " → " + profile.container_label(rule->container) + "\n" +
                     tr("The original tab stays open. This address will open in a new tab with "
                        "separate cookies.");
    auto weak = std::weak_ptr<void>(callback_lifetime);
    const auto serial = source->navigation_serial;
    dialog.action = [this, weak, tab_id, url, rule = *rule, serial](bool accept,
                                                                    const std::string &) {
        if (weak.expired() || !accept)
            return;
        auto source = tab(tab_id);
        const auto current_rule = match_container_rule(profile.container_rules, rule_host(url));
        if (!source || source->closing || source->private_tab ||
            source->navigation_serial != serial || !profile.container(rule.container) ||
            !current_rule || current_rule->host != rule.host ||
            current_rule->container != rule.container ||
            current_rule->subdomains != rule.subdomains)
            return;
        new_tab(url, false, true, false, source->workspace, source->group, rule.container);
    };
    dialogs.push_back(std::move(dialog));
    changed();
    return true;
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
