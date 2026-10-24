#include "core/browser/client.hpp"
#include "include/cef_frame.h"
#include "include/cef_menu_model.h"
#include "include/cef_resource_handler.h"
#include "include/cef_resource_request_handler.h"
#include "ui/browser_ui.hpp"
#include <cstring>
#include <spdlog/spdlog.h>
namespace aura {
namespace {
class EmptyResource final : public CefResourceHandler {
  public:
    explicit EmptyResource(Protection::Action action) {
        if (action == Protection::Action::NoopScript) {
            mime_ = "application/javascript";
            bytes_ = ";";
        } else if (action == Protection::Action::NoopStyle) {
            mime_ = "text/css";
        } else {
            mime_ = "image/gif";
            const unsigned char data[] = {71, 73, 70,  56,  57,  97, 1,   0, 1, 0, 128, 0,  0, 0,
                                          0,  0,  255, 255, 255, 33, 249, 4, 1, 0, 0,   0,  0, 44,
                                          0,  0,  0,   0,   1,   0,  1,   0, 0, 2, 1,   68, 0, 59};
            bytes_.assign(reinterpret_cast<const char *>(data), sizeof data);
        }
    }
    bool Open(CefRefPtr<CefRequest>, bool &handle, CefRefPtr<CefCallback>) override {
        handle = true;
        return true;
    }
    void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t &length,
                            CefString &) override {
        response->SetStatus(200);
        response->SetMimeType(mime_);
        response->SetHeaderByName("Cache-Control", "no-store", true);
        length = bytes_.size();
    }
    bool Read(void *data, int count, int &read, CefRefPtr<CefResourceReadCallback>) override {
        read = std::min(std::max(0, count), int(bytes_.size() - offset_));
        if (!read)
            return false;
        std::memcpy(data, bytes_.data() + offset_, read);
        offset_ += read;
        return true;
    }
    void Cancel() override {
        offset_ = bytes_.size();
    }

  private:
    std::string mime_, bytes_;
    size_t offset_ = 0;
    IMPLEMENT_REFCOUNTING(EmptyResource);
};
class NetworkPolicy final : public CefResourceRequestHandler {
  public:
    explicit NetworkPolicy(std::shared_ptr<RequestPolicy> policy) : policy_(std::move(policy)) {}
    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                     CefRefPtr<CefRequest> request,
                                     CefRefPtr<CefCallback>) override {
        std::string source = request->GetReferrerURL().ToString();
        if (browser && browser->IsValid()) {
            auto top = browser->GetMainFrame();
            if (top && top->IsValid())
                source = top->GetURL();
        } else if (frame && frame->IsValid())
            source = frame->GetURL();
        replacement_ = Protection::Action::Allow;
        if (policy_->protection) {
            auto decision = policy_->protection->decide(request->GetURL().ToString(), source,
                                                        request->GetResourceType(),
                                                        request->GetMethod().ToString());
            if (decision.action == Protection::Action::Block)
                return RV_CANCEL;
            replacement_ = decision.action;
            if (!decision.rewritten.empty())
                request->SetURL(decision.rewritten);
        }
        if (policy_->do_not_track.load())
            request->SetHeaderByName("DNT", "1", true);
        if (!policy_->send_referrer.load())
            request->SetReferrer("", REFERRER_POLICY_NO_REFERRER);
        return RV_CONTINUE;
    }

