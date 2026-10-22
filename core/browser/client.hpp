#pragma once
#include "core/browser/browser.hpp"
#include "include/cef_find_handler.h"
#include "include/cef_permission_handler.h"
namespace aura {
class BrowserClient final : public DiagnosticClient,
                            public CefDownloadHandler,
                            public CefFindHandler,
                            public CefContextMenuHandler,
                            public CefJSDialogHandler,
                            public CefPermissionHandler {
  public:
    BrowserClient(BrowserController &owner, Tab &tab)
        : DiagnosticClient(tab.page), owner_(owner), id_(tab.id), policy_(owner.request_policy) {}
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override {
        return this;
    }
    CefRefPtr<CefFindHandler> GetFindHandler() override {
        return this;
    }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
        return this;
    }
    CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override {
        return this;
    }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override {
        return this;
    }
    CefRefPtr<CefResourceRequestHandler>
    GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>,
                              bool, bool, const CefString &, bool &) override;
    bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, bool,
                        bool) override;
    bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                        const CefString &, uint32_t,
                                        CefRefPtr<CefMediaAccessCallback>) override;
    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t, const CefString &, uint32_t,
                                CefRefPtr<CefPermissionPromptCallback>) override;
    void OnDismissPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t,
                                   cef_permission_request_result_t) override;
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList &, const void *, int,
                 int) override;
    void OnTextSelectionChanged(CefRefPtr<CefBrowser>, const CefString &,
                                const CefRange &) override;
#ifdef _WIN32
    void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList &,
                            const CefAcceleratedPaintInfo &) override;
#endif
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus, int,
                                   const CefString &) override;
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString &,
                       const CefString &, WindowOpenDisposition, bool, const CefPopupFeatures &,
                       CefWindowInfo &, CefRefPtr<CefClient> &, CefBrowserSettings &,
                       CefRefPtr<CefDictionaryValue> &, bool *) override;
    bool OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString &,
                          WindowOpenDisposition, bool) override;
    void OnBeforePopupAborted(CefRefPtr<CefBrowser>, int popup_id) override;
    void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, TransitionType) override;
    void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool, bool, bool) override;
    void OnLoadingProgressChange(CefRefPtr<CefBrowser>, double) override;
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int) override;
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, ErrorCode, const CefString &,
                     const CefString &) override;
    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString &) override;
    void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString &) override;
    void OnFaviconURLChange(CefRefPtr<CefBrowser>, const std::vector<CefString> &) override;
    void OnStatusMessage(CefRefPtr<CefBrowser>, const CefString &) override;
    bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t,
                        const CefCursorInfo &) override;
    bool CanDownload(CefRefPtr<CefBrowser>, const CefString &, const CefString &) override;
    bool OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>, const CefString &,
                          CefRefPtr<CefBeforeDownloadCallback>) override;
    void OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>,
                           CefRefPtr<CefDownloadItemCallback>) override;
    void OnFindResult(CefRefPtr<CefBrowser>, int, int, const CefRect &, int, bool) override;
    void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                             CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel>) override;
    bool RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>,
                        CefRefPtr<CefMenuModel>, CefRefPtr<CefRunContextMenuCallback>) override;
    bool OnContextMenuCommand(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                              CefRefPtr<CefContextMenuParams>, int, EventFlags) override;
    bool OnJSDialog(CefRefPtr<CefBrowser>, const CefString &, JSDialogType, const CefString &,
                    const CefString &, CefRefPtr<CefJSDialogCallback>, bool &) override;
    bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser>, const CefString &, bool,
                              CefRefPtr<CefJSDialogCallback>) override;
    void OnResetDialogState(CefRefPtr<CefBrowser>) override;

  private:
    BrowserController &owner_;
    int id_;
    std::shared_ptr<RequestPolicy> policy_;
    IMPLEMENT_REFCOUNTING(BrowserClient);
};
} // namespace aura
