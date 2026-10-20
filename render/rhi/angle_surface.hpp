#pragma once
#ifdef _WIN32
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include <windows.h>
#include <memory>
namespace aura {
class AngleSurface {
public:
    explicit AngleSurface(HWND window);
    ~AngleSurface();
    AngleSurface(const AngleSurface&) = delete;
    AngleSurface& operator=(const AngleSurface&) = delete;
    SkCanvas& begin(int width, int height);
    void present();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    sk_sp<GrDirectContext> context_;
    sk_sp<SkSurface> surface_;
    int width_{}, height_{};
};
}
#endif
