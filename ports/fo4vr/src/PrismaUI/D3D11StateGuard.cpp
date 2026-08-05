#include "PCH.h"

#include "PrismaUI/D3D11StateGuard.h"

namespace PrismaUI
{
    namespace
    {
        template <std::size_t Size>
        void ReleaseInstances(
            std::array<ID3D11ClassInstance*, Size>& values,
            UINT count) noexcept
        {
            const auto bounded = (std::min)(
                static_cast<std::size_t>(count),
                values.size());
            for (std::size_t index = 0;
                 index < bounded;
                 ++index) {
                if (values[index]) {
                    values[index]->Release();
                    values[index] = nullptr;
                }
            }
        }
    }

    ScopedD3D11State::ScopedD3D11State(
        ID3D11DeviceContext* context) noexcept :
        context_(context)
    {
        if (!context_) {
            return;
        }

        context_->OMGetRenderTargets(
            static_cast<UINT>(renderTargets_.size()),
            renderTargets_.data(),
            &depthStencil_);

        context_->IAGetInputLayout(
            inputLayout_.GetAddressOf());
        context_->IAGetPrimitiveTopology(&topology_);
        context_->IAGetVertexBuffers(
            0,
            1,
            vertexBuffer_.GetAddressOf(),
            &vertexStride_,
            &vertexOffset_);
        context_->IAGetIndexBuffer(
            indexBuffer_.GetAddressOf(),
            &indexFormat_,
            &indexOffset_);

        vertexClassCount_ =
            static_cast<UINT>(vertexClasses_.size());
        context_->VSGetShader(
            vertexShader_.GetAddressOf(),
            vertexClasses_.data(),
            &vertexClassCount_);
        pixelClassCount_ =
            static_cast<UINT>(pixelClasses_.size());
        context_->PSGetShader(
            pixelShader_.GetAddressOf(),
            pixelClasses_.data(),
            &pixelClassCount_);
        geometryClassCount_ =
            static_cast<UINT>(geometryClasses_.size());
        context_->GSGetShader(
            geometryShader_.GetAddressOf(),
            geometryClasses_.data(),
            &geometryClassCount_);
        hullClassCount_ =
            static_cast<UINT>(hullClasses_.size());
        context_->HSGetShader(
            hullShader_.GetAddressOf(),
            hullClasses_.data(),
            &hullClassCount_);
        domainClassCount_ =
            static_cast<UINT>(domainClasses_.size());
        context_->DSGetShader(
            domainShader_.GetAddressOf(),
            domainClasses_.data(),
            &domainClassCount_);

        context_->VSGetConstantBuffers(
            0,
            1,
            vertexConstantBuffer_.GetAddressOf());
        context_->PSGetConstantBuffers(
            0,
            1,
            pixelConstantBuffer_.GetAddressOf());
        context_->PSGetShaderResources(
            0,
            kShaderResourceSlots,
            pixelResources_.data());
        context_->PSGetSamplers(
            0,
            1,
            pixelSampler_.GetAddressOf());

        context_->OMGetBlendState(
            blendState_.GetAddressOf(),
            blendFactor_,
            &sampleMask_);
        context_->OMGetDepthStencilState(
            depthState_.GetAddressOf(),
            &stencilReference_);
        context_->RSGetState(rasterState_.GetAddressOf());

        viewportCount_ =
            static_cast<UINT>(viewports_.size());
        context_->RSGetViewports(
            &viewportCount_,
            viewports_.data());
        scissorCount_ =
            static_cast<UINT>(scissors_.size());
        context_->RSGetScissorRects(
            &scissorCount_,
            scissors_.data());
        context_->GetPredication(
            predicate_.GetAddressOf(),
            &predicateValue_);
    }

    ScopedD3D11State::~ScopedD3D11State() noexcept
    {
        if (!context_) {
            return;
        }

        context_->OMSetRenderTargets(
            static_cast<UINT>(renderTargets_.size()),
            renderTargets_.data(),
            depthStencil_);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(topology_);
        auto* vertexBuffer = vertexBuffer_.Get();
        context_->IASetVertexBuffers(
            0,
            1,
            &vertexBuffer,
            &vertexStride_,
            &vertexOffset_);
        context_->IASetIndexBuffer(
            indexBuffer_.Get(),
            indexFormat_,
            indexOffset_);

        context_->VSSetShader(
            vertexShader_.Get(),
            vertexClasses_.data(),
            vertexClassCount_);
        context_->PSSetShader(
            pixelShader_.Get(),
            pixelClasses_.data(),
            pixelClassCount_);
        context_->GSSetShader(
            geometryShader_.Get(),
            geometryClasses_.data(),
            geometryClassCount_);
        context_->HSSetShader(
            hullShader_.Get(),
            hullClasses_.data(),
            hullClassCount_);
        context_->DSSetShader(
            domainShader_.Get(),
            domainClasses_.data(),
            domainClassCount_);

        auto* vertexConstant =
            vertexConstantBuffer_.Get();
        context_->VSSetConstantBuffers(
            0,
            1,
            &vertexConstant);
        auto* pixelConstant =
            pixelConstantBuffer_.Get();
        context_->PSSetConstantBuffers(
            0,
            1,
            &pixelConstant);
        context_->PSSetShaderResources(
            0,
            kShaderResourceSlots,
            pixelResources_.data());
        auto* sampler = pixelSampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);

        context_->OMSetBlendState(
            blendState_.Get(),
            blendFactor_,
            sampleMask_);
        context_->OMSetDepthStencilState(
            depthState_.Get(),
            stencilReference_);
        context_->RSSetState(rasterState_.Get());
        context_->RSSetViewports(
            viewportCount_,
            viewports_.data());
        context_->RSSetScissorRects(
            scissorCount_,
            scissors_.data());
        context_->SetPredication(
            predicate_.Get(),
            predicateValue_);

        for (auto*& renderTarget : renderTargets_) {
            if (renderTarget) {
                renderTarget->Release();
                renderTarget = nullptr;
            }
        }
        if (depthStencil_) {
            depthStencil_->Release();
            depthStencil_ = nullptr;
        }
        for (auto*& resource : pixelResources_) {
            if (resource) {
                resource->Release();
                resource = nullptr;
            }
        }
        ReleaseInstances(
            vertexClasses_,
            vertexClassCount_);
        ReleaseInstances(
            pixelClasses_,
            pixelClassCount_);
        ReleaseInstances(
            geometryClasses_,
            geometryClassCount_);
        ReleaseInstances(
            hullClasses_,
            hullClassCount_);
        ReleaseInstances(
            domainClasses_,
            domainClassCount_);
    }
}
