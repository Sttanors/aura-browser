#include "core/browser/client.hpp"
#include "include/cef_frame.h"
#include "include/cef_menu_model.h"
#include "include/cef_resource_request_handler.h"
#include "ui/browser_ui.hpp"
#include <spdlog/spdlog.h>
namespace aura {
namespace {
class NetworkPolicy final : public CefResourceRequestHandler {
  public:
    explicit NetworkPolicy(std::shared_ptr<RequestPolicy> policy) : policy_(std::move(policy)) {}
    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                     CefRefPtr<CefRequest> request,
                                     CefRefPtr<CefCallback>) override {
        const auto source = browser && browser->GetMainFrame()
                                ? browser->GetMainFrame()->GetURL().ToString()
                            : frame ? frame->GetURL().ToString()
                                    : request->GetReferrerURL().ToString();
        if (policy_->protection && policy_->protection->block(request->GetURL().ToString(), source,
                                                              request->GetResourceType()))
            return RV_CANCEL;
        if (policy_->do_not_track.load())
            request->SetHeaderByName("DNT", "1", true);
        if (!policy_->send_referrer.load())
            request->SetReferrer("", REFERRER_POLICY_NO_REFERRER);
        return RV_CONTINUE;
    }

  private:
    std::shared_ptr<RequestPolicy> policy_;
    IMPLEMENT_REFCOUNTING(NetworkPolicy);
};
} // namespace
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
        owner_.configure_permissions(
            browser->GetHost()->GetRequestContext(), url,
            frame->IsMain() ? url : browser->GetMainFrame()->GetURL().ToString());
    if (frame->IsMain()) {
        if (auto t = owner_.tab(id_))
            ++t->navigation_serial;
        if (owner_.current() && owner_.current()->id == id_)
            owner_.close_extension_popup();
        owner_.cancel_permissions(id_);
    }
    return false;
}
bool BrowserClient::OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                                   const CefString &origin, uint32_t permissions,
                                                   CefRefPtr<CefMediaAccessCallback> callback) {
    const uint32_t devices =
        CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
    const bool audio = permissions & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE;
    const bool video = permissions & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
    if ((permissions & ~devices) || (!audio && !video) ||
        (audio && owner_.profile.microphone == "block") ||
        (video && owner_.profile.camera == "block")) {
        callback->Cancel();
        ++owner_.audit["permissions_denied"];
        return true;
    }
    BrowserDialog dialog;
    dialog.tab = id_;
    dialog.origin = origin;
    dialog.permission = true;
    dialog.message =
        owner_.tr("Allow access to: ") + owner_.tr(audio && video ? "Camera and microphone"
                                                   : audio        ? "Microphone"
                                                                  : "Camera");
    dialog.action = [callback, permissions](bool allow, const std::string &) {
        if (allow)
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
    struct Policy {
        uint32_t bit;
        const std::string *value;
        const char *name;
    };
    const auto &p = owner_.profile;
    const Policy policies[] = {
        {CEF_PERMISSION_TYPE_NOTIFICATIONS, &p.notifications, "Notifications"},
        {CEF_PERMISSION_TYPE_GEOLOCATION, &p.geolocation, "Location"},
        {CEF_PERMISSION_TYPE_CAMERA_STREAM | CEF_PERMISSION_TYPE_CAMERA_PAN_TILT_ZOOM, &p.camera,
         "Camera"},
        {CEF_PERMISSION_TYPE_MIC_STREAM, &p.microphone, "Microphone"},
        {CEF_PERMISSION_TYPE_CLIPBOARD, &p.clipboard, "Clipboard"},
        {CEF_PERMISSION_TYPE_MIDI_SYSEX, &p.midi, "MIDI devices"},
        {CEF_PERMISSION_TYPE_SENSORS, &p.sensors, "Motion sensors"}};
    uint32_t known = 0;
    bool blocked = false;
    std::string names;
    for (const auto &policy : policies) {
        known |= policy.bit;
        if (permissions & policy.bit) {
            blocked |= *policy.value == "block";
            if (!names.empty())
                names += ", ";
            names += owner_.tr(policy.name);
        }
    }
    if (!permissions || (permissions & ~known) || blocked) {
        callback->Continue(CEF_PERMISSION_RESULT_DENY);
        ++owner_.audit["permissions_denied"];
        return true;
    }
    BrowserDialog dialog;
    dialog.tab = id_;
    dialog.origin = origin;
    dialog.permission = true;
    dialog.permission_id = prompt;
    dialog.message = owner_.tr("Allow access to: ") + names;
    dialog.action = [callback](bool allow, const std::string &) {
        callback->Continue(allow ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY);
    };
    owner_.dialogs.push_back(std::move(dialog));
    ++owner_.audit["permission_prompts"];
    owner_.changed();
    return true;
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
bool BrowserClient::OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
                                  const CefString &url, const CefString &,
                                  WindowOpenDisposition disposition, bool user_gesture,
                                  const CefPopupFeatures &, CefWindowInfo &, CefRefPtr<CefClient> &,
                                  CefBrowserSettings &, CefRefPtr<CefDictionaryValue> &, bool *) {
    if (owner_.profile.block_popups && !user_gesture) {
        ++owner_.audit["popups_blocked"];
        return true;
    }
    owner_.open_from_page(id_, url, disposition != CEF_WOD_NEW_BACKGROUND_TAB);
    return true;
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
            t->browser->GetHost()->SetZoomLevel(t->zoom);
        owner_.page_changed(id_, true);
    }
}
void BrowserClient::OnLoadError(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, ErrorCode code,
                                const CefString &text, const CefString &url) {
    DiagnosticClient::OnLoadError(b, f, code, text, url);
    if (f->IsMain() && code != ERR_ABORTED) {
        ++owner_.audit["load_errors"];
        owner_.changed();
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
    if (auto t = owner_.tab(id_); t && t->ntp)
        return;
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
    owner_.start_download(item, callback, suggested);
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
    auto bounds = owner_.ui->web_bounds();
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
