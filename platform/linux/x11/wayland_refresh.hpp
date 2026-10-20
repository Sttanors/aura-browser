#pragma once
#include "common/geometry.hpp"
#include <memory>
namespace aura {
// Read-only wl_output connection: no surfaces, graphics contexts or GPU access.
class WaylandRefresh {
  public:
    WaylandRefresh();
    ~WaylandRefresh();
    int rate(Rect window);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace aura
