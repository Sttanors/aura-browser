#pragma once
#ifdef _WIN32
#include "common/paint_buffer.hpp"
#include <d3d11_1.h>
#include <wrl/client.h>
namespace aura {
// M0 probe deliberately reads back a completed copy. Production GPU-only composition is M2.
class SharedTextureProbe {
public:
    void copy_and_read(HANDLE handle, PaintBuffer& target);
private:
    Microsoft::WRL::ComPtr<ID3D11Device1> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
};
}
#endif
