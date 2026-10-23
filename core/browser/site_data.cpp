#include "core/browser/browser.hpp"
#include "include/cef_cookie.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "ui/browser_ui.hpp"
#include <cmath>
#include <limits>

namespace aura {
namespace {
class UiTask final : public CefTask {
  public:
    explicit UiTask(std::function<void()> action) : action_(std::move(action)) {}
    void Execute() override { action_(); }

  private:
    std::function<void()> action_;
    IMPLEMENT_REFCOUNTING(UiTask);
};
class Finished final : public CefCompletionCallback {
  public:
    explicit Finished(std::function<void()> action) : action_(std::move(action)) {}
    void OnComplete() override { action_(); }

  private:
    std::function<void()> action_;
    IMPLEMENT_REFCOUNTING(Finished);
};
class SiteCookies final : public CefCookieVisitor {
  public:
    using Done = std::function<void(uint64_t)>;
    SiteCookies(std::string host, bool remove, Done done)
        : host_(std::move(host)), remove_(remove), done_(std::move(done)) {}
    ~SiteCookies() override {
        auto done = std::move(done_);
        const auto count = count_;
        // Also runs for an empty cookie jar. Post after the native visitor's delete operations.
        CefPostTask(TID_UI, new UiTask([done = std::move(done), count] { done(count); }));
    }
    bool Visit(const CefCookie &cookie, int, int, bool &remove) override {
        auto domain = CefString(&cookie.domain).ToString();
        const bool domain_cookie = domain.starts_with('.');
        if (domain_cookie)
            domain.erase(0, 1);
        const bool matches = domain == host_ || (domain_cookie && host_.size() > domain.size() &&
                                                 host_.ends_with("." + domain));
        if (matches) {
            ++count_;
            remove = remove_;
        }
        return true;
    }