    CefRefPtr<CefResourceHandler> GetResourceHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                                     CefRefPtr<CefRequest>) override {
        return replacement_ == Protection::Action::Allow
                   ? nullptr
                   : CefRefPtr<CefResourceHandler>(new EmptyResource(replacement_));
    }

  private:
    Protection::Action replacement_ = Protection::Action::Allow;
    std::shared_ptr<RequestPolicy> policy_;
    IMPLEMENT_REFCOUNTING(NetworkPolicy);
};
} // namespace
void BrowserClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect) {
    const auto &shot = owner_.screenshot;
    if (shot.native_running && shot.job_tab == id_ && shot.surface_width > 0 &&
        shot.surface_height > 0)
        rect = {0, 0, shot.surface_width, shot.surface_height};
    else
        DiagnosticClient::GetViewRect(browser, rect);
}
void BrowserClient::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                            const RectList &dirty, const void *data, int width, int height) {
    auto t = owner_.tab(id_);
    if (t && !t->sleeping &&
        !(owner_.screenshot.native_running && owner_.screenshot.job_tab == id_ &&
          owner_.screenshot.surface_width > 0))
        DiagnosticClient::OnPaint(browser, type, dirty, data, width, height);
}
#ifdef _WIN32
void BrowserClient::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                       const RectList &dirty, const CefAcceleratedPaintInfo &info) {
    auto t = owner_.tab(id_);
    if (t && !t->sleeping)
        DiagnosticClient::OnAcceleratedPaint(browser, type, dirty, info);
}
#endif
CefRefPtr<CefResourceRequestHandler>
BrowserClient::GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                         CefRefPtr<CefRequest>, bool, bool, const CefString &,
                                         bool &) {
    return new NetworkPolicy(policy_);
}
bool BrowserClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefRequest> request, bool, bool) {
    const auto url = request->GetURL().ToString();
    if (url.starts_with("http://") || url.starts_with("https://"))
        owner_.configure_permissions(browser->GetHost()->GetRequestContext(), url,
                                     frame->IsMain() ? url
                                                     : browser->GetMainFrame()->GetURL().ToString(),
                                     owner_.tab(id_) ? owner_.tab(id_)->container : 0,
                                     owner_.tab(id_) && owner_.tab(id_)->private_tab);
    // A document that navigates cannot retain a pending permission prompt.
    owner_.cancel_permissions(id_);
    if (frame->IsMain()) {
        if (auto t = owner_.tab(id_)) {
            ++t->navigation_serial;
            t->reader_active = t->reader_pending = false;
            t->reader_context = 0;
            t->reader_ui = "null";
            t->selected_text.clear();
            owner_.wake_tab(id_);
        }
        if (owner_.current() && owner_.current()->id == id_)
            owner_.close_extension_popup();
    }
    return false;
}
bool BrowserClient::OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser,
                                                   CefRefPtr<CefFrame>, const CefString &origin,
                                                   uint32_t permissions,
                                                   CefRefPtr<CefMediaAccessCallback> callback) {
    auto tab = owner_.tab(id_);
    const uint32_t devices =
        CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
    const bool audio = permissions & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE,
               video = permissions & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
    if (!tab || (permissions & ~devices) || (!audio && !video) ||
        (audio && owner_.permission_value(*tab, origin, "microphone") == "block") ||
        (video && owner_.permission_value(*tab, origin, "camera") == "block")) {
        callback->Cancel();
        ++owner_.audit["permissions_denied"];
        return true;
    }
    auto top = permission_origin(browser->GetMainFrame()->GetURL());
    if (top.empty())
        top = permission_origin(origin);
    if (!top.empty() && tab->permission_origins.size() < 128)
        tab->permission_origins.insert(top);
    owner_.configure_permissions(browser->GetHost()->GetRequestContext(), origin, top,
                                 tab->container, tab->private_tab);
    // Windowless CEF uses Alloy: default media handling denies every request.
    // Keep the native MediaStream registrar, but supply Aura's scoped decision.
    std::vector<SitePermission> choices, scope;
    std::string names;
    for (const char *id : {"camera", "microphone"}) {
        if ((std::string(id) == "camera" && !video) || (std::string(id) == "microphone" && !audio))
            continue;
        const auto site = owner_.permission_site(*tab, origin, id);
        scope.push_back({site, id, "", tab->container});
        if (owner_.permission_value(*tab, origin, id) == "ask") {
            choices.push_back(scope.back());
            if (!names.empty())
                names += ", ";
            names += owner_.tr(find_permission(id)->title);
        }
    }
    if (choices.empty()) {
        callback->Continue(permissions);
        return true;
    }
    BrowserDialog dialog;
    dialog.tab = id_;
    dialog.origin = origin;
    dialog.permission = true;
    dialog.permission_private = tab->private_tab;
    dialog.permission_rules = std::move(scope);
    dialog.message = owner_.tr("Allow access to: ") + names;
    const int id = id_, container = tab->container;
    const bool private_mode = tab->private_tab;
    const auto serial = tab->navigation_serial;
    const auto weak = std::weak_ptr<void>(owner_.callback_lifetime);
    auto *owner = &owner_;
    dialog.user_decision = [owner, id, container, private_mode, serial, weak,
                            choices](bool accept) {
        if (weak.expired())
            return;
        const auto tab = owner->tab(id);
        if (!tab || tab->closing || tab->navigation_serial != serial)
            return;
        for (const auto &rule : choices)
            owner->change_site_permission(rule.origin, container, private_mode, rule.permission,
                                          accept ? "allow" : "block");
    };
    dialog.action = [callback, permissions](bool accept, const std::string &) {
        if (accept)
            callback->Continue(permissions);
        else
            callback->Cancel();
    };
    owner_.dialogs.push_back(std::move(dialog));
    ++owner_.audit["permission_prompts"];
    owner_.changed();
    return true;
}
bool BrowserClient::OnShowPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t prompt,
                                           const CefString &origin, uint32_t permissions,
                                           CefRefPtr<CefPermissionPromptCallback> callback) {
    auto tab = owner_.tab(id_);
    const struct {
        uint32_t bit;
        const char *id;
    } policies[] = {
        {CEF_PERMISSION_TYPE_NOTIFICATIONS, "notifications"},
        {CEF_PERMISSION_TYPE_GEOLOCATION, "geolocation"},
        {CEF_PERMISSION_TYPE_CAMERA_STREAM, "camera"},
        {CEF_PERMISSION_TYPE_CAMERA_PAN_TILT_ZOOM, "camera_controls"},
        {CEF_PERMISSION_TYPE_MIC_STREAM, "microphone"},
        {CEF_PERMISSION_TYPE_CLIPBOARD, "clipboard"},
        {CEF_PERMISSION_TYPE_MIDI_SYSEX, "midi"},
        {CEF_PERMISSION_TYPE_SENSORS, "sensors"},
    };
    uint32_t known = 0;
    bool blocked = !tab;
    std::string names;
    std::vector<SitePermission> choices, scope;
    for (const auto &policy : policies) {
        known |= policy.bit;
        if (!(permissions & policy.bit) || !tab)
            continue;
        const auto site = owner_.permission_site(*tab, origin, policy.id);
        const auto value = owner_.permission_value(*tab, origin, policy.id);
        blocked |= value == "block";
        scope.push_back({site, policy.id, "", tab->container});
        if (!site.empty() && tab->permission_origins.size() < 128)
            tab->permission_origins.insert(site);
        if (value == "ask") {
            if (!names.empty())
                names += ", ";
            names += owner_.tr(find_permission(policy.id)->title);
            choices.push_back(scope.back());
        }
    }
    if (!permissions || (permissions & ~known) || blocked) {
        callback->Continue(CEF_PERMISSION_RESULT_DENY);
        ++owner_.audit["permissions_denied"];
        return true;
    }
    if (choices.empty()) {
        callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
        ++owner_.audit["permissions_allowed"];
        return true;
    }
    BrowserDialog dialog;
    dialog.tab = id_;
    dialog.origin = origin;
    dialog.permission = true;
    dialog.permission_id = prompt;
    dialog.permission_private = tab->private_tab;
    dialog.permission_rules = std::move(scope);
    dialog.message = owner_.tr("Allow access to: ") + names;
    const int id = id_, container = tab->container;
    const bool private_mode = tab->private_tab;
    const auto serial = tab->navigation_serial;
    const auto weak = std::weak_ptr<void>(owner_.callback_lifetime);
    auto *owner = &owner_;
    dialog.user_decision = [owner, id, container, private_mode, serial, weak,
                            choices](bool accept) {
        if (weak.expired())
            return;
        const auto tab = owner->tab(id);
        if (!tab || tab->closing || tab->navigation_serial != serial)
            return;
        for (const auto &rule : choices)
            owner->change_site_permission(rule.origin, container, private_mode, rule.permission,
                                          accept ? "allow" : "block", false);
    };
    dialog.action = [callback](bool allow, const std::string &) {
        callback->Continue(allow ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY);
    };
    owner_.dialogs.push_back(std::move(dialog));
    ++owner_.audit["permission_prompts"];
    owner_.changed();
    return true;
}
void BrowserClient::OnMediaAccessChange(CefRefPtr<CefBrowser>, bool video, bool audio) {
    if (auto tab = owner_.tab(id_)) {
        tab->media_video = video;
        tab->media_audio = audio;
        tab->media_capture = video || audio;
        ++owner_.audit["media_access_changes"];
        owner_.changed();
    }
}
void BrowserClient::OnDismissPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t prompt,
                                              cef_permission_request_result_t) {
    std::erase_if(owner_.dialogs, [&](const auto &d) {
        return d.permission && d.tab == id_ && d.permission_id == prompt;
    });
    owner_.changed();
}
void BrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> b) {
    DiagnosticClient::OnAfterCreated(b);
    owner_.browser_created(id_, b);
}
void BrowserClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> b, TerminationStatus status,
                                              int code, const CefString &text) {
    DiagnosticClient::OnRenderProcessTerminated(b, status, code, text);
    if (auto t = owner_.tab(id_); t && !t->closing) {
        ++owner_.audit["renderer_crashes"];
        owner_.changed();
    }
}
void BrowserClient::OnBeforeClose(CefRefPtr<CefBrowser> b) {
    owner_.cancel_permissions(id_);
    DiagnosticClient::OnBeforeClose(b);
    owner_.browser_closed(id_);
}
bool BrowserClient::OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int popup_id,
                                  const CefString &url, const CefString &,
                                  WindowOpenDisposition disposition, bool user_gesture,
                                  const CefPopupFeatures &, CefWindowInfo &info, CefRefPtr<CefClient> &client,
                                  CefBrowserSettings &, CefRefPtr<CefDictionaryValue> &, bool *) {
    if (owner_.profile.block_popups && !user_gesture) {
        ++owner_.audit["popups_blocked"];
        return true;
    }
    auto target = owner_.prepare_popup(id_, popup_id, url,
                                       disposition != CEF_WOD_NEW_BACKGROUND_TAB);
    if (!target)
        return true;
    info.SetAsWindowless(owner_.window->handle());
    client = new BrowserClient(owner_, *target);
    target->client = static_cast<BrowserClient *>(client.get());
    // Let Chromium create the actual browsing context and retain opener/name/
    // WindowProxy semantics. Do not replace this with a second LoadURL.
    return false;
}
void BrowserClient::OnBeforePopupAborted(CefRefPtr<CefBrowser>, int popup_id) {
    owner_.abort_popup(id_, popup_id);
}
bool BrowserClient::OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                     const CefString &url, WindowOpenDisposition disposition,
                                     bool user_gesture) {
    if (disposition == CEF_WOD_CURRENT_TAB)
        return false;
    if (owner_.profile.block_popups && !user_gesture) {
        ++owner_.audit["popups_blocked"];
        return true;
    }
    owner_.open_from_page(id_, url, disposition != CEF_WOD_NEW_BACKGROUND_TAB);
    return true;
}
void BrowserClient::OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, TransitionType) {
    if (!frame->IsMain())
        return;
    if (auto t = owner_.tab(id_)) {
        owner_.apply_site_zoom(*t, frame->GetURL().ToString());
        t->page.failed = false;
        t->page.error.clear();
        t->page.loaded = false;
        t->progress = 0.08;
        owner_.changed();
    }
}
void BrowserClient::OnLoadingStateChange(CefRefPtr<CefBrowser>, bool loading, bool back,
                                         bool forward) {
    if (auto t = owner_.tab(id_)) {
        t->loading = loading;
        t->back = back;
        t->forward = forward;
        if (!loading)
            t->progress = 1;
        owner_.changed();
    }
}
void BrowserClient::OnLoadingProgressChange(CefRefPtr<CefBrowser>, double progress) {
    if (auto t = owner_.tab(id_)) {
        t->progress = progress;
        owner_.changed();
    }
}
void BrowserClient::OnLoadEnd(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, int status) {
    DiagnosticClient::OnLoadEnd(b, f, status);
    if (f->IsMain()) {
        if (auto t = owner_.tab(id_); t && t->browser && !t->ntp)
            owner_.apply_site_zoom(*t, f->GetURL().ToString());
        owner_.page_changed(id_, true);
    }
}
void BrowserClient::OnLoadError(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, ErrorCode code,
                                const CefString &text, const CefString &url) {
    DiagnosticClient::OnLoadError(b, f, code, text, url);
    if (f->IsMain() && code != ERR_ABORTED) {
        if (auto tab = owner_.tab(id_); tab && !url.empty())
            tab->page.url = url.ToString();
        ++owner_.audit["load_errors"];
        owner_.page_changed(id_);
    }
}
void BrowserClient::OnTitleChange(CefRefPtr<CefBrowser> b, const CefString &title) {
    if (auto t = owner_.tab(id_); t && t->ntp)
        return;
    DiagnosticClient::OnTitleChange(b, title);
    owner_.page_changed(id_);
}
void BrowserClient::OnAddressChange(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f,
                                    const CefString &url) {
    if (f->IsMain()) {
        if (auto t = owner_.tab(id_)) {
            t->ntp = url == "about:blank#aura-newtab";
            b->GetHost()->WasHidden(!owner_.tab_visible(t->id) || t->ntp);
            b->GetHost()->SetAudioMuted(t->muted);
            if (t->ntp) {
                t->page.url = "aura://newtab";
                t->page.title = owner_.tr(t->private_tab ? "Private tab" : "New tab");
                owner_.page_changed(id_);
                return;
            }
        }
    }
    DiagnosticClient::OnAddressChange(b, f, url);
    owner_.page_changed(id_);
}
void BrowserClient::OnFaviconURLChange(CefRefPtr<CefBrowser>, const std::vector<CefString> &urls) {
    if (!urls.empty())
        owner_.update_favicon(id_, urls.front());
}
void BrowserClient::OnStatusMessage(CefRefPtr<CefBrowser>, const CefString &value) {
    if (owner_.current() == owner_.tab(id_) && owner_.ui->status != value.ToString()) {
        owner_.ui->status = value;
        owner_.changed();
    }
}
bool BrowserClient::OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t type,
                                   const CefCursorInfo &) {
    if (owner_.current() == owner_.tab(id_))
        owner_.window->cursor(type == CT_IBEAM ? 1 : type == CT_HAND ? 2 : 0);
    return true;
}
bool BrowserClient::CanDownload(CefRefPtr<CefBrowser>, const CefString &, const CefString &) {
    spdlog::info("Download request allowed");
    return true;
}
bool BrowserClient::OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item,
                                     const CefString &suggested,
                                     CefRefPtr<CefBeforeDownloadCallback> callback) {
    owner_.start_download(item, callback, suggested, id_);
    return true;
}
void BrowserClient::OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item,
                                      CefRefPtr<CefDownloadItemCallback> callback) {
    owner_.download_updated(item, callback);
}
void BrowserClient::OnFindResult(CefRefPtr<CefBrowser>, int, int count, const CefRect &, int active,
                                 bool) {
    if (owner_.current() == owner_.tab(id_)) {
        owner_.find_matches = count;
        owner_.find_active = active;
        owner_.audit["find_matches"] = std::max(owner_.audit["find_matches"], count);
        owner_.changed();
    }
}
void BrowserClient::OnBeforeContextMenu(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame>,
                                        CefRefPtr<CefContextMenuParams> p,
                                        CefRefPtr<CefMenuModel> model) {
    model->Clear();
    if (!p->GetLinkUrl().empty()) {
        model->AddItem(MENU_ID_USER_FIRST, owner_.tr("Open link in new tab"));
        model->AddItem(MENU_ID_USER_FIRST + 1, owner_.tr("Copy link address"));
        model->AddSeparator();
    }
    model->AddItem(MENU_ID_BACK, owner_.tr("Back"));
    model->SetEnabled(MENU_ID_BACK, b->CanGoBack());
    model->AddItem(MENU_ID_FORWARD, owner_.tr("Forward"));
    model->SetEnabled(MENU_ID_FORWARD, b->CanGoForward());
    model->AddItem(MENU_ID_RELOAD, owner_.tr("Reload"));
    model->AddSeparator();
    if (p->IsEditable()) {
        model->AddItem(MENU_ID_UNDO, owner_.tr("Undo"));
        model->AddItem(MENU_ID_CUT, owner_.tr("Cut"));
        model->AddItem(MENU_ID_PASTE, owner_.tr("Paste"));
    }
    model->AddItem(MENU_ID_COPY, owner_.tr("Copy"));
    model->AddItem(MENU_ID_SELECT_ALL, owner_.tr("Select all"));
    model->AddItem(MENU_ID_USER_FIRST + 3, owner_.tr("Translate selection"));
    model->SetEnabled(MENU_ID_USER_FIRST + 3, !p->GetSelectionText().empty());
    model->AddItem(MENU_ID_USER_FIRST + 4, owner_.tr("Translate page"));
    model->AddSeparator();
    model->AddItem(MENU_ID_USER_FIRST + 2, owner_.tr("Developer tools"));
}
bool BrowserClient::RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                   CefRefPtr<CefContextMenuParams> p, CefRefPtr<CefMenuModel> model,
                                   CefRefPtr<CefRunContextMenuCallback> callback) {
    owner_.close_menu();
    owner_.menu_callback = callback;
    owner_.menu.clear();
    for (std::size_t i = 0; i < model->GetCount(); ++i)
        owner_.menu.push_back(
            {model->GetCommandIdAt(i),
             model->GetTypeAt(i) == MENUITEMTYPE_SEPARATOR ? "" : model->GetLabelAt(i).ToString(),
             model->IsEnabledAt(i)});
    auto target = owner_.tab(id_);
    auto bounds = target ? owner_.ui->page_bounds(*target) : owner_.ui->web_bounds();
    owner_.menu_x =
        std::clamp(bounds.x + int(p->GetXCoord() / owner_.ui_scale), 8, owner_.width - 250);
    owner_.menu_y =
        std::clamp(bounds.y + int(p->GetYCoord() / owner_.ui_scale), 32,
                   std::max(32, owner_.height - static_cast<int>(owner_.menu.size()) * 32 - 12));
    owner_.changed();
    return true;
}
bool BrowserClient::OnContextMenuCommand(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                         CefRefPtr<CefContextMenuParams> p, int command,
                                         EventFlags) {
    if (command == MENU_ID_COPY) {
        if (auto tab = owner_.tab(id_); tab && tab->reader_active) {
            const auto text = p->GetSelectionText().ToString();
            if (!text.empty()) {
                owner_.window->copy(text);
                ++owner_.audit["reader_context_copies"];
            }
            return true;
        }
    }
    if (command == MENU_ID_USER_FIRST) {
        owner_.open_from_page(id_, p->GetLinkUrl(), true);
        return true;
    }
    if (command == MENU_ID_USER_FIRST + 1) {
        owner_.window->copy(p->GetLinkUrl());
        return true;
    }
    if (command == MENU_ID_USER_FIRST + 2) {
        owner_.devtools();
        return true;
    }
    if (command == MENU_ID_USER_FIRST + 3 || command == MENU_ID_USER_FIRST + 4) {
        if (auto tab = owner_.tab(id_)) {
            tab->selected_text = p->GetSelectionText().ToString();
            owner_.ui->open_translation(command == MENU_ID_USER_FIRST + 3, id_);
        }
        return true;
    }
    return false;
}
bool BrowserClient::OnJSDialog(CefRefPtr<CefBrowser>, const CefString &origin, JSDialogType kind,
                               const CefString &message, const CefString &text,
                               CefRefPtr<CefJSDialogCallback> callback, bool &suppress) {
    if (!owner_.profile.allow_dialogs) {
        suppress = true;
        ++owner_.audit["dialogs_blocked"];
        return false;
    }
    ++owner_.audit["javascript_dialogs"];
    BrowserDialog dialog;
    dialog.tab = id_;
    dialog.origin = url_host(origin);
    dialog.message = message;
    dialog.prompt = kind == JSDIALOGTYPE_PROMPT;
    dialog.input.set(text);
    dialog.callback = callback;
    owner_.dialogs.push_back(std::move(dialog));
    owner_.changed();
    return true;
}
bool BrowserClient::OnBeforeUnloadDialog(CefRefPtr<CefBrowser>, const CefString &message, bool,
                                         CefRefPtr<CefJSDialogCallback> callback) {
    BrowserDialog dialog;
    dialog.tab = id_;
    dialog.origin = owner_.tr("Leave this page?");
    dialog.message = message;
    dialog.before_unload = true;
    dialog.callback = callback;
    owner_.dialogs.push_back(std::move(dialog));
    owner_.changed();
    return true;
}
void BrowserClient::OnResetDialogState(CefRefPtr<CefBrowser>) {
    for (auto it = owner_.dialogs.begin(); it != owner_.dialogs.end();) {
        if (it->tab == id_ && !it->permission)
            it = owner_.dialogs.erase(it);
        else
            ++it;
    }
    owner_.changed();
}
} // namespace aura

namespace aura {
void BrowserClient::OnTextSelectionChanged(CefRefPtr<CefBrowser>, const CefString &text,
                                           const CefRange &) {
    if (auto t = owner_.tab(id_)) {
        t->selected_text = text.ToString();
        if (t->selected_text.size() > 16 * 1024 * 1024)
            t->selected_text.clear();
        owner_.changed();
    }
}
} // namespace aura
