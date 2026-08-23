#include "D3D11StateGuard.h"

namespace PrismaUI {

D3D11StateGuard::D3D11StateGuard(ID3D11DeviceContext* context) : context_(context)
{
    if (!context_) return;

    context_->OMGetBlendState(blendState_.GetAddressOf(), blendFactor_, &sampleMask_);
    context_->OMGetDepthStencilState(depthStencilState_.GetAddressOf(), &stencilRef_);
    context_->RSGetState(rasterizerState_.GetAddressOf());
    context_->RSGetViewports(&viewportCount_, viewports_.data());
    context_->RSGetScissorRects(&scissorCount_, scissors_.data());

    context_->IAGetInputLayout(inputLayout_.GetAddressOf());
    context_->IAGetPrimitiveTopology(&topology_);
    context_->IAGetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &vertexStride_, &vertexOffset_);
    context_->IAGetIndexBuffer(indexBuffer_.GetAddressOf(), &indexFormat_, &indexOffset_);

    context_->VSGetShader(vertexShader_.GetAddressOf(), nullptr, nullptr);
    context_->PSGetShader(pixelShader_.GetAddressOf(), nullptr, nullptr);
    context_->GSGetShader(geometryShader_.GetAddressOf(), nullptr, nullptr);
    context_->HSGetShader(hullShader_.GetAddressOf(), nullptr, nullptr);
    context_->DSGetShader(domainShader_.GetAddressOf(), nullptr, nullptr);
    context_->VSGetConstantBuffers(0, 1, vertexConstantBuffer_.GetAddressOf());
    context_->PSGetConstantBuffers(0, 1, pixelConstantBuffer_.GetAddressOf());
    context_->PSGetShaderResources(0, 1, pixelSrv_.GetAddressOf());
    context_->PSGetSamplers(0, 1, pixelSampler_.GetAddressOf());

    captured_ = true;
}

D3D11StateGuard::~D3D11StateGuard()
{
    if (!context_ || !captured_) return;

    context_->OMSetBlendState(blendState_.Get(), blendFactor_, sampleMask_);
    context_->OMSetDepthStencilState(depthStencilState_.Get(), stencilRef_);
    context_->RSSetState(rasterizerState_.Get());
    context_->RSSetViewports(viewportCount_, viewportCount_ ? viewports_.data() : nullptr);
    context_->RSSetScissorRects(scissorCount_, scissorCount_ ? scissors_.data() : nullptr);

    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetPrimitiveTopology(topology_);
    ID3D11Buffer* vertexBuffer = vertexBuffer_.Get();
    context_->IASetVertexBuffers(0, 1, &vertexBuffer, &vertexStride_, &vertexOffset_);
    context_->IASetIndexBuffer(indexBuffer_.Get(), indexFormat_, indexOffset_);

    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->GSSetShader(geometryShader_.Get(), nullptr, 0);
    context_->HSSetShader(hullShader_.Get(), nullptr, 0);
    context_->DSSetShader(domainShader_.Get(), nullptr, 0);

    ID3D11Buffer* vertexConstantBuffer = vertexConstantBuffer_.Get();
    ID3D11Buffer* pixelConstantBuffer = pixelConstantBuffer_.Get();
    ID3D11ShaderResourceView* pixelSrv = pixelSrv_.Get();
    ID3D11SamplerState* pixelSampler = pixelSampler_.Get();
    context_->VSSetConstantBuffers(0, 1, &vertexConstantBuffer);
    context_->PSSetConstantBuffers(0, 1, &pixelConstantBuffer);
    context_->PSSetShaderResources(0, 1, &pixelSrv);
    context_->PSSetSamplers(0, 1, &pixelSampler);
}

}
