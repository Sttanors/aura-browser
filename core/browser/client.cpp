#include "core/browser/client.hpp"
#include "core/browser/native_clipboard.hpp"
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
    const auto &shot = controller().screenshot;
    if (shot.native_running && shot.job_tab == id_ && shot.surface_width > 0 &&
        shot.surface_height > 0)
        rect = {0, 0, shot.surface_width, shot.surface_height};
    else
        DiagnosticClient::GetViewRect(browser, rect);
}
void BrowserClient::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                            const RectList &dirty, const void *data, int width, int height) {
    auto t = controller().tab(id_);
    if (t && !t->sleeping &&
        !(controller().screenshot.native_running && controller().screenshot.job_tab == id_ &&
          controller().screenshot.surface_width > 0))
        DiagnosticClient::OnPaint(browser, type, dirty, data, width, height);
}
#ifdef _WIN32
void BrowserClient::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                       const RectList &dirty, const CefAcceleratedPaintInfo &info) {
    auto t = controller().tab(id_);
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
    if (frame->IsMain() && controller().route_container(id_, url, request->GetMethod().ToString()))
        return true;
    if (url.starts_with("http://") || url.starts_with("https://"))
        controller().configure_permissions(
            browser->GetHost()->GetRequestContext(), url,
            frame->IsMain() ? url : browser->GetMainFrame()->GetURL().ToString(),
            controller().tab(id_) ? controller().tab(id_)->container : 0,
            controller().tab(id_) && controller().tab(id_)->private_tab);
    // A document that navigates cannot retain a pending permission prompt.
    controller().cancel_permissions(id_);
    if (frame->IsMain()) {
        if (auto t = controller().tab(id_)) {
            ++t->navigation_serial;
            t->reader_active = t->reader_pending = t->reading_pending = false;
            t->reader_context = 0;
            t->reader_ui = "null";
            t->selected_text.clear();
            controller().wake_tab(id_);
        }
        if (controller().current() && controller().current()->id == id_)
            controller().close_extension_popup();
    }
    return false;
}
bool BrowserClient::OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser,
                                                   CefRefPtr<CefFrame>, const CefString &origin,
                                                   uint32_t permissions,
                                                   CefRefPtr<CefMediaAccessCallback> callback) {
    auto tab = controller().tab(id_);
    const uint32_t devices =
        CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
    const bool audio = permissions & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE,
               video = permissions & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
    if (!tab || (permissions & ~devices) || (!audio && !video) ||
        (audio && controller().permission_value(*tab, origin, "microphone") == "block") ||
        (video && controller().permission_value(*tab, origin, "camera") == "block")) {
        callback->Cancel();
        ++controller().audit["permissions_denied"];
        return true;
    }
    auto top = permission_origin(browser->GetMainFrame()->GetURL());
    if (top.empty())
        top = permission_origin(origin);
    if (!top.empty() && tab->permission_origins.size() < 128)
        tab->permission_origins.insert(top);
    controller().configure_permissions(browser->GetHost()->GetRequestContext(), origin, top,
                                       tab->container, tab->private_tab);
    // Windowless CEF uses Alloy: default media handling denies every request.
    // Keep the native MediaStream registrar, but supply Aura's scoped decision.
    std::vector<SitePermission> choices, scope;
    std::string names;
    for (const char *id : {"camera", "microphone"}) {
        if ((std::string(id) == "camera" && !video) || (std::string(id) == "microphone" && !audio))
            continue;
        const auto site = controller().permission_site(*tab, origin, id);
        scope.push_back({site, id, "", tab->container});
        if (controller().permission_value(*tab, origin, id) == "ask") {
            choices.push_back(scope.back());
            if (!names.empty())
                names += ", ";
            names += controller().tr(find_permission(id)->title);
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
    dialog.message = controller().tr("Allow access to: ") + names;
    const int id = id_, container = tab->container;
    const bool private_mode = tab->private_tab;
    const auto serial = tab->navigation_serial;
    const auto weak = std::weak_ptr<void>(controller().callback_lifetime);
    auto *owner = &this->controller();
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
    controller().dialogs.push_back(std::move(dialog));
    ++controller().audit["permission_prompts"];
    controller().changed();
    return true;
}
bool BrowserClient::OnShowPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t prompt,
                                           const CefString &origin, uint32_t permissions,
                                           CefRefPtr<CefPermissionPromptCallback> callback) {
    auto tab = controller().tab(id_);
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
        const auto site = controller().permission_site(*tab, origin, policy.id);
        const auto value = controller().permission_value(*tab, origin, policy.id);
        blocked |= value == "block";
        scope.push_back({site, policy.id, "", tab->container});
        if (!site.empty() && tab->permission_origins.size() < 128)
            tab->permission_origins.insert(site);
        if (value == "ask") {
            if (!names.empty())
                names += ", ";
            names += controller().tr(find_permission(policy.id)->title);
            choices.push_back(scope.back());
        }
    }
    if (!permissions || (permissions & ~known) || blocked) {
        callback->Continue(CEF_PERMISSION_RESULT_DENY);
        ++controller().audit["permissions_denied"];
        return true;
    }
    if (choices.empty()) {
        callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
        ++controller().audit["permissions_allowed"];
        return true;
    }
    BrowserDialog dialog;
    dialog.tab = id_;
    dialog.origin = origin;
    dialog.permission = true;
    dialog.permission_id = prompt;
    dialog.permission_private = tab->private_tab;
    dialog.permission_rules = std::move(scope);
    dialog.message = controller().tr("Allow access to: ") + names;
    const int id = id_, container = tab->container;
    const bool private_mode = tab->private_tab;
    const auto serial = tab->navigation_serial;
    const auto weak = std::weak_ptr<void>(controller().callback_lifetime);
    auto *owner = &this->controller();
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
    controller().dialogs.push_back(std::move(dialog));
    ++controller().audit["permission_prompts"];
    controller().changed();
    return true;
}
void BrowserClient::OnMediaAccessChange(CefRefPtr<CefBrowser>, bool video, bool audio) {
    if (auto tab = controller().tab(id_)) {
        tab->media_video = video;
        tab->media_audio = audio;
        tab->media_capture = video || audio;
        ++controller().audit["media_access_changes"];
        controller().changed();
    }
}
void BrowserClient::OnDismissPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t prompt,
                                              cef_permission_request_result_t) {
    std::erase_if(controller().dialogs, [&](const auto &d) {
        return d.permission && d.tab == id_ && d.permission_id == prompt;
    });
    controller().changed();
}
void BrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> b) {
    DiagnosticClient::OnAfterCreated(b);
    controller().browser_created(id_, b);
}
void BrowserClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> b, TerminationStatus status,
                                              int code, const CefString &text) {
    DiagnosticClient::OnRenderProcessTerminated(b, status, code, text);
    if (auto t = controller().tab(id_); t && !t->closing) {
        t->loading = false;
        t->progress = 0;
        ++controller().audit["renderer_crashes"];
        controller().changed();
    }
}
void BrowserClient::OnBeforeClose(CefRefPtr<CefBrowser> b) {
    controller().cancel_permissions(id_);
    DiagnosticClient::OnBeforeClose(b);
    controller().browser_closed(id_);
}
bool BrowserClient::OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int popup_id,
                                  const CefString &url, const CefString &,
                                  WindowOpenDisposition disposition, bool user_gesture,
                                  const CefPopupFeatures &, CefWindowInfo &info, CefRefPtr<CefClient> &client,
                                  CefBrowserSettings &, CefRefPtr<CefDictionaryValue> &, bool *) {
    if (controller().profile.block_popups && !user_gesture) {
        ++controller().audit["popups_blocked"];
        return true;
    }
    auto target =
        controller().prepare_popup(id_, popup_id, url, disposition != CEF_WOD_NEW_BACKGROUND_TAB);
    if (!target)
        return true;
    info.SetAsWindowless(controller().window->handle());
    client = new BrowserClient(controller(), *target);
    target->client = static_cast<BrowserClient *>(client.get());
    // Let Chromium create the actual browsing context and retain opener/name/
    // WindowProxy semantics. Do not replace this with a second LoadURL.
    return false;
}
void BrowserClient::OnBeforePopupAborted(CefRefPtr<CefBrowser>, int popup_id) {
    controller().abort_popup(id_, popup_id);
}
bool BrowserClient::OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                     const CefString &url, WindowOpenDisposition disposition,
                                     bool user_gesture) {
    if (disposition == CEF_WOD_CURRENT_TAB)
        return false;
    if (controller().profile.block_popups && !user_gesture) {
        ++controller().audit["popups_blocked"];
        return true;
    }
    controller().open_from_page(id_, url, disposition != CEF_WOD_NEW_BACKGROUND_TAB);
    return true;
}
void BrowserClient::OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, TransitionType) {
    if (!frame->IsMain())
        return;
    if (auto t = controller().tab(id_)) {
        controller().apply_site_zoom(*t, frame->GetURL().ToString());
        t->page.failed = false;
        t->process_ended = false;
        t->page.error.clear();
        t->page.loaded = false;
        t->progress = 0.08;
        controller().changed();
    }
}
void BrowserClient::OnLoadingStateChange(CefRefPtr<CefBrowser>, bool loading, bool back,
                                         bool forward) {
    if (auto t = controller().tab(id_)) {
        t->loading = loading;
        t->back = back;
        t->forward = forward;
        if (!loading)
            t->progress = 1;
        controller().changed();
    }
}
void BrowserClient::OnLoadingProgressChange(CefRefPtr<CefBrowser>, double progress) {
    if (auto t = controller().tab(id_)) {
        t->progress = progress;
        controller().changed();
    }
}
void BrowserClient::OnLoadEnd(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, int status) {
    DiagnosticClient::OnLoadEnd(b, f, status);
    if (f->IsMain()) {
        if (auto t = controller().tab(id_); t && t->browser && !t->ntp)
            controller().apply_site_zoom(*t, f->GetURL().ToString());
        controller().page_changed(id_, true);
        controller().begin_pending_download(id_);
    }
}
void BrowserClient::OnLoadError(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, ErrorCode code,
                                const CefString &text, const CefString &url) {
    DiagnosticClient::OnLoadError(b, f, code, text, url);
    if (f->IsMain() && code != ERR_ABORTED) {
        if (auto tab = controller().tab(id_); tab && !url.empty())
            tab->page.url = url.ToString();
        ++controller().audit["load_errors"];
        controller().page_changed(id_);
    }
}
void BrowserClient::OnTitleChange(CefRefPtr<CefBrowser> b, const CefString &title) {
    if (auto t = controller().tab(id_); t && t->ntp)
        return;
    DiagnosticClient::OnTitleChange(b, title);
    controller().page_changed(id_);
}
void BrowserClient::OnAddressChange(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f,
                                    const CefString &url) {
    if (f->IsMain()) {
        if (auto t = controller().tab(id_)) {
            t->ntp = url == "about:blank#aura-newtab";
            b->GetHost()->WasHidden(!controller().tab_visible(t->id) || t->ntp);
            b->GetHost()->SetAudioMuted(t->muted);
            if (t->ntp) {
                t->page.url = "aura://newtab";
                t->page.title = controller().tr(t->private_tab ? "Private tab" : "New tab");
                controller().page_changed(id_);
                return;
            }
        }
    }
    DiagnosticClient::OnAddressChange(b, f, url);
    controller().page_changed(id_);
}
void BrowserClient::OnFaviconURLChange(CefRefPtr<CefBrowser>, const std::vector<CefString> &urls) {
    if (!urls.empty())
        controller().update_favicon(id_, urls.front());
}
void BrowserClient::OnStatusMessage(CefRefPtr<CefBrowser>, const CefString &value) {
    if (controller().current() == controller().tab(id_) &&
        controller().ui->status != value.ToString()) {
        controller().ui->status = value;
        controller().changed();
    }
}
bool BrowserClient::OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t type,
                                   const CefCursorInfo &) {
    if (controller().current() == controller().tab(id_))
        controller().window->cursor(type == CT_IBEAM ? 1 : type == CT_HAND ? 2 : 0);
    return true;
}
bool BrowserClient::CanDownload(CefRefPtr<CefBrowser>, const CefString &, const CefString &) {
    spdlog::info("Download request allowed");
    return true;
}
bool BrowserClient::OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item,
                                     const CefString &suggested,
                                     CefRefPtr<CefBeforeDownloadCallback> callback) {
    controller().start_download(item, callback, suggested, id_);
    return true;
}
void BrowserClient::OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item,
                                      CefRefPtr<CefDownloadItemCallback> callback) {
    controller().download_updated(item, callback, id_);
}
void BrowserClient::OnFindResult(CefRefPtr<CefBrowser>, int, int count, const CefRect &, int active,
                                 bool) {
    if (controller().current() == controller().tab(id_)) {
        controller().find_matches = count;
        controller().find_active = active;
        controller().audit["find_matches"] = std::max(controller().audit["find_matches"], count);
        controller().changed();
    }
}
void BrowserClient::OnBeforeContextMenu(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> frame,
                                        CefRefPtr<CefContextMenuParams> p,
                                        CefRefPtr<CefMenuModel> model) {
    model->Clear();
    const auto tab = controller().tab(id_);
    if (clipboard_query_) {
        clipboard_query_ = false;
        if (tab && tab == controller().current() && !controller().extension_popup &&
            controller().dialogs.empty() && !controller().ui->editing &&
            controller().ui->visible_panel() == BrowserUi::None &&
            tab->navigation_serial == clipboard_navigation_ &&
            std::chrono::steady_clock::now() < clipboard_deadline_) {
            ++controller().audit["native_clipboard_queries"];
            copy_native_selection(controller(), frame, p, clipboard_cut_);
            controller().changed();
        }
        return;
    }
    spelling_menu_navigation_ = tab ? tab->navigation_serial : 0;
    // CEF 156 can report IsSpellCheckEnabled() == false even for a native
    // spelling marker with suggestions. The marker itself is authoritative;
    // password fields and spellcheck=false elements do not produce one.
    if (controller().profile.spellcheck && p->IsEditable()) {
        if (!p->GetMisspelledWord().empty()) {
            std::vector<CefString> suggestions;
            p->GetDictionarySuggestions(suggestions);
            for (size_t i = 0; i < std::min<size_t>(5, suggestions.size()); ++i)
                model->AddItem(MENU_ID_SPELLCHECK_SUGGESTION_0 + int(i), suggestions[i]);
            if (suggestions.empty()) {
                model->AddItem(MENU_ID_NO_SPELLING_SUGGESTIONS,
                               controller().tr("No spelling suggestions"));
                model->SetEnabled(MENU_ID_NO_SPELLING_SUGGESTIONS, false);
            }
            model->AddItem(MENU_ID_ADD_TO_DICTIONARY,
                           controller().tr(tab && tab->private_tab
                                               ? "Remember for this private session"
                                               : "Add word to dictionary"));
            model->SetEnabled(MENU_ID_ADD_TO_DICTIONARY, tab && tab->spelling_supported);
        }
        model->AddItem(MENU_ID_USER_FIRST + 21, controller().tr("Spelling dictionary"));
        model->AddSeparator();
    }
    if (!p->IsEditable() && !p->GetLinkUrl().empty()) {
        model->AddItem(MENU_ID_USER_FIRST, controller().tr("Open link in new tab"));
        model->AddItem(MENU_ID_USER_FIRST + 1, controller().tr("Copy link address"));
        model->AddSeparator();
    }
    if (!p->IsEditable()) {
        model->AddItem(MENU_ID_BACK, controller().tr("Back"));
        model->SetEnabled(MENU_ID_BACK, b->CanGoBack());
        model->AddItem(MENU_ID_FORWARD, controller().tr("Forward"));
        model->SetEnabled(MENU_ID_FORWARD, b->CanGoForward());
        model->AddItem(MENU_ID_RELOAD, controller().tr("Reload"));
        model->AddSeparator();
    }
    if (p->IsEditable()) {
        model->AddItem(MENU_ID_UNDO, controller().tr("Undo"));
        model->AddItem(MENU_ID_CUT, controller().tr("Cut"));
        model->AddItem(MENU_ID_PASTE, controller().tr("Paste"));
    }
    model->AddItem(MENU_ID_COPY, controller().tr("Copy"));
    model->SetEnabled(MENU_ID_COPY, (p->GetEditStateFlags() & CM_EDITFLAG_CAN_COPY) != 0);
    if (p->IsEditable()) {
        model->SetEnabled(MENU_ID_CUT, (p->GetEditStateFlags() & CM_EDITFLAG_CAN_CUT) != 0);
        model->SetEnabled(MENU_ID_UNDO, (p->GetEditStateFlags() & CM_EDITFLAG_CAN_UNDO) != 0);
    }
    model->AddItem(MENU_ID_SELECT_ALL, controller().tr("Select all"));
    model->AddItem(MENU_ID_USER_FIRST + 3, controller().tr("Translate selection"));
    model->SetEnabled(MENU_ID_USER_FIRST + 3, !p->GetSelectionText().empty());
    model->AddItem(MENU_ID_USER_FIRST + 4, controller().tr("Translate page"));
    model->AddSeparator();
    model->AddItem(MENU_ID_USER_FIRST + 2, controller().tr("Developer tools"));
}
bool BrowserClient::RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                   CefRefPtr<CefContextMenuParams> p, CefRefPtr<CefMenuModel> model,
                                   CefRefPtr<CefRunContextMenuCallback> callback) {
    if (model->GetCount() == 0) {
        callback->Cancel();
        return true;
    }
    controller().close_menu();
    controller().menu_callback = callback;
    controller().menu.clear();
    for (std::size_t i = 0; i < model->GetCount(); ++i)
        controller().menu.push_back(
            {model->GetCommandIdAt(i),
             model->GetTypeAt(i) == MENUITEMTYPE_SEPARATOR ? "" : model->GetLabelAt(i).ToString(),
             model->IsEnabledAt(i)});
    auto target = controller().tab(id_);
    auto bounds = target ? controller().ui->page_bounds(*target) : controller().ui->web_bounds();
    controller().menu_x = std::clamp(bounds.x + int(p->GetXCoord() / controller().ui_scale), 8,
                                     controller().width - 250);
    controller().menu_y = std::clamp(
        bounds.y + int(p->GetYCoord() / controller().ui_scale), 32,
        std::max(32, controller().height - static_cast<int>(controller().menu.size()) * 32 - 12));
    controller().changed();
    return true;
}
bool BrowserClient::OnContextMenuCommand(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                         CefRefPtr<CefContextMenuParams> p, int command,
                                         EventFlags) {
    if (controller().window->backend() == "wayland" &&
        (command == MENU_ID_COPY || command == MENU_ID_CUT || command == MENU_ID_PASTE)) {
        auto &owner = controller();
        const auto navigation = spelling_menu_navigation_;
        const int id = id_;
        auto current = [&owner, navigation, id] {
            auto tab = owner.tab(id);
            return tab && tab == owner.current() && !tab->closing &&
                   tab->navigation_serial == navigation && !owner.extension_popup &&
                   owner.dialogs.empty() && !owner.ui->editing && !owner.ui->find_open &&
                   owner.ui->visible_panel() == BrowserUi::None;
        };
        if (current()) {
            if (command == MENU_ID_PASTE) {
                if (p->IsEditable())
                    paste_native_selection(owner, browser, frame, std::move(current));
            } else
                copy_native_selection(owner, frame, p, command == MENU_ID_CUT);
        }
        return true;
    }
    if (command == MENU_ID_USER_FIRST + 21) {
        controller().ui->open_spelling_dictionary();
        return true;
    }
    if (command == MENU_ID_ADD_TO_DICTIONARY || (command >= MENU_ID_SPELLCHECK_SUGGESTION_0 &&
                                                 command <= MENU_ID_SPELLCHECK_SUGGESTION_LAST)) {
        auto t = controller().tab(id_);
        if (!t || t->closing || t->navigation_serial != spelling_menu_navigation_ ||
            !controller().profile.spellcheck || !p->IsEditable() || p->GetMisspelledWord().empty())
            return true;
        if (command == MENU_ID_ADD_TO_DICTIONARY) {
            if (!t->spelling_supported ||
                !controller().change_spelling_word(p->GetMisspelledWord(), true, t->private_tab))
                controller().ui->toast(controller().tr(
                    "Could not add this word. Check the dictionary and try again."));
        } else {
            std::vector<CefString> suggestions;
            p->GetDictionarySuggestions(suggestions);
            const auto index = size_t(command - MENU_ID_SPELLCHECK_SUGGESTION_0);
            if (index < suggestions.size() && t->browser)
                t->browser->GetHost()->ReplaceMisspelling(suggestions[index]);
        }
        return true;
    }
    if (command == MENU_ID_COPY) {
        if (auto tab = controller().tab(id_); tab && tab->reader_active) {
            const auto text = p->GetSelectionText().ToString();
            if (!text.empty()) {
                controller().window->copy(text);
                ++controller().audit["reader_context_copies"];
            }
            return true;
        }
    }
    if (command == MENU_ID_USER_FIRST) {
        controller().open_from_page(id_, p->GetLinkUrl(), true);
        return true;
    }
    if (command == MENU_ID_USER_FIRST + 1) {
        controller().window->copy(p->GetLinkUrl());
        return true;
    }
    if (command == MENU_ID_USER_FIRST + 2) {
        controller().devtools();
        return true;
    }
    if (command == MENU_ID_USER_FIRST + 3 || command == MENU_ID_USER_FIRST + 4) {
        if (auto tab = controller().tab(id_)) {
            tab->selected_text = p->GetSelectionText().ToString();
            controller().ui->open_translation(command == MENU_ID_USER_FIRST + 3, id_);
        }
        return true;
    }
    return false;
}
bool BrowserClient::OnJSDialog(CefRefPtr<CefBrowser>, const CefString &origin, JSDialogType kind,
                               const CefString &message, const CefString &text,
                               CefRefPtr<CefJSDialogCallback> callback, bool &suppress) {
    if (!controller().profile.allow_dialogs) {
        suppress = true;
        ++controller().audit["dialogs_blocked"];
        return false;
    }
    ++controller().audit["javascript_dialogs"];
    BrowserDialog dialog;
    dialog.tab = id_;
    dialog.origin = url_host(origin);
    dialog.message = message;
    dialog.prompt = kind == JSDIALOGTYPE_PROMPT;
    dialog.input.set(text);
    dialog.callback = callback;
    controller().dialogs.push_back(std::move(dialog));
    controller().changed();
    return true;
}
bool BrowserClient::OnBeforeUnloadDialog(CefRefPtr<CefBrowser>, const CefString &message, bool,
                                         CefRefPtr<CefJSDialogCallback> callback) {
    BrowserDialog dialog;
    dialog.tab = id_;
    dialog.origin = controller().tr("Leave this page?");
    dialog.message = message;
    const auto &merge = controller().state()->duplicate_merge;
    if (merge.active && merge.waiting &&
        merge.waiting->ref == DuplicateRef{controller().window_uid, id_}) {
        if (auto t = controller().tab(id_)) {
            auto title = t->page.title;
            for (auto &c : title)
                if (static_cast<unsigned char>(c) < 32 || c == 127)
                    c = ' ';
            size_t end = std::min<size_t>(120, title.size());
            while (end < title.size() && end &&
                   (static_cast<unsigned char>(title[end]) & 0xc0) == 0x80)
                --end;
            title.resize(end);
            dialog.context = title;
        }
        if (controller().window)
            controller().window->activate();
    }
    dialog.before_unload = true;
    dialog.callback = callback;
    controller().dialogs.push_back(std::move(dialog));
    controller().changed();
    return true;
}
void BrowserClient::OnResetDialogState(CefRefPtr<CefBrowser>) {
    for (auto it = controller().dialogs.begin(); it != controller().dialogs.end();) {
        if (it->tab == id_ && !it->permission)
            it = controller().dialogs.erase(it);
        else
            ++it;
    }
    controller().changed();
}
} // namespace aura

