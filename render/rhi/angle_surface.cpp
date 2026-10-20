#include "render/rhi/angle_surface.hpp"
#include "include/core/SkColorSpace.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include <stdexcept>
#include <string>

namespace aura {
// The small EGL ABI below is loaded from CEF's matching ANGLE DLLs.
// EGLint=int, EGLBoolean=unsigned int, EGL handles=opaque pointers (Khronos EGL ABI).
struct AngleSurface::Impl {
    using Handle = void*;
    using Boolean = unsigned int;
    using Proc = void(*)();
    HMODULE egl{}, gles{};
    Handle display{}, config{}, context{}, window_surface{};
    using GetProcAddressFn = Proc(WINAPI*)(const char*);
    GetProcAddressFn get_proc{};
    Boolean(WINAPI* make_current)(Handle, Handle, Handle, Handle){};
    Boolean(WINAPI* swap_buffers)(Handle, Handle){};
    Boolean(WINAPI* destroy_surface)(Handle, Handle){};
    Boolean(WINAPI* destroy_context)(Handle, Handle){};
    Boolean(WINAPI* terminate)(Handle){};
    template<class Function> Function symbol(const char* name) {
        const auto pointer = GetProcAddress(egl, name);
        if (!pointer) throw std::runtime_error(std::string("Missing EGL entry point: ") + name);
        return reinterpret_cast<Function>(pointer);
    }
    ~Impl() {
        if (display && make_current) make_current(display, nullptr, nullptr, nullptr);
        if (display && window_surface && destroy_surface) destroy_surface(display, window_surface);
        if (display && context && destroy_context) destroy_context(display, context);
        if (display && terminate) terminate(display);
        if (gles) FreeLibrary(gles);
        if (egl) FreeLibrary(egl);
    }
};
AngleSurface::AngleSurface(HWND window) : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_;
    p.egl = LoadLibraryExW(L"libEGL.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    p.gles = LoadLibraryExW(L"libGLESv2.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!p.egl || !p.gles) throw std::runtime_error("CEF ANGLE DLLs unavailable");
    p.get_proc = p.symbol<Impl::GetProcAddressFn>("eglGetProcAddress");
    const auto platform_display = reinterpret_cast<Impl::Handle(WINAPI*)(unsigned int, void*, const int*)>(
        p.get_proc("eglGetPlatformDisplayEXT"));
    if (!platform_display) throw std::runtime_error("EGL_EXT_platform_base unavailable");
    constexpr int none = 0x3038;
    const int display_attributes[] = {0x3203, 0x3208, none}; // ANGLE type = D3D11.
    p.display = platform_display(0x3202, nullptr, display_attributes);
    const auto initialize = p.symbol<Impl::Boolean(WINAPI*)(void*, int*, int*)>("eglInitialize");
    if (!p.display || !initialize(p.display, nullptr, nullptr)) throw std::runtime_error("ANGLE D3D11 initialization failed");
    p.make_current = p.symbol<decltype(p.make_current)>("eglMakeCurrent");
    p.swap_buffers = p.symbol<decltype(p.swap_buffers)>("eglSwapBuffers");
    p.destroy_surface = p.symbol<decltype(p.destroy_surface)>("eglDestroySurface");
    p.destroy_context = p.symbol<decltype(p.destroy_context)>("eglDestroyContext");
    p.terminate = p.symbol<decltype(p.terminate)>("eglTerminate");
    const int config_attributes[] = {0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3033,4,0x3040,0x40,none};
    int count = 0;
    const auto choose = p.symbol<Impl::Boolean(WINAPI*)(void*, const int*, void**, int, int*)>("eglChooseConfig");
    if (!choose(p.display, config_attributes, &p.config, 1, &count) || count != 1)
        throw std::runtime_error("ANGLE GLES3 config unavailable");
    const auto create_context = p.symbol<void*(WINAPI*)(void*, void*, void*, const int*)>("eglCreateContext");
    const int context_attributes[] = {0x3098, 3, none};
    p.context = create_context(p.display, p.config, nullptr, context_attributes);
    const auto create_surface = p.symbol<void*(WINAPI*)(void*, void*, HWND, const int*)>("eglCreateWindowSurface");
    p.window_surface = create_surface(p.display, p.config, window, nullptr);
    if (!p.context || !p.window_surface || !p.make_current(p.display, p.window_surface, p.window_surface, p.context))
        throw std::runtime_error("ANGLE surface creation failed");
    auto interface = GrGLMakeAssembledGLESInterface(&p, [](void* context, const char* name) -> GrGLFuncPtr {
        const auto& api = *static_cast<Impl*>(context);
        auto pointer = reinterpret_cast<GrGLFuncPtr>(GetProcAddress(api.gles, name));
        if (!pointer) pointer = reinterpret_cast<GrGLFuncPtr>(api.get_proc(name));
        return pointer;
    });
    context_ = GrDirectContexts::MakeGL(std::move(interface));
    if (!context_) throw std::runtime_error("Skia Ganesh could not create GLES context");
    const auto interval = p.symbol<Impl::Boolean(WINAPI*)(void*, int)>("eglSwapInterval");
    interval(p.display, 1);
}
AngleSurface::~AngleSurface() {
    surface_.reset();
    if (context_) context_->releaseResourcesAndAbandonContext();
    context_.reset();
}
SkCanvas& AngleSurface::begin(int width, int height) {
    auto& p = *impl_;
    if (!p.make_current(p.display, p.window_surface, p.window_surface, p.context))
        throw std::runtime_error("eglMakeCurrent failed");
    if (!surface_ || width != width_ || height != height_) {
        surface_.reset();
        const GrGLFramebufferInfo info{0, 0x8058}; // Default framebuffer / GL_RGBA8.
        const auto target = GrBackendRenderTargets::MakeGL(width, height, 0, 0, info);
        surface_ = SkSurfaces::WrapBackendRenderTarget(context_.get(), target,
            kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType, nullptr, nullptr);
        if (!surface_) throw std::runtime_error("Skia GPU surface unavailable");
        width_ = width; height_ = height;
    }
    return *surface_->getCanvas();
}
void AngleSurface::present() {
    context_->flushAndSubmit();
    if (!impl_->swap_buffers(impl_->display, impl_->window_surface))
        throw std::runtime_error("eglSwapBuffers failed");
}
}
