#pragma once
#include "core/browser/browser.hpp"
#include "include/cef_context_menu_handler.h"
#include "include/cef_frame.h"

namespace aura {
// Never use OnTextSelectionChanged for clipboard exports: it includes passwords.
inline void copy_native_selection(BrowserController &owner, CefRefPtr<CefFrame> frame,
                                  CefRefPtr<CefContextMenuParams> params, bool cut) {
    if (!owner.native_focus || owner.quitting)
        return;
    const auto flag = cut ? CM_EDITFLAG_CAN_CUT : CM_EDITFLAG_CAN_COPY;
    const auto text = params->GetSelectionText().ToString();
    if (!(params->GetEditStateFlags() & flag) || text.empty() || text.size() > 16 * 1024 * 1024) {
        ++owner.audit["native_clipboard_refused"];
        return;
    }
    owner.window->copy(text);
    if (cut && frame && frame->IsValid())
        frame->Delete();
}

inline void paste_native_selection(BrowserController &owner, CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame, std::function<bool()> current) {
    if (!owner.native_focus || !browser || !frame || !frame->IsValid())
        return;
    const auto url = frame->GetURL().ToString();
    const auto id = frame->GetIdentifier();
    std::weak_ptr<void> weak = owner.callback_lifetime;
    owner.window->paste(
        [weak, &owner, browser, frame, url, id, current = std::move(current)](std::string text) {
            if (weak.expired() || text.empty() || !owner.native_focus || !current() ||
                !frame->IsValid() || frame->GetURL().ToString() != url)
                return;
            auto focused = browser->GetFocusedFrame();
            if (focused && focused->GetIdentifier() == id)
                browser->GetHost()->ImeCommitText(text, CefRange(-1, -1), 0);
        });
}

inline void request_native_copy(CefRefPtr<CefBrowser> browser) {
    CefKeyEvent key;
    key.windows_key_code = 0x5d;
    key.native_key_code = 135;
    key.type = KEYEVENT_RAWKEYDOWN;
    browser->GetHost()->SendKeyEvent(key);
    key.type = KEYEVENT_KEYUP;
    browser->GetHost()->SendKeyEvent(key);
}
} // namespace aura
