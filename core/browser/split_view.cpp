#include "core/browser/browser.hpp"
#include "ui/browser_ui.hpp"
#include <algorithm>
#include <cmath>
namespace aura {
bool BrowserController::tab_visible(int id) const {
    return (active >= 0 && active < int(tabs.size()) && tabs[active]->id == id) ||
           (split_enabled() && (split_tabs[0] == id || split_tabs[1] == id));
}
void BrowserController::resize_pages() {
    for (auto &t : tabs) {
        const auto r = ui->page_bounds(*t);
        t->page.scale = scale;
        const int w = std::max(1, int(std::lround(r.width * ui_scale)));
        const int h = std::max(1, int(std::lround(r.height * ui_scale)));
        const bool resized = t->page.view_width != w || t->page.view_height != h;
        t->page.view_width = w;
        t->page.view_height = h;
        if (t->browser) {
            if (resized)
                t->browser->GetHost()->WasResized();
            t->browser->GetHost()->NotifyScreenInfoChanged();
            t->browser->GetHost()->WasHidden(!tab_visible(t->id) || t->ntp);
        }
    }
}
void BrowserController::end_split() {
    if (!split_enabled())
        return;
    split_tabs = {};
    resize_pages();
    changed(true);
}
void BrowserController::toggle_split() {
    if (split_enabled()) {
        end_split();
        return;
    }
    auto t = current();
    if (!t || t->ntp || t->closing) {
        ui->toast(tr("Open two websites in this workspace first."));
        return;
    }
    const auto visible = visible_tabs();
    auto other = std::find_if(visible.begin(), visible.end(), [&](int i) {
        return tabs[i].get() != t && !tabs[i]->ntp && !tabs[i]->closing;
    });
    if (other == visible.end()) {
        ui->toast(tr("Open two websites in this workspace first."));
        return;
    }
    split_tabs = {t->id, tabs[*other]->id};
    wake_tab(split_tabs[0]);
    wake_tab(split_tabs[1]);
    resize_pages();
    changed(true);
}
} // namespace aura
