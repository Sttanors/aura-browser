#include "render/cef_bridge/shared_texture_probe.hpp"
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
namespace aura {
namespace {
void checked(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(result));
}
}
void SharedTextureProbe::copy_and_read(HANDLE handle, PaintBuffer& target) {
    if (!device_) {
        Microsoft::WRL::ComPtr<ID3D11Device> base;
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &base, nullptr, &context_), "D3D11CreateDevice");
        checked(base.As(&device_), "ID3D11Device1");
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    checked(device_->OpenSharedResource1(handle, IID_PPV_ARGS(&source)), "OpenSharedResource1");
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Width > 16384 || desc.Height > 16384 || desc.ArraySize != 1 || desc.MipLevels != 1)
        throw std::runtime_error("Unsupported CEF shared texture dimensions");
    if (static_cast<std::uint64_t>(desc.Width) * desc.Height * 4 > 256 * 1024 * 1024)
        throw std::runtime_error("CEF shared texture exceeds M0 memory budget");
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!rgba && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        throw std::runtime_error("Unsupported CEF shared texture format");
    D3D11_TEXTURE2D_DESC previous{};
    if (staging_) staging_->GetDesc(&previous);
    if (!staging_ || previous.Width != desc.Width || previous.Height != desc.Height || previous.Format != desc.Format) {
        staging_.Reset();
        auto staging_desc = desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags = 0;
        checked(device_->CreateTexture2D(&staging_desc, nullptr, &staging_), "Create staging texture");
    }
    context_->CopyResource(staging_.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    // Map waits for CopyResource: the CEF source is no longer accessed when this callback returns.
    checked(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map copied texture");
    std::vector<std::uint8_t> pixels;
    try {
        pixels.resize(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y) {
            auto* destination = pixels.data() + static_cast<std::size_t>(y) * desc.Width * 4;
            const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
            std::memcpy(destination, row, static_cast<std::size_t>(desc.Width) * 4);
            if (rgba) for (UINT x = 0; x < desc.Width; ++x) std::swap(destination[x * 4], destination[x * 4 + 2]);
        }
    } catch (...) {
        context_->Unmap(staging_.Get(), 0);
        throw;
    }
    context_->Unmap(staging_.Get(), 0);
    target.update(static_cast<int>(desc.Width), static_cast<int>(desc.Height), pixels, {});
}
}
