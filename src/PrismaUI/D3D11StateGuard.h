#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>

namespace PrismaUI {

class D3D11StateGuard {
public:
    explicit D3D11StateGuard(ID3D11DeviceContext* context);
    ~D3D11StateGuard();

    D3D11StateGuard(const D3D11StateGuard&) = delete;
    D3D11StateGuard& operator=(const D3D11StateGuard&) = delete;

private:
    ID3D11DeviceContext* context_ = nullptr;
    bool captured_ = false;

    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;
    FLOAT blendFactor_[4]{};
    UINT sampleMask_ = 0;

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState_;
    UINT stencilRef_ = 0;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;

    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_;
    UINT vertexStride_ = 0;
    UINT vertexOffset_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer_;
    DXGI_FORMAT indexFormat_ = DXGI_FORMAT_UNKNOWN;
    UINT indexOffset_ = 0;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> geometryShader_;
    Microsoft::WRL::ComPtr<ID3D11HullShader> hullShader_;
    Microsoft::WRL::ComPtr<ID3D11DomainShader> domainShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> pixelConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pixelSrv_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pixelSampler_;

    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports_{};
    UINT viewportCount_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors_{};
    UINT scissorCount_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
};

}