  private:
    std::string host_;
    bool remove_;
    Done done_;
    uint64_t count_ = 0;
    IMPLEMENT_REFCOUNTING(SiteCookies);
};
CefRefPtr<CefDictionaryValue> dictionary(const std::string &json) {
    auto v = CefParseJSON(json, JSON_PARSER_RFC);
    return v && v->GetType() == VTYPE_DICTIONARY ? v->GetDictionary() : nullptr;
}
uint64_t bytes(double value) {
    return std::isfinite(value) && value > 0
               ? uint64_t(std::min(value, double(std::numeric_limits<int64_t>::max())))
               : 0;
}
uint64_t metric(CefRefPtr<CefDictionaryValue> value, const char *key) {
    return value->GetType(key) == VTYPE_INT ? bytes(value->GetInt(key))
                                            : bytes(value->GetDouble(key));
}
std::string host(const std::string &origin) {
    CefURLParts p;
    return CefParseURL(origin, p) ? CefString(&p.host).ToString() : std::string{};
}
} // namespace
void BrowserController::open_site_data() {
    ++site_data.serial;
    site_data.pending = 0;
    site_data.clearing = false;
    site_data.message.clear();
    auto t = current();
    site_data.tab = t ? t->id : 0;
    site_data.container = t ? t->container : 0;
    site_data.private_mode = t && t->private_tab;
    site_data.origin =
        t ? permission_origin(t->browser ? t->browser->GetMainFrame()->GetURL().ToString()
                                         : t->page.url)
          : "";
    ui->panel(BrowserUi::SiteData);
    refresh_site_data();
}
void BrowserController::refresh_site_data(const std::string &origin) {
    if (site_data.clearing)
        return;
    ++site_data.serial;
    const auto serial = site_data.serial;
    if (!origin.empty())
        site_data.origin = origin;
    site_data.error.clear();
    site_data.usage = site_data.quota = site_data.cookies = 0;
    site_data.types.clear();
    site_data.pending = 0;
    auto t = tab(site_data.tab);
    if (!valid_permission_origin(site_data.origin) || !t || t->closing || !t->devtools ||
        !t->browser || t->container != site_data.container ||
        t->private_tab != site_data.private_mode) {
        site_data.error = tr("Open a website in this context and reopen Site data.");
        changed();
        return;
    }
    auto context = t->browser->GetHost()->GetRequestContext();
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    site_data.pending = 2;
    site_data.started = std::chrono::steady_clock::now();
    auto valid = [this, weak, serial] { return !weak.expired() && site_data.serial == serial; };
    t->devtools->call(
        "Storage.getUsageAndQuota", "{\"origin\":" + json_quote(site_data.origin) + "}",
        [this, valid](bool ok, const std::string &json) {
            if (!valid())
                return;
            auto p = dictionary(json);
            if (!ok || !p || !p->HasKey("usage"))
                site_data.error = tr("Could not measure site storage. Try refreshing.");
            else {
                site_data.usage = metric(p, "usage");
                site_data.quota = metric(p, "quota");
                if (auto types = p->GetList("usageBreakdown"))
                    for (size_t i = 0; i < types->GetSize(); ++i)
                        if (auto row = types->GetDictionary(i))
                            site_data.types[row->GetString("storageType")] = metric(row, "usage");
            }
            --site_data.pending;
            changed();
        });
    if (!context->GetCookieManager(nullptr)->VisitAllCookies(
            new SiteCookies(host(site_data.origin), false, [this, valid](uint64_t count) {
                if (!valid())
                    return;
                site_data.cookies = count;
                --site_data.pending;
                changed();
            }))) {
        // CEF releases the visitor even if access is rejected; its completion owns pending.
        site_data.error = tr("Could not read cookies for this site.");
    }
    changed();
}
void BrowserController::clear_site_data(bool cookies) {
    auto t = tab(site_data.tab);
    if (site_data.pending || site_data.clearing || !valid_permission_origin(site_data.origin) ||
        !t || t->closing || !t->devtools || !t->browser || t->container != site_data.container ||
        t->private_tab != site_data.private_mode)
        return;
    const auto origin = site_data.origin;
    const auto serial = site_data.serial;
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    BrowserDialog dialog;
    dialog.owned = true;
    dialog.origin = tr(cookies ? "Delete matching cookies?" : "Delete this site's storage?");
    dialog.accept_label = "Delete";
    dialog.message =
        tr(cookies ? "This can sign you out across ports and subdomains sharing a cookie. Other "
                     "containers are unaffected."
                   : "Deletes local storage, databases, caches and service workers for this "
                     "origin. Cookies and permissions are kept. Open pages can create data again.");
    dialog.message += "\n" + origin + " / " + profile.container_label(site_data.container);
    dialog.action = [this, weak, origin, serial, cookies](bool accept, const std::string &) {
        if (weak.expired() || !accept || site_data.serial != serial)
            return;
        auto t = tab(site_data.tab);
        if (!t || t->closing || !t->browser || !t->devtools ||
            t->container != site_data.container || t->private_tab != site_data.private_mode)
            return;
        site_data.clearing = true;
        site_data.started = std::chrono::steady_clock::now();
        auto done = [this, weak, serial](bool ok) {
            if (weak.expired() || site_data.serial != serial)
                return;
            site_data.clearing = false;
            site_data.message =
                ok ? tr("Site data deleted. Reload open pages to see the change.") : "";
            if (ok) {
                ++audit["site_data_clears"];
                refresh_site_data();
            } else {
                site_data.error = tr("Could not delete site data. Try again.");
                changed();
            }
        };
        if (cookies) {
            auto manager = t->browser->GetHost()->GetRequestContext()->GetCookieManager(nullptr);
            const bool queued = manager->VisitAllCookies(
                new SiteCookies(host(origin), true, [manager, done](uint64_t) {
                    if (!manager->FlushStore(new Finished([done] { done(true); })))
                        done(false);
                }));
            if (!queued) {
                done(false);
                ++site_data.serial;
            }
        } else
            t->devtools->call(
                "Storage.clearDataForOrigin",
                "{\"origin\":" + json_quote(origin) +
                    ",\"storageTypes\":\"file_systems,indexeddb,local_storage,websql,service_"
                    "workers,cache_storage,shared_storage,interest_groups,storage_buckets\"}",
                [done](bool ok, const std::string &) { done(ok); });
        changed();
    };
    dialogs.push_back(std::move(dialog));
    changed();
}
} // namespace aura
