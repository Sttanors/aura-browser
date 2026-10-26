#include "core/browser/extension_access.hpp"
#include "common/config.hpp"
#include "core/browser/devtools.hpp"
#include "core/browser/protection.hpp"
#include "core/cef/diagnostic_client.hpp"
#include "include/cef_parser.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"
#include <algorithm>
#include <chrono>
namespace aura {
struct ExtensionSiteAccess::Impl : std::enable_shared_from_this<Impl> {
    DiagnosticState page;
    ExtensionAccessSnapshot state;
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<CefClient> client;
    CefRefPtr<DevToolsSession> tools;
    CefRefPtr<CefWindow> host_window;
    bool opening = false;
    std::string expression;
    int created = 0, retired = 0;
    std::chrono::steady_clock::time_point deadline;
    void fail() {
        ++state.revision;
        state.busy = false;
        state.available = false;
        state.error = "Could not read or change site access. Try again.";
    }
    void execute() {
        if (!browser || !page.loaded || page.closing || expression.empty())
            return;
        auto script = std::move(expression);
        expression.clear();
        auto weak = weak_from_this();
        tools->call(
            "Runtime.evaluate",
            "{\"awaitPromise\":true,\"returnByValue\":true,\"expression\":" + json_quote(script) +
                "}",
            [weak](bool ok, const std::string &json) {
                auto self = weak.lock();
                if (!self || self->page.closing)
                    return;
                auto value = CefParseJSON(json, JSON_PARSER_RFC);
                auto root = value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary()
                                                                          : nullptr;
                auto result = root ? root->GetDictionary("result") : nullptr;
                auto data = result ? result->GetDictionary("value") : nullptr;
                if (!ok || !data || root->HasKey("exceptionDetails") ||
                    data->GetString("id") != self->state.id) {
                    self->fail();
                    return;
                }
                self->state.busy = false;
                ++self->state.revision;
                self->state.available = data->GetBool("available");
                self->state.mode = data->GetString("mode");
                self->state.hosts.clear();
                if (auto hosts = data->GetList("hosts"))
                    for (size_t i = 0; i < hosts->GetSize(); ++i)
                        self->state.hosts.push_back(hosts->GetString(i));
                self->state.error = data->GetBool("error")
                                        ? "Could not read or change site access. Try again."
                                    : self->state.available
                                        ? ""
                                        : "This extension has no adjustable website permissions.";
            });
    }
    class Client final : public DiagnosticClient {
      public:
        explicit Client(std::shared_ptr<Impl> owner)
            : DiagnosticClient(owner->page), owner_(std::move(owner)) {}
        void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
            DiagnosticClient::OnAfterCreated(browser);
            owner_->browser = browser;
            ++owner_->created;
            owner_->opening = false;
            owner_->tools = new DevToolsSession(browser);
            owner_->tools->attach();
            if (owner_->page.closing)
                browser->GetHost()->CloseBrowser(true);
        }
        void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
            DiagnosticClient::OnBeforeClose(browser);
            if (owner_->tools)
                owner_->tools->close();
            owner_->tools = nullptr;
            owner_->browser = nullptr;
            owner_->client = nullptr;
            owner_->state.busy = false;
            ++owner_->retired;
            if (owner_->host_window)
                owner_->host_window->Close();
        }
        bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefRequest> request, bool, bool) override {
            return !frame->IsMain() || (request->GetURL() != "about:blank" &&
                                        request->GetURL() != "chrome://extensions/");
        }
        void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                       int status) override {
            DiagnosticClient::OnLoadEnd(browser, frame, status);
            if (frame->IsMain() && frame->GetURL() == "chrome://extensions/")
                owner_->execute();
        }
        void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status,
                                       int code, const CefString &error) override {
            DiagnosticClient::OnRenderProcessTerminated(browser, status, code, error);
            owner_->fail();
            owner_->page.closing = true;
            browser->GetHost()->CloseBrowser(true);
        }
        void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList &, const void *, int,
                     int) override {}

      private:
        std::shared_ptr<Impl> owner_;
        IMPLEMENT_REFCOUNTING(Client);
    };
    class ViewDelegate final : public CefBrowserViewDelegate {
      public:
        cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_CHROME; }

      private:
        IMPLEMENT_REFCOUNTING(ViewDelegate);
    };
    class WindowDelegate final : public CefWindowDelegate {
      public:
        WindowDelegate(std::shared_ptr<Impl> owner, CefRefPtr<CefBrowserView> view)
            : owner_(std::move(owner)), view_(view) {}
        void OnWindowCreated(CefRefPtr<CefWindow> window) override {
            owner_->host_window = window;
            window->SetBounds({0, 0, 640, 480});
            window->AddChildView(view_);
            view_ = nullptr;
        }
        void OnWindowDestroyed(CefRefPtr<CefWindow>) override {
            owner_->host_window = nullptr;
            owner_->opening = false;
            if (!owner_->browser) {
                owner_->client = nullptr;
                owner_->page.closed = true;
            }
        }
        cef_show_state_t GetInitialShowState(CefRefPtr<CefWindow>) override {
            return CEF_SHOW_STATE_HIDDEN;
        }
        cef_runtime_style_t GetWindowRuntimeStyle() override { return CEF_RUNTIME_STYLE_CHROME; }
        bool CanClose(CefRefPtr<CefWindow>) override {
            return !owner_->browser || owner_->browser->GetHost()->TryCloseBrowser();
        }

      private:
        std::shared_ptr<Impl> owner_;
        CefRefPtr<CefBrowserView> view_;
        IMPLEMENT_REFCOUNTING(WindowDelegate);
    };
};
ExtensionSiteAccess::ExtensionSiteAccess() : impl_(std::make_shared<Impl>()) {}
ExtensionSiteAccess::~ExtensionSiteAccess() { close(); }
void ExtensionSiteAccess::request(const std::string &id, const std::string &operation,
                                  const std::string &value) {
    auto &r = *impl_;
    if (r.page.closed)
        r.page = DiagnosticState{};
    if (r.state.busy || r.page.closing || id.size() != 32 ||
        !std::all_of(id.begin(), id.end(), [](char c) { return c >= 'a' && c <= 'p'; }))
        return;
    if (operation != "read" && operation != "mode" && operation != "add" && operation != "remove")
        return;
    if (operation == "mode" && value != "ON_CLICK" && value != "ON_SPECIFIC_SITES" &&
        value != "ON_ALL_SITES")
        return;
    r.state.id = id;
    r.state.busy = true;
    r.state.error.clear();
    r.state.available = false;
    ++r.state.revision;
    r.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::string mutation;
    if (operation == "mode")
        mutation = "await api.updateExtensionConfiguration({extensionId:id,hostAccess:" +
                   json_quote(value) + "});";
    if (operation == "add")
        mutation = "const host=" + json_quote(value) +
                   ";const matches=await "
                   "api.getMatchingExtensionsForSite(host);if(!matches.some(e=>e.id===id))throw "
                   "Error();await "
                   "api.updateExtensionConfiguration({extensionId:id,hostAccess:'ON_SPECIFIC_SITES'"
                   "});await api.addHostPermission(id,host);";
    if (operation == "remove")
        mutation = "await api.removeHostPermission(id," + json_quote(value) + ");";
    r.expression =
        "(async()=>{try{if(location.href!=='chrome://extensions/')throw Error();const "
        "api=chrome.developerPrivate,id=" +
        json_quote(id) + ";let error=false;try{" + mutation + "}catch(e){error=true;}" +
        "const info=await api.getExtensionInfo(id);const "
        "p=info.permissions.runtimeHostPermissions;return "
        "{id,error,available:!!p&&info.userMayModify,mode:p?p.hostAccess:'',hosts:p?p.hosts.filter("
        "h=>h.granted).map(h=>h.host).sort():[]};}catch(e){return {error:true}}})()";
    if (!r.browser && !r.opening) {
        r.page.view_width = 160;
        r.page.view_height = 100;
        r.client = new Impl::Client(impl_);
        CefBrowserSettings settings;
        settings.windowless_frame_rate = 1;
        settings.javascript = STATE_ENABLED;
        r.page.loaded = false;
        r.opening = true;
        auto view = CefBrowserView::CreateBrowserView(
            r.client, "chrome://extensions/", settings, nullptr,
            CefRequestContext::GetGlobalContext(), new Impl::ViewDelegate());
        if (!view) {
            r.opening = false;
            r.client = nullptr;
            r.fail();
            return;
        }
        if (!CefWindow::CreateTopLevelWindow(new Impl::WindowDelegate(impl_, view))) {
            r.opening = false;
            r.client = nullptr;
            r.fail();
        }
    } else
        r.execute();
}
const ExtensionAccessSnapshot &ExtensionSiteAccess::snapshot() const { return impl_->state; }
void ExtensionSiteAccess::tick() {
    if (impl_->state.busy && std::chrono::steady_clock::now() > impl_->deadline) {
        impl_->fail();
        close();
    }
}
void ExtensionSiteAccess::close() {
    auto &r = *impl_;
    if (!r.page.closing) {
        r.page.closing = true;
        r.expression.clear();
        if (r.browser)
            r.browser->GetHost()->CloseBrowser(true);
        else if (r.host_window)
            r.host_window->Close();
    }
}
bool ExtensionSiteAccess::closed() const {
    return !impl_->browser && !impl_->host_window && !impl_->opening;
}
int ExtensionSiteAccess::created_count() const { return impl_->created; }
int ExtensionSiteAccess::closed_count() const { return impl_->retired; }
std::string ExtensionSiteAccess::report() const {
    const auto &s = impl_->state;
    std::string hosts = "[";
    for (const auto &h : s.hosts) {
        if (hosts.size() > 1)
            hosts += ',';
        hosts += json_quote(h);
    }
    hosts += ']';
    return "{\"revision\":" + std::to_string(s.revision) + ",\"id\":" + json_quote(s.id) +
           ",\"mode\":" + json_quote(s.mode) + ",\"error\":" + json_quote(s.error) +
           ",\"available\":" + (s.available ? "true" : "false") +
           ",\"busy\":" + (s.busy ? "true" : "false") + ",\"hosts\":" + hosts +
           ",\"created\":" + std::to_string(impl_->created) +
           ",\"closed\":" + std::to_string(impl_->retired) + ",\"service_visible\":" +
           (impl_->host_window && impl_->host_window->IsVisible() ? "true" : "false") +
           ",\"page_url\":" + json_quote(impl_->page.url) +
           ",\"page_loaded\":" + (impl_->page.loaded ? "true" : "false") + "}";
}
} // namespace aura
