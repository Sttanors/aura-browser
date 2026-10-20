#pragma once
#include "include/cef_browser.h"
#include "include/cef_devtools_message_observer.h"
#include <functional>
#include <map>
namespace aura {
class DevToolsSession final : public CefDevToolsMessageObserver {
  public:
    using Callback = std::function<void(bool, const std::string &)>;
    explicit DevToolsSession(CefRefPtr<CefBrowser> browser);
    Callback event;
    void OnDevToolsEvent(CefRefPtr<CefBrowser>, const CefString &, const void *, size_t) override;
    void attach();
    void close();
    void call(const std::string &method, const std::string &params = "{}", Callback callback = {});
    void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int, bool, const void *, size_t) override;

  private:
    CefRefPtr<CefBrowser> browser_;
    CefRefPtr<CefRegistration> registration_;
    std::map<int, Callback> callbacks_;
    int next_id_ = 700000;
    IMPLEMENT_REFCOUNTING(DevToolsSession);
};
} // namespace aura
