#include "common/config.hpp"
#include "core/browser/browser.hpp"
#include "core/browser/native_clipboard.hpp"
#include "include/cef_context_menu_handler.h"
#include "include/cef_frame.h"
#include "include/cef_image.h"
#include "ui/browser_ui.hpp"
#include <algorithm>
#include <cstring>
namespace aura {
void BrowserController::edit_extension_access(const std::string &id, const std::string &operation,
                                              const std::string &value) {
    if (safe_mode()) {
        ui->toast(tr("Extensions are paused in safe mode."));
        return;
    }
    if (quitting || private_window ||
        (current() && (current()->private_tab || current()->container))) {
        ui->toast(tr("Site access is managed from a regular tab in the default container."));
        return;
    }
    const auto it = std::find_if(extensions.items.begin(), extensions.items.end(),
                                 [&](const auto &e) { return e.id == id; });
    if (it == extensions.items.end() || !it->enabled || !it->load_requested || it->pending_update) {
        ui->toast(tr("Restart Aura to load the extension."));
        return;
    }
    state_->extension_access.request(id, operation, value);
    changed();
}
namespace {
class ExtensionPopupClient final : public DiagnosticClient,
                                   public CefContextMenuHandler,
                                   public CefJSDialogHandler {
  public:
    ExtensionPopupClient(BrowserController &owner, std::shared_ptr<ExtensionPopup> popup)
        : DiagnosticClient(popup->page), owner_(owner), popup_(std::move(popup)) {}
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
        return this;
    }
    bool clipboard_key(int code) {
        if (!current() || (code != 'C' && code != 'X' && code != 'V'))
            return false;
        if (code == 'V') {
            auto popup = popup_;
            auto &owner = owner_;
            const auto navigation = popup->navigation_serial;
            paste_native_selection(owner, popup->browser, popup->browser->GetFocusedFrame(),
                                   [&owner, popup, navigation] {
                                       return owner.extension_popup == popup &&
                                              !popup->page.closing &&
                                              popup->navigation_serial == navigation &&
                                              owner.dialogs.empty() && !owner.ui->editing &&
                                              !owner.ui->find_open &&
                                              owner.ui->visible_panel() == BrowserUi::None;
                                   });
        } else {
            clipboard_query_ = true;
            clipboard_cut_ = code == 'X';
            clipboard_navigation_ = popup_->navigation_serial;
            clipboard_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            request_native_copy(popup_->browser);
        }
        return true;
    }
    void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                             CefRefPtr<CefContextMenuParams> p,
                             CefRefPtr<CefMenuModel> model) override {
        model->Clear();
        if (clipboard_query_) {
            clipboard_query_ = false;
            if (current() && popup_->navigation_serial == clipboard_navigation_ &&
                std::chrono::steady_clock::now() < clipboard_deadline_) {
                ++owner_.audit["native_clipboard_queries"];
                copy_native_selection(owner_, frame, p, clipboard_cut_);
                owner_.changed();
            }
            return;
        }
        if (!current() || owner_.window->backend() != "wayland")
            return;
        menu_navigation_ = popup_->navigation_serial;
        if (p->IsEditable()) {
            model->AddItem(MENU_ID_UNDO, owner_.tr("Undo"));
            model->SetEnabled(MENU_ID_UNDO, (p->GetEditStateFlags() & CM_EDITFLAG_CAN_UNDO) != 0);
            model->AddItem(MENU_ID_CUT, owner_.tr("Cut"));
            model->SetEnabled(MENU_ID_CUT, (p->GetEditStateFlags() & CM_EDITFLAG_CAN_CUT) != 0);
        }
        model->AddItem(MENU_ID_COPY, owner_.tr("Copy"));
        model->SetEnabled(MENU_ID_COPY, (p->GetEditStateFlags() & CM_EDITFLAG_CAN_COPY) != 0);
        if (p->IsEditable())
            model->AddItem(MENU_ID_PASTE, owner_.tr("Paste"));
        model->AddItem(MENU_ID_SELECT_ALL, owner_.tr("Select all"));
    }
    bool RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                        CefRefPtr<CefContextMenuParams> p, CefRefPtr<CefMenuModel> model,
                        CefRefPtr<CefRunContextMenuCallback> callback) override {
        if (!current() || !model->GetCount()) {
            callback->Cancel();
            return true;
        }
        owner_.close_menu();
        owner_.menu_callback = callback;
        for (size_t i = 0; i < model->GetCount(); ++i)
            owner_.menu.push_back(
                {model->GetCommandIdAt(i), model->GetLabelAt(i).ToString(), model->IsEnabledAt(i)});
        owner_.menu_x = std::clamp(popup_->content.x + int(p->GetXCoord() / owner_.ui_scale), 8,
                                   owner_.width - 250);
        owner_.menu_y = std::clamp(popup_->content.y + int(p->GetYCoord() / owner_.ui_scale), 32,
                                   std::max(32, owner_.height - int(owner_.menu.size()) * 32 - 12));
        owner_.changed();
        return true;
    }
    bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                              CefRefPtr<CefContextMenuParams> p, int command, EventFlags) override {
        if (!current() || popup_->navigation_serial != menu_navigation_)
            return true;
        if (command == MENU_ID_COPY || command == MENU_ID_CUT) {
            copy_native_selection(owner_, frame, p, command == MENU_ID_CUT);
            return true;
        }
        if (command == MENU_ID_PASTE) {
            if (p->IsEditable()) {
                auto popup = popup_;
                auto &owner = owner_;
                const auto navigation = popup->navigation_serial;
                paste_native_selection(owner, browser, frame, [&owner, popup, navigation] {
                    return owner.extension_popup == popup && !popup->page.closing &&
                           popup->navigation_serial == navigation && owner.dialogs.empty();
                });
            }
            return true;
        }
        return false;
    }
    CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override {
        return this;
    }
    bool OnJSDialog(CefRefPtr<CefBrowser> browser, const CefString &, JSDialogType kind,
                    const CefString &message, const CefString &text,
                    CefRefPtr<CefJSDialogCallback> callback, bool &) override {
        if (popup_->page.closing || owner_.extension_popup != popup_) {
            callback->Continue(false, "");
            return true;
        }
        BrowserDialog dialog;
        dialog.tab = -browser->GetIdentifier();
        dialog.origin = popup_->name;
        dialog.message = message;
        dialog.prompt = kind == JSDIALOGTYPE_PROMPT;
        dialog.input.set(text);
        dialog.callback = callback;
        owner_.dialogs.push_back(std::move(dialog));
        ++owner_.audit["extension_popup_dialogs"];
        owner_.changed();
        return true;
    }
    void OnResetDialogState(CefRefPtr<CefBrowser> browser) override {
        const auto id = -browser->GetIdentifier();
        std::erase_if(owner_.dialogs, [&](const auto &d) { return d.tab == id; });
        owner_.changed();
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        DiagnosticClient::OnAfterCreated(browser);
        owner_.popup_created(popup_, browser);
    }
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        OnResetDialogState(browser);
        DiagnosticClient::OnBeforeClose(browser);
        owner_.popup_closed(popup_);
    }
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status,
                                   int code, const CefString &error) override {
        DiagnosticClient::OnRenderProcessTerminated(browser, status, code, error);
        ++owner_.audit["renderer_crashes"];
        owner_.defer([&owner = owner_, popup = popup_] {
            if (owner.extension_popup == popup)
                owner.close_extension_popup();
        });
    }
    void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, TransitionType) override {
        ++popup_->navigation_serial;
        if (owner_.extension_popup == popup_)
            owner_.close_menu();
        if (frame->IsMain()) {
            popup_->page.loaded = false;
            popup_->page.failed = false;
            owner_.changed();
        }
    }
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int status) override {
        DiagnosticClient::OnLoadEnd(browser, frame, status);
        if (frame->IsMain() && !popup_->page.failed &&
            frame->GetURL().ToString().starts_with("chrome-extension://")) {
            owner_.extensions.observe_origin(frame->GetURL());
            owner_.defer([&owner = owner_, popup = popup_] {
                if (owner.extension_popup == popup && !popup->page.closing) {
                    owner.resize_extension_popup();
                }
            });
        }
    }
    bool OnAutoResize(CefRefPtr<CefBrowser>, const CefSize &size) override {
        if (popup_->page.closing || !popup_->page.loaded)
            return true;
        owner_.defer([&owner = owner_, popup = popup_, size] {
            if (owner.extension_popup != popup || popup->page.closing)
                return;
            popup->desired_width = size.width;
            popup->desired_height = size.height;
            owner.ui->layout_extension_popup();
            owner.changed();
        });
        return true;
    }
    bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request, bool, bool) override {
        if (!frame->IsMain())
            return false;
        const auto url = request->GetURL().ToString();
        if (url == "about:blank" ||
            url.starts_with("chrome-extension://" + popup_->extension_id + "/"))
            return false;
        if (url.starts_with("https://") || url.starts_with("http://")) {
            owner_.defer([&owner = owner_, url] {
                owner.close_extension_popup();
                owner.new_tab(url);
            });
        }
        return true;
    }
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString &url,
                       const CefString &, WindowOpenDisposition, bool user_gesture,
                       const CefPopupFeatures &, CefWindowInfo &, CefRefPtr<CefClient> &,
                       CefBrowserSettings &, CefRefPtr<CefDictionaryValue> &, bool *) override {
        const auto target = url.ToString();
        if (user_gesture &&
            (target.starts_with("https://") || target.starts_with("http://") ||
             target.starts_with("chrome-extension://" + popup_->extension_id + "/")))
            owner_.defer([&owner = owner_, target] {
                owner.close_extension_popup();
                owner.new_tab(target);
            });
        return true;
    }
    bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t type,
                        const CefCursorInfo &) override {
        if (owner_.extension_popup == popup_)
            owner_.window->cursor(type == CT_IBEAM ? 1 : type == CT_HAND ? 2 : 0);
        return true;
    }

  private:
    bool current() const {
        return owner_.extension_popup == popup_ && popup_->browser && !popup_->page.closing &&
               owner_.dialogs.empty() && !owner_.quitting;
    }
    bool clipboard_query_ = false, clipboard_cut_ = false;
    uint64_t clipboard_navigation_ = 0, menu_navigation_ = 0;
    std::chrono::steady_clock::time_point clipboard_deadline_;
    BrowserController &owner_;
    std::shared_ptr<ExtensionPopup> popup_;
    IMPLEMENT_REFCOUNTING(ExtensionPopupClient);
};
} // namespace
bool BrowserController::extension_clipboard_key(int code) {
    if (!extension_popup || !extension_popup->client || window->backend() != "wayland")
        return false;
    return static_cast<ExtensionPopupClient *>(extension_popup->client.get())->clipboard_key(code);
}
void BrowserController::open_extension_popup(size_t index) {
    if (safe_mode()) {
        ui->toast(tr("Extensions are paused in safe mode."));
        return;
    }
    if (index >= extensions.items.size() || quitting)
        return;
    const auto e = extensions.items[index];
    if (current() && current()->container) {
        ui->toast(tr("Extension popups are available in the default container."));
        return;
    }
    if (current() && current()->private_tab) {
        ui->toast(tr("Extensions are unavailable in private tabs."));
        return;
    }
    if (!e.enabled || !e.load_requested) {
        ui->toast(tr("Restart Aura to load the extension."));
        return;
    }
    if (e.popup.empty()) {
        ui->toast(tr("This extension has no page to open."));
        return;
    }
    if (extension_popup && extension_popup->extension_id == e.id) {
        close_extension_popup();
        return;
    }
    close_extension_popup(false);
    ui->panel(BrowserUi::None);
    ui->find_open = false;
    ui->editing = false;
    close_menu();
    auto popup = std::make_shared<ExtensionPopup>();
    popup->extension_id = e.id;
    popup->name = e.name;
    popup->url = "chrome-extension://" + e.id + "/" + e.popup;
    popup->page.url = popup->url;
    popup->page.scale = scale * ui_scale;
    popup->page.invalidate = [this] { changed(); };
    popup->page.ime_caret = [this,
                             weak = std::weak_ptr<ExtensionPopup>(popup)](const CefRect &caret) {
        if (auto p = weak.lock(); p && extension_popup == p)
            window->caret(p->content.x + caret.x, p->content.y + caret.y + caret.height);
    };
    extension_popup = popup;
    ui->layout_extension_popup();
    popup->client = new ExtensionPopupClient(*this, popup);
    CefWindowInfo info;
    info.SetAsWindowless(window->handle());
    CefBrowserSettings settings;
    settings.windowless_frame_rate = window->refresh_rate();
    settings.background_color = 0xFFFFFFFF;
    // Extension UI keeps its own scripts, images and storage regardless of website content
    // settings.
    settings.javascript = STATE_ENABLED;
    settings.image_loading = STATE_ENABLED;
    settings.local_storage = STATE_ENABLED;
    if (auto t = current(); t && t->browser)
        t->browser->GetHost()->SetFocus(false);
    popup->browser =
        CefBrowserHost::CreateBrowserSync(info, popup->client, "about:blank", settings, nullptr,
                                          CefRequestContext::GetGlobalContext());
    if (!popup->browser) {
        popup->client = nullptr;
        extension_popup.reset();
        ui->toast(tr("Could not open extension popup."));
        return;
    }
    popup->browser->GetHost()->SetZoomLevel(0);
    popup->browser->GetHost()->SetFocus(true);
    changed();
}
void BrowserController::popup_created(const std::shared_ptr<ExtensionPopup> &popup,
                                      CefRefPtr<CefBrowser> browser) {
    popup->browser = browser;
    ++created_;
    ++audit["extension_popups_opened"];
    defer([this, popup] {
        if (extension_popup == popup && popup->browser && !popup->page.closing)
            popup->browser->GetMainFrame()->LoadURL(popup->url);
    });
}
void BrowserController::popup_closed(const std::shared_ptr<ExtensionPopup> &popup) {
    popup->page.closed = true;
    popup->browser = nullptr;
    popup->client = nullptr;
    ++closed_;
    ++audit["extension_popups_closed"];
    if (extension_popup == popup) {
        close_menu();
        extension_popup.reset();
        if (!quitting)
            if (auto t = current(); t && t->browser)
                t->browser->GetHost()->SetFocus(true);
    }
    changed();
}
void BrowserController::close_extension_popup(bool restore_focus) {
    if (!extension_popup)
        return;
    close_menu();
    auto popup = std::move(extension_popup);
    closing_popups_.push_back(popup);
    popup->page.closing = true;
    if (popup->browser) {
        popup->browser->GetHost()->SetFocus(false);
        popup->browser->GetHost()->CloseBrowser(true);
    } else
        popup->page.closed = true;
    if (restore_focus && !quitting)
        if (auto t = current(); t && t->browser)
            t->browser->GetHost()->SetFocus(true);
    window->cursor(0);
    changed();
}
void BrowserController::resize_extension_popup() {
    if (auto p = extension_popup) {
        p->page.scale = scale * ui_scale;
        if (p->browser) {
            p->browser->GetHost()->NotifyScreenInfoChanged();
            p->browser->GetHost()->WasResized();
        }
    }
    ui->layout_extension_popup();
    if (auto p = extension_popup; p && p->browser && !p->page.closing) {
        p->browser->GetHost()->SetAutoResizeEnabled(
            true, CefSize(180, 80),
            CefSize(std::min(800, width - 296), std::min(600, height - 140)));
    }
}
void BrowserController::pin_extension(size_t index) {
    if (index >= extensions.items.size())
        return;
    extensions.pin(index);
    ui->layout_extension_popup();
    changed();
}
const PaintBuffer &BrowserController::extension_icon(size_t index) {
    static const PaintBuffer empty;
    if (index >= extensions.items.size())
        return empty;
    const auto &e = extensions.items[index];
    const auto key = e.id + ":" + e.directory;
    if (auto it = extension_icons_.find(key); it != extension_icons_.end())
        return it->second;
    auto &buffer = extension_icons_[key];
    auto path = extensions.icon_path(index);
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path, error) ||
        std::filesystem::file_size(path, error) > 1024 * 1024)
        return buffer;
    // The minimal Skia build omits image decoders. Use Chromium's PNG decoder,
    // checking IHDR first so a small encoded icon cannot allocate a huge bitmap.
    auto data = read_resource(path);
    if (data.size() < 24 || std::memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8))
        return buffer;
    auto dimension = [&](size_t at) {
        return (uint32_t(uint8_t(data[at])) << 24) | (uint32_t(uint8_t(data[at + 1])) << 16) |
               (uint32_t(uint8_t(data[at + 2])) << 8) | uint32_t(uint8_t(data[at + 3]));
    };
    const auto expected_w = dimension(16), expected_h = dimension(20);
    if (!expected_w || !expected_h || expected_w > 512 || expected_h > 512)
        return buffer;
    auto icon = CefImage::CreateImage();
    if (!icon->AddPNG(1, data.data(), data.size()))
        return buffer;
    int w = 0, h = 0;
    auto bitmap =
        icon->GetAsBitmap(1, CEF_COLOR_TYPE_BGRA_8888, CEF_ALPHA_TYPE_PREMULTIPLIED, w, h);
    if (bitmap && w == int(expected_w) && h == int(expected_h)) {
        std::vector<uint8_t> bytes(bitmap->GetSize());
        bitmap->GetData(bytes.data(), bytes.size(), 0);
        buffer.update(w, h, bytes, {});
    }
    return buffer;
}
} // namespace aura