namespace aura {
void BrowserClient::OnTextSelectionChanged(CefRefPtr<CefBrowser>, const CefString &text,
                                           const CefRange &) {
    if (auto t = controller().tab(id_)) {
        t->selected_text = text.ToString();
        if (t->selected_text.size() > 16 * 1024 * 1024)
            t->selected_text.clear();
        controller().changed();
    }
}
} // namespace aura

namespace aura {
bool BrowserClient::GetScreenPoint(CefRefPtr<CefBrowser>, int x, int y, int &screen_x,
                                   int &screen_y) {
    const auto t = controller().tab(id_);
    if (!t || controller().window->backend() == "wayland")
        return false;
    const auto r = controller().ui->page_bounds(*t);
    const auto p = controller().window->screen_point(r.x + int(x / controller().ui_scale),
                                                     r.y + int(y / controller().ui_scale));
    screen_x = p.first;
    screen_y = p.second;
    return true;
}
} // namespace aura

namespace aura {
void BrowserClient::native_copy(bool cut) {
    auto t = controller().tab(id_);
    if (!t || !t->browser || t->closing)
        return;
    clipboard_query_ = true;
    clipboard_cut_ = cut;
    clipboard_navigation_ = t->navigation_serial;
    clipboard_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    request_native_copy(t->browser);
}
} // namespace aura
