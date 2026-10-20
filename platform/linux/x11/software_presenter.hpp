#pragma once
#include "common/geometry.hpp"
#include "include/core/SkSurface.h"
#include <X11/Xlib.h>
#include <memory>
#include <string>
namespace aura {
// CPU backing store, atomic window updates and bounded, synchronized presentation.
class SoftwarePresenter {
  public:
    SoftwarePresenter(Display *display, Window window, GC gc);
    ~SoftwarePresenter();
    bool resize(int width, int height);
    sk_sp<SkSurface> surface() const;
    bool ready() const;
    int refresh_rate();
    void submit(Rect damage);
    bool event(XEvent &event);
    std::string report() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace aura
