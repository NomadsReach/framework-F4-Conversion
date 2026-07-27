#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/platform/GPUDriver.h>
#pragma warning(pop)

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace PrismaUI
{
    class D3D11GpuDriver final : public ultralight::GPUDriver
    {
    public:
        D3D11GpuDriver(
            ID3D11Device* device,
            ID3D11DeviceContext* context) noexcept;
        ~D3D11GpuDriver() override = default;

        void BeginSynchronize() override;
        void EndSynchronize() override;

        std::uint32_t NextTextureId() override;
        void CreateTexture(
            std::uint32_t textureId,
            ultralight::RefPtr<ultralight::Bitmap> bitmap) override;
        void UpdateTexture(
            std::uint32_t textureId,
            ultralight::RefPtr<ultralight::Bitmap> bitmap) override;
        void DestroyTexture(std::uint32_t textureId) override;

        std::uint32_t NextRenderBufferId() override;
        void CreateRenderBuffer(
            std::uint32_t renderBufferId,
            const ultralight::RenderBuffer& buffer) override;
        void DestroyRenderBuffer(
            std::uint32_t renderBufferId) override;

        std::uint32_t NextGeometryId() override;
        void CreateGeometry(
            std::uint32_t geometryId,
            const ultralight::VertexBuffer& vertices,
            const ultralight::IndexBuffer& indices) override;
        void UpdateGeometry(
            std::uint32_t geometryId,
            const ultralight::VertexBuffer& vertices,
            const ultralight::IndexBuffer& indices) override;
        void DestroyGeometry(std::uint32_t geometryId) override;

        void UpdateCommandList(
            const ultralight::CommandList& list) override;

        [[nodiscard]] bool AttachDevice(
            ID3D11Device* device,
            ID3D11DeviceContext* context) noexcept;
        [[nodiscard]] bool ExecutePending() noexcept;
        [[nodiscard]] Microsoft::WRL::ComPtr<
            ID3D11ShaderResourceView>
            GetTextureView(std::uint32_t textureId) const noexcept;
        [[nodiscard]] bool HasFatalError() const noexcept
        {
            return fatalError_.load(std::memory_order_acquire);
        }

    private:
        enum class OperationKind : std::uint8_t
        {
            CreateTexture,
            UpdateTexture,
            DestroyTexture,
            CreateRenderBuffer,
            DestroyRenderBuffer,
            CreateGeometry,
            UpdateGeometry,
            DestroyGeometry
        };

        struct Operation
        {
            OperationKind kind{};
            std::uint32_t id = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t rowBytes = 0;
            ultralight::BitmapFormat bitmapFormat =
                ultralight::BitmapFormat::BGRA8_UNORM_SRGB;
            bool emptyBitmap = false;
            ultralight::RenderBuffer renderBuffer{};
            ultralight::VertexBufferFormat vertexFormat =
                ultralight::VertexBufferFormat::_2f_4ub_2f;
            std::size_t firstOffset = 0;
            std::size_t firstSize = 0;
            std::size_t secondOffset = 0;
            std::size_t secondSize = 0;
        };

        struct Batch
        {
            std::vector<Operation> operations;
            std::vector<std::byte> bytes;
            std::vector<ultralight::Command> commands;

            void Clear() noexcept
            {
                operations.clear();
                bytes.clear();
                commands.clear();
            }
        };

        struct Texture
        {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> resource;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t renderBufferId = 0;
            std::size_t bytes = 0;
            ultralight::BitmapFormat format =
                ultralight::BitmapFormat::BGRA8_UNORM_SRGB;
            bool renderTarget = false;
            bool pendingDestroy = false;
        };

        struct RenderBuffer
        {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> multisample;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
            std::uint32_t textureId = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::size_t pixelCount = 0;
            bool dirty = false;
        };

        struct Geometry
        {
            Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
            Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
            ultralight::VertexBufferFormat format =
                ultralight::VertexBufferFormat::_2f_4ub_2f;
            std::size_t vertexCapacity = 0;
            std::size_t indexCapacity = 0;
            std::size_t vertexCount = 0;
            std::size_t indexBytes = 0;
            std::vector<std::uint32_t> invalidIndexPrefix;
        };

        struct Shader
        {
            Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex;
            Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel;
            Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
        };

        void Fail(
            const char* operation,
            const char* detail,
            HRESULT result = S_OK) noexcept;
        [[nodiscard]] bool SynchronizationThreadIsValid(
            const char* operation) noexcept;
        [[nodiscard]] bool SynchronizationCallIsValid(
            const char* operation) noexcept;
        [[nodiscard]] bool ExecutionThreadIsValid(
            const char* operation) noexcept;
        [[nodiscard]] bool RejectExecution(
            const char* stage,
            HRESULT result = S_OK) noexcept;
        [[nodiscard]] bool AppendBytes(
            const void* data,
            std::size_t size,
            std::size_t& offset) noexcept;
        [[nodiscard]] bool RecordTexture(
            OperationKind kind,
            std::uint32_t textureId,
            ultralight::RefPtr<ultralight::Bitmap> bitmap) noexcept;
        [[nodiscard]] bool RecordGeometry(
            OperationKind kind,
            std::uint32_t geometryId,
            const ultralight::VertexBuffer& vertices,
            const ultralight::IndexBuffer& indices) noexcept;

        [[nodiscard]] bool EnsurePipeline() noexcept;
        [[nodiscard]] bool ExecuteOperation(
            const Operation& operation,
            const Batch& batch) noexcept;
        [[nodiscard]] bool ExecuteCommand(
            const ultralight::Command& command) noexcept;
        [[nodiscard]] bool CreateOrUpdateGeometry(
            const Operation& operation,
            const Batch& batch,
            bool requireExisting) noexcept;
        [[nodiscard]] bool UpdateUniforms(
            const ultralight::GPUState& state) noexcept;
        [[nodiscard]] bool ResolveTexture(
            std::uint32_t textureId) noexcept;
        [[nodiscard]] bool BindTexture(
            UINT slot,
            std::uint32_t textureId) noexcept;
        [[nodiscard]] bool ResolveAll() noexcept;

        ID3D11Device* device_ = nullptr;
        ID3D11DeviceContext* context_ = nullptr;

        std::atomic<bool> fatalError_ = false;
        std::atomic<bool> failureLogged_ = false;
        const char* executionFailureStage_ = "unreported";
        HRESULT executionFailureResult_ = S_OK;
        std::thread::id synchronizationThread_{};
        bool synchronizing_ = false;

        std::uint32_t nextTextureId_ = 1;
        std::uint32_t nextRenderBufferId_ = 1;
        std::uint32_t nextGeometryId_ = 1;

        Batch staging_;
        Batch pending_;
        Batch recycled_;
        mutable std::mutex batchMutex_;
        bool pendingReady_ = false;
        bool recycledReady_ = false;

        std::unordered_map<std::uint32_t, Texture> textures_;
        std::unordered_map<std::uint32_t, RenderBuffer> renderBuffers_;
        std::unordered_map<std::uint32_t, Geometry> geometries_;
        std::size_t liveTextureBytes_ = 0;
        std::size_t liveRenderTargetPixels_ = 0;
        std::size_t liveGeometryBytes_ = 0;

        Shader fillShader_;
        Shader pathShader_;
        Microsoft::WRL::ComPtr<ID3D11Buffer> uniformBuffer_;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
        Microsoft::WRL::ComPtr<ID3D11BlendState> overwrite_;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthDisabled_;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> scissorRaster_;
        bool pipelineReady_ = false;
    };
}
