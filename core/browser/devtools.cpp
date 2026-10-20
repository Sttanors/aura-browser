#include "core/browser/devtools.hpp"
#include "include/cef_parser.h"
namespace aura {
DevToolsSession::DevToolsSession(CefRefPtr<CefBrowser> browser) : browser_(browser) {}
void DevToolsSession::attach() {
    registration_ = browser_->GetHost()->AddDevToolsMessageObserver(this);
}
void DevToolsSession::close() {
    registration_ = nullptr;
    callbacks_.clear();
    event = {};
    browser_ = nullptr;
}
void DevToolsSession::call(const std::string &method, const std::string &params,
                           Callback callback) {
    if (!browser_)
        return;
    auto value = CefParseJSON(params, JSON_PARSER_RFC);
    auto dictionary = value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary()
                                                                    : CefDictionaryValue::Create();
    const int id = ++next_id_;
    if (callback)
        callbacks_[id] = std::move(callback);
    if (!browser_->GetHost()->ExecuteDevToolsMethod(id, method, dictionary)) {
        auto it = callbacks_.find(id);
        if (it != callbacks_.end()) {
            auto cb = std::move(it->second);
            callbacks_.erase(it);
            cb(false, "{\"message\":\"DevTools call rejected\"}");
        }
    }
}
void DevToolsSession::OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int id, bool success,
                                             const void *data, size_t size) {
    auto it = callbacks_.find(id);
    if (it == callbacks_.end())
        return;
    auto cb = std::move(it->second);
    callbacks_.erase(it);
    cb(success, data ? std::string(static_cast<const char *>(data), size) : "{}");
}
void DevToolsSession::OnDevToolsEvent(CefRefPtr<CefBrowser>, const CefString &method,
                                      const void *data, size_t size) {
    if (event)
        event(true, method.ToString() + "\n" +
                        (data ? std::string(static_cast<const char *>(data), size) : "{}"));
}
} // namespace aura
