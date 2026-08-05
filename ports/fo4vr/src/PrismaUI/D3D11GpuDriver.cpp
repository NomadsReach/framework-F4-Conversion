#include "PCH.h"

#include "PrismaUI/D3D11GpuDriver.h"
#include "PrismaUI/GpuGeometryIndexPolicy.h"
#include "PrismaUI/GpuResourceBudget.h"

#include <Ultralight/Matrix.h>

#include "fill_fxc.h"
#include "fill_path_fxc.h"
#include "v2f_c4f_t2f_fxc.h"
#include "v2f_c4f_t2f_t2f_d28f_fxc.h"

namespace PrismaUI
{
    namespace
    {
        constexpr UINT kMsaaSampleCount = 8;
        constexpr std::uint32_t kMaximumDimension = 4096;
        constexpr std::size_t kMaximumBatchBytes =
            128ull * 1024ull * 1024ull;
        constexpr std::size_t kMaximumOperations = 65536;
        constexpr std::size_t kMaximumCommands = 65536;
        constexpr std::size_t kMaximumTextures = 4096;
        constexpr std::size_t kMaximumRenderBuffers = 256;
        constexpr std::size_t kMaximumGeometries = 65536;
        constexpr std::size_t kMaximumTextureBytes =
            768ull * 1024ull * 1024ull;
        constexpr std::size_t kMaximumGeometryBytes =
            256ull * 1024ull * 1024ull;

        struct alignas(16) ShaderUniforms
        {
            float state[4]{};
            float transform[16]{};
            float scalars[2][4]{};
            float vectors[8][4]{};
            std::uint32_t clipSize = 0;
            std::uint32_t padding[3]{};
            float clips[8][16]{};
        };

        static_assert(sizeof(ShaderUniforms) == 768);
        static_assert(
            sizeof(ultralight::Vertex_2f_4ub_2f) == 20);
        static_assert(
            sizeof(ultralight::Vertex_2f_4ub_2f_2f_28f) == 140);
        static_assert(
            sizeof(ultralight::IndexType) ==
            sizeof(std::uint32_t));

        [[nodiscard]] constexpr bool SupportedBitmapFormat(
            ultralight::BitmapFormat format) noexcept
        {
            return format ==
                       ultralight::BitmapFormat::A8_UNORM ||
                   format ==
                       ultralight::BitmapFormat::
                           BGRA8_UNORM_SRGB;
        }

        [[nodiscard]] constexpr DXGI_FORMAT DxgiFormat(
            ultralight::BitmapFormat format) noexcept
        {
            return format ==
                       ultralight::BitmapFormat::A8_UNORM ?
                DXGI_FORMAT_A8_UNORM :
                DXGI_FORMAT_B8G8R8A8_UNORM;
        }

        [[nodiscard]] constexpr std::size_t BytesPerPixel(
            ultralight::BitmapFormat format) noexcept
        {
            return format ==
                       ultralight::BitmapFormat::A8_UNORM ?
                1u :
                4u;
        }

        [[nodiscard]] constexpr std::size_t VertexStride(
            ultralight::VertexBufferFormat format) noexcept
        {
            return format ==
                       ultralight::VertexBufferFormat::
                           _2f_4ub_2f ?
                sizeof(ultralight::Vertex_2f_4ub_2f) :
                sizeof(
                    ultralight::
                        Vertex_2f_4ub_2f_2f_28f);
        }

        [[nodiscard]] bool SupportedVertexFormat(
            ultralight::VertexBufferFormat format) noexcept
        {
            return format ==
                       ultralight::VertexBufferFormat::
                           _2f_4ub_2f ||
                   format ==
                       ultralight::VertexBufferFormat::
                           _2f_4ub_2f_2f_28f;
        }

        [[nodiscard]] bool CheckedProduct(
            std::size_t left,
            std::size_t right,
            std::size_t& result) noexcept
        {
            if (left != 0 &&
                right >
                    (std::numeric_limits<std::size_t>::max)() /
                        left) {
                return false;
            }
            result = left * right;
            return true;
        }

        [[nodiscard]] bool IsFinite(
            const ultralight::GPUState& state) noexcept
        {
            for (const auto value : state.transform.data) {
                if (!std::isfinite(value)) {
                    return false;
                }
            }
            for (const auto value : state.uniform_scalar) {
                if (!std::isfinite(value)) {
                    return false;
                }
            }
            for (const auto& vector : state.uniform_vector) {
                if (!std::isfinite(vector.x) ||
                    !std::isfinite(vector.y) ||
                    !std::isfinite(vector.z) ||
                    !std::isfinite(vector.w)) {
                    return false;
                }
            }
            if (state.clip_size > 8) {
                return false;
            }
            for (std::uint8_t index = 0;
                 index < state.clip_size;
                 ++index) {
                for (const auto value : state.clip[index].data) {
                    if (!std::isfinite(value)) {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    D3D11GpuDriver::D3D11GpuDriver(
        ID3D11Device* device,
        ID3D11DeviceContext* context) noexcept :
        device_(device),
        context_(context)
    {}

    void D3D11GpuDriver::Fail(
        const char* operation,
        const char* detail,
        HRESULT result) noexcept
    {
        fatalError_.store(true, std::memory_order_release);
        if (!failureLogged_.exchange(
                true,
                std::memory_order_acq_rel)) {
            if (FAILED(result)) {
                logger::critical(
                    "PrismaUI GPU driver {} failed: {} (HRESULT 0x{:08X})",
                    operation,
                    detail,
                    static_cast<std::uint32_t>(result));
            } else {
                logger::critical(
                    "PrismaUI GPU driver {} failed: {}",
                    operation,
                    detail);
            }
        }
    }

    bool D3D11GpuDriver::SynchronizationThreadIsValid(
        const char* operation) noexcept
    {
        if (fatalError_.load(std::memory_order_acquire)) {
            return false;
        }

        const auto currentThread = std::this_thread::get_id();
        bool wrongThread = false;
        {
            std::lock_guard lock(batchMutex_);
            if (synchronizationThread_ == std::thread::id{}) {
                synchronizationThread_ = currentThread;
            } else {
                wrongThread =
                    synchronizationThread_ != currentThread;
            }
        }
        if (wrongThread) {
            Fail(
                operation,
                "callback occurred on a thread other than the Ultralight worker");
            return false;
        }
        return true;
    }

    bool D3D11GpuDriver::SynchronizationCallIsValid(
        const char* operation) noexcept
    {
        if (!SynchronizationThreadIsValid(operation)) {
            return false;
        }
        if (!synchronizing_) {
            Fail(
                operation,
                "callback occurred outside its synchronization window");
            return false;
        }
        return true;
    }

    bool D3D11GpuDriver::ExecutionThreadIsValid(
        const char* operation) noexcept
    {
        const auto currentThread = std::this_thread::get_id();
        bool synchronizationThread = false;
        {
            std::lock_guard lock(batchMutex_);
            synchronizationThread =
                synchronizationThread_ != std::thread::id{} &&
                synchronizationThread_ == currentThread;
        }
        if (synchronizationThread) {
            Fail(
                operation,
                "D3D11 execution attempted on the Ultralight worker");
            return false;
        }
        return true;
    }

    bool D3D11GpuDriver::RejectExecution(
        const char* stage,
        HRESULT result) noexcept
    {
        executionFailureStage_ =
            stage ? stage : "unreported";
        executionFailureResult_ = result;
        return false;
    }

    void D3D11GpuDriver::BeginSynchronize()
    {
        if (fatalError_.load(std::memory_order_acquire) ||
            !SynchronizationThreadIsValid(
                "BeginSynchronize")) {
            return;
        }
        if (synchronizing_) {
            Fail(
                "BeginSynchronize",
                "nested synchronization is not supported");
            return;
        }

        {
            std::lock_guard lock(batchMutex_);
            if (recycledReady_) {
                std::swap(staging_, recycled_);
                recycledReady_ = false;
            }
        }
        staging_.Clear();
        synchronizing_ = true;
    }

    void D3D11GpuDriver::EndSynchronize()
    {
        if (!SynchronizationCallIsValid(
                "EndSynchronize")) {
            synchronizing_ = false;
            return;
        }
        synchronizing_ = false;

        std::lock_guard lock(batchMutex_);
        if (pendingReady_) {
            Fail(
                "EndSynchronize",
                "the prior command batch was not consumed");
            return;
        }
        std::swap(staging_, pending_);
        pendingReady_ = true;
    }

    std::uint32_t D3D11GpuDriver::NextTextureId()
    {
        if (fatalError_.load(std::memory_order_acquire) ||
            !SynchronizationThreadIsValid("NextTextureId")) {
            return 0;
        }
        if (nextTextureId_ == 0 ||
            nextTextureId_ ==
                (std::numeric_limits<std::uint32_t>::max)()) {
            Fail(
                "NextTextureId",
                "texture ID space was exhausted");
            return 0;
        }
        return nextTextureId_++;
    }

    std::uint32_t D3D11GpuDriver::NextRenderBufferId()
    {
        if (fatalError_.load(std::memory_order_acquire) ||
            !SynchronizationThreadIsValid(
                "NextRenderBufferId")) {
            return 0;
        }
        if (nextRenderBufferId_ == 0 ||
            nextRenderBufferId_ ==
                (std::numeric_limits<std::uint32_t>::max)()) {
            Fail(
                "NextRenderBufferId",
                "render-buffer ID space was exhausted");
            return 0;
        }
        return nextRenderBufferId_++;
    }

    std::uint32_t D3D11GpuDriver::NextGeometryId()
    {
        if (fatalError_.load(std::memory_order_acquire) ||
            !SynchronizationThreadIsValid("NextGeometryId")) {
            return 0;
        }
        if (nextGeometryId_ == 0 ||
            nextGeometryId_ ==
                (std::numeric_limits<std::uint32_t>::max)()) {
            Fail(
                "NextGeometryId",
                "geometry ID space was exhausted");
            return 0;
        }
        return nextGeometryId_++;
    }

    bool D3D11GpuDriver::AppendBytes(
        const void* data,
        std::size_t size,
        std::size_t& offset) noexcept
    {
        if (size == 0) {
            offset = staging_.bytes.size();
            return true;
        }
        if (!data ||
            size > kMaximumBatchBytes ||
            staging_.bytes.size() >
                kMaximumBatchBytes - size) {
            return false;
        }
        try {
            offset = staging_.bytes.size();
            const auto source =
                static_cast<const std::byte*>(data);
            staging_.bytes.insert(
                staging_.bytes.end(),
                source,
                source + size);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool D3D11GpuDriver::RecordTexture(
        OperationKind kind,
        std::uint32_t textureId,
        ultralight::RefPtr<ultralight::Bitmap> bitmap) noexcept
    {
        if (!SynchronizationCallIsValid("Texture") ||
            textureId == 0 ||
            !bitmap ||
            staging_.operations.size() >=
                kMaximumOperations) {
            return false;
        }

        Operation operation;
        operation.kind = kind;
        operation.id = textureId;
        operation.width = bitmap->width();
        operation.height = bitmap->height();
        operation.rowBytes = bitmap->row_bytes();
        operation.bitmapFormat = bitmap->format();
        operation.emptyBitmap = bitmap->IsEmpty();

        if (operation.width == 0 ||
            operation.height == 0 ||
            operation.width > kMaximumDimension ||
            operation.height > kMaximumDimension ||
            !SupportedBitmapFormat(operation.bitmapFormat)) {
            return false;
        }

        if (!operation.emptyBitmap) {
            std::size_t expected = 0;
            if (operation.rowBytes <
                    operation.width *
                        BytesPerPixel(operation.bitmapFormat) ||
                !CheckedProduct(
                    operation.rowBytes,
                    operation.height,
                    expected) ||
                expected != bitmap->size()) {
                return false;
            }

            auto pixels = bitmap->LockPixelsSafe();
            if (!pixels.data() ||
                pixels.size() < expected ||
                !AppendBytes(
                    pixels.data(),
                    expected,
                    operation.firstOffset)) {
                return false;
            }
            operation.firstSize = expected;
        }

        try {
            staging_.operations.push_back(operation);
            return true;
        } catch (...) {
            return false;
        }
    }

    void D3D11GpuDriver::CreateTexture(
        std::uint32_t textureId,
        ultralight::RefPtr<ultralight::Bitmap> bitmap)
    {
        if (!RecordTexture(
                OperationKind::CreateTexture,
                textureId,
                std::move(bitmap))) {
            Fail(
                "CreateTexture",
                "could not record texture creation");
        }
    }

    void D3D11GpuDriver::UpdateTexture(
        std::uint32_t textureId,
        ultralight::RefPtr<ultralight::Bitmap> bitmap)
    {
        if (!RecordTexture(
                OperationKind::UpdateTexture,
                textureId,
                std::move(bitmap))) {
            Fail(
                "UpdateTexture",
                "could not record texture update");
        }
    }

    void D3D11GpuDriver::DestroyTexture(
        std::uint32_t textureId)
    {
        if (!SynchronizationCallIsValid("DestroyTexture") ||
            textureId == 0 ||
            staging_.operations.size() >=
                kMaximumOperations) {
            Fail(
                "DestroyTexture",
                "invalid texture destruction");
            return;
        }
        try {
            staging_.operations.push_back({
                .kind = OperationKind::DestroyTexture,
                .id = textureId
            });
        } catch (...) {
            Fail(
                "DestroyTexture",
                "could not record texture destruction");
        }
    }

    void D3D11GpuDriver::CreateRenderBuffer(
        std::uint32_t renderBufferId,
        const ultralight::RenderBuffer& buffer)
    {
        if (!SynchronizationCallIsValid(
                "CreateRenderBuffer") ||
            renderBufferId == 0 ||
            buffer.texture_id == 0 ||
            buffer.width == 0 ||
            buffer.height == 0 ||
            buffer.width > kMaximumDimension ||
            buffer.height > kMaximumDimension ||
            buffer.has_depth_buffer ||
            buffer.has_stencil_buffer ||
            staging_.operations.size() >=
                kMaximumOperations) {
            Fail(
                "CreateRenderBuffer",
                "invalid render-buffer description");
            return;
        }
        try {
            staging_.operations.push_back({
                .kind = OperationKind::CreateRenderBuffer,
                .id = renderBufferId,
                .renderBuffer = buffer
            });
        } catch (...) {
            Fail(
                "CreateRenderBuffer",
                "could not record render-buffer creation");
        }
    }

    void D3D11GpuDriver::DestroyRenderBuffer(
        std::uint32_t renderBufferId)
    {
        if (!SynchronizationCallIsValid(
                "DestroyRenderBuffer") ||
            renderBufferId == 0 ||
            staging_.operations.size() >=
                kMaximumOperations) {
            Fail(
                "DestroyRenderBuffer",
                "invalid render-buffer destruction");
            return;
        }
        try {
            staging_.operations.push_back({
                .kind = OperationKind::DestroyRenderBuffer,
                .id = renderBufferId
            });
        } catch (...) {
            Fail(
                "DestroyRenderBuffer",
                "could not record render-buffer destruction");
        }
    }

    bool D3D11GpuDriver::RecordGeometry(
        OperationKind kind,
        std::uint32_t geometryId,
        const ultralight::VertexBuffer& vertices,
        const ultralight::IndexBuffer& indices) noexcept
    {
        if (!SynchronizationCallIsValid("Geometry") ||
            geometryId == 0 ||
            !SupportedVertexFormat(vertices.format) ||
            vertices.size == 0 ||
            indices.size == 0 ||
            !vertices.data ||
            !indices.data ||
            vertices.size % VertexStride(vertices.format) != 0 ||
            indices.size % sizeof(ultralight::IndexType) != 0 ||
            staging_.operations.size() >=
                kMaximumOperations) {
            return false;
        }

        Operation operation;
        operation.kind = kind;
        operation.id = geometryId;
        operation.vertexFormat = vertices.format;
        operation.firstSize = vertices.size;
        operation.secondSize = indices.size;
        if (!AppendBytes(
                vertices.data,
                vertices.size,
                operation.firstOffset) ||
            !AppendBytes(
                indices.data,
                indices.size,
                operation.secondOffset)) {
            return false;
        }
        try {
            staging_.operations.push_back(operation);
            return true;
        } catch (...) {
            return false;
        }
    }

    void D3D11GpuDriver::CreateGeometry(
        std::uint32_t geometryId,
        const ultralight::VertexBuffer& vertices,
        const ultralight::IndexBuffer& indices)
    {
        if (!RecordGeometry(
                OperationKind::CreateGeometry,
                geometryId,
                vertices,
                indices)) {
            Fail(
                "CreateGeometry",
                "could not record geometry creation");
        }
    }

    void D3D11GpuDriver::UpdateGeometry(
        std::uint32_t geometryId,
        const ultralight::VertexBuffer& vertices,
        const ultralight::IndexBuffer& indices)
    {
        if (!RecordGeometry(
                OperationKind::UpdateGeometry,
                geometryId,
                vertices,
                indices)) {
            Fail(
                "UpdateGeometry",
                "could not record geometry update");
        }
    }

    void D3D11GpuDriver::DestroyGeometry(
        std::uint32_t geometryId)
    {
        if (!SynchronizationCallIsValid("DestroyGeometry") ||
            geometryId == 0 ||
            staging_.operations.size() >=
                kMaximumOperations) {
            Fail(
                "DestroyGeometry",
                "invalid geometry destruction");
            return;
        }
        try {
            staging_.operations.push_back({
                .kind = OperationKind::DestroyGeometry,
                .id = geometryId
            });
        } catch (...) {
            Fail(
                "DestroyGeometry",
                "could not record geometry destruction");
        }
    }

    void D3D11GpuDriver::UpdateCommandList(
        const ultralight::CommandList& list)
    {
        if (!SynchronizationCallIsValid(
                "UpdateCommandList") ||
            list.size > kMaximumCommands ||
            (list.size > 0 && !list.commands)) {
            Fail(
                "UpdateCommandList",
                "invalid command list");
            return;
        }
        try {
            if (list.size == 0) {
                staging_.commands.clear();
            } else {
                staging_.commands.assign(
                    list.commands,
                    list.commands + list.size);
            }
        } catch (...) {
            Fail(
                "UpdateCommandList",
                "could not copy command list");
        }
    }

    bool D3D11GpuDriver::AttachDevice(
        ID3D11Device* device,
        ID3D11DeviceContext* context) noexcept
    {
        if (!device ||
            !context ||
            fatalError_.load(std::memory_order_acquire) ||
            !ExecutionThreadIsValid("AttachDevice")) {
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> contextDevice;
        context->GetDevice(contextDevice.GetAddressOf());
        if (contextDevice.Get() != device) {
            Fail(
                "AttachDevice",
                "device and immediate context do not match");
            return false;
        }
        if (device_ == device && context_ == context) {
            return true;
        }
        if ((device_ && device_ != device) ||
            (context_ && context_ != context)) {
            Fail(
                "AttachDevice",
                "device replacement requires coordinated driver recreation");
            return false;
        }

        device_ = device;
        context_ = context;
        return true;
    }

    bool D3D11GpuDriver::EnsurePipeline() noexcept
    {
        if (pipelineReady_) {
            return true;
        }
        if (!device_ || !context_) {
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> contextDevice;
        context_->GetDevice(contextDevice.GetAddressOf());
        if (contextDevice.Get() != device_) {
            Fail(
                "EnsurePipeline",
                "device and immediate context do not match");
            return false;
        }

        constexpr D3D11_INPUT_ELEMENT_DESC pathLayout[]{
            {
                "POSITION",
                0,
                DXGI_FORMAT_R32G32_FLOAT,
                0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA,
                0
            },
            {
                "COLOR",
                0,
                DXGI_FORMAT_R8G8B8A8_UINT,
                0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA,
                0
            },
            {
                "TEXCOORD",
                0,
                DXGI_FORMAT_R32G32_FLOAT,
                0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA,
                0
            }
        };
        constexpr D3D11_INPUT_ELEMENT_DESC fillLayout[]{
            {
                "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "COLOR", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "COLOR", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "COLOR", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "COLOR", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "COLOR", 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA, 0
            },
            {
                "COLOR", 7, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                D3D11_APPEND_ALIGNED_ELEMENT,
                D3D11_INPUT_PER_VERTEX_DATA, 0
            }
        };

        auto result = device_->CreateVertexShader(
            v2f_c4f_t2f_t2f_d28f_fxc,
            v2f_c4f_t2f_t2f_d28f_fxc_len,
            nullptr,
            fillShader_.vertex.GetAddressOf());
        if (SUCCEEDED(result)) {
            result = device_->CreatePixelShader(
                fill_fxc,
                fill_fxc_len,
                nullptr,
                fillShader_.pixel.GetAddressOf());
        }
        if (SUCCEEDED(result)) {
            result = device_->CreateInputLayout(
                fillLayout,
                static_cast<UINT>(std::size(fillLayout)),
                v2f_c4f_t2f_t2f_d28f_fxc,
                v2f_c4f_t2f_t2f_d28f_fxc_len,
                fillShader_.layout.GetAddressOf());
        }
        if (SUCCEEDED(result)) {
            result = device_->CreateVertexShader(
                v2f_c4f_t2f_fxc,
                v2f_c4f_t2f_fxc_len,
                nullptr,
                pathShader_.vertex.GetAddressOf());
        }
        if (SUCCEEDED(result)) {
            result = device_->CreatePixelShader(
                fill_path_fxc,
                fill_path_fxc_len,
                nullptr,
                pathShader_.pixel.GetAddressOf());
        }
        if (SUCCEEDED(result)) {
            result = device_->CreateInputLayout(
                pathLayout,
                static_cast<UINT>(std::size(pathLayout)),
                v2f_c4f_t2f_fxc,
                v2f_c4f_t2f_fxc_len,
                pathShader_.layout.GetAddressOf());
        }

        D3D11_BUFFER_DESC uniformDescription{};
        uniformDescription.ByteWidth = sizeof(ShaderUniforms);
        uniformDescription.Usage = D3D11_USAGE_DYNAMIC;
        uniformDescription.BindFlags =
            D3D11_BIND_CONSTANT_BUFFER;
        uniformDescription.CPUAccessFlags =
            D3D11_CPU_ACCESS_WRITE;
        if (SUCCEEDED(result)) {
            result = device_->CreateBuffer(
                &uniformDescription,
                nullptr,
                uniformBuffer_.GetAddressOf());
        }

        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.Filter =
            D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDescription.AddressU =
            D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressV =
            D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressW =
            D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.ComparisonFunc =
            D3D11_COMPARISON_NEVER;
        samplerDescription.MinLOD = 0.0f;
        samplerDescription.MaxLOD = 0.0f;
        if (SUCCEEDED(result)) {
            result = device_->CreateSamplerState(
                &samplerDescription,
                sampler_.GetAddressOf());
        }

        D3D11_BLEND_DESC blendDescription{};
        blendDescription.RenderTarget[0].BlendEnable = TRUE;
        blendDescription.RenderTarget[0].SrcBlend =
            D3D11_BLEND_ONE;
        blendDescription.RenderTarget[0].DestBlend =
            D3D11_BLEND_INV_SRC_ALPHA;
        blendDescription.RenderTarget[0].BlendOp =
            D3D11_BLEND_OP_ADD;
        blendDescription.RenderTarget[0].SrcBlendAlpha =
            D3D11_BLEND_ONE;
        blendDescription.RenderTarget[0].DestBlendAlpha =
            D3D11_BLEND_INV_SRC_ALPHA;
        blendDescription.RenderTarget[0].BlendOpAlpha =
            D3D11_BLEND_OP_ADD;
        blendDescription.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(result)) {
            result = device_->CreateBlendState(
                &blendDescription,
                blend_.GetAddressOf());
        }
        blendDescription.RenderTarget[0].BlendEnable = FALSE;
        if (SUCCEEDED(result)) {
            result = device_->CreateBlendState(
                &blendDescription,
                overwrite_.GetAddressOf());
        }

        D3D11_DEPTH_STENCIL_DESC depthDescription{};
        depthDescription.DepthEnable = FALSE;
        depthDescription.StencilEnable = FALSE;
        if (SUCCEEDED(result)) {
            result = device_->CreateDepthStencilState(
                &depthDescription,
                depthDisabled_.GetAddressOf());
        }

        D3D11_RASTERIZER_DESC rasterDescription{};
        rasterDescription.FillMode = D3D11_FILL_SOLID;
        rasterDescription.CullMode = D3D11_CULL_NONE;
        rasterDescription.DepthClipEnable = TRUE;
        rasterDescription.MultisampleEnable = TRUE;
        rasterDescription.ScissorEnable = FALSE;
        if (SUCCEEDED(result)) {
            result = device_->CreateRasterizerState(
                &rasterDescription,
                raster_.GetAddressOf());
        }
        rasterDescription.ScissorEnable = TRUE;
        if (SUCCEEDED(result)) {
            result = device_->CreateRasterizerState(
                &rasterDescription,
                scissorRaster_.GetAddressOf());
        }

        if (FAILED(result)) {
            Fail(
                "EnsurePipeline",
                "D3D11 pipeline creation failed",
                result);
            return false;
        }
        pipelineReady_ = true;
        return true;
    }

    bool D3D11GpuDriver::CreateOrUpdateGeometry(
        const Operation& operation,
        const Batch& batch,
        bool requireExisting) noexcept
    {
        if (operation.firstOffset >
                batch.bytes.size() ||
            operation.firstSize >
                batch.bytes.size() -
                    operation.firstOffset ||
            operation.secondOffset >
                batch.bytes.size() ||
            operation.secondSize >
                batch.bytes.size() -
                    operation.secondOffset ||
            !SupportedVertexFormat(
                operation.vertexFormat)) {
            return RejectExecution(
                "geometry payload range or vertex format");
        }

        const auto stride =
            VertexStride(operation.vertexFormat);
        if (operation.firstSize == 0 ||
            operation.firstSize % stride != 0 ||
            operation.secondSize == 0 ||
            operation.secondSize %
                    sizeof(ultralight::IndexType) !=
                0 ||
            operation.firstSize >
                (std::numeric_limits<UINT>::max)() ||
            operation.secondSize >
                (std::numeric_limits<UINT>::max)()) {
            return RejectExecution(
                "geometry buffer size or alignment");
        }

        const auto vertexCount =
            operation.firstSize / stride;
        const auto indexCount =
            operation.secondSize /
            sizeof(ultralight::IndexType);
        const auto* vertexData =
            batch.bytes.data() + operation.firstOffset;
        const auto* indexData =
            batch.bytes.data() + operation.secondOffset;

        auto iterator = geometries_.find(operation.id);
        if (requireExisting !=
            (iterator != geometries_.end())) {
            return RejectExecution(
                "geometry create/update lifecycle");
        }
        if (!requireExisting &&
            geometries_.size() >= kMaximumGeometries) {
            return RejectExecution(
                "geometry object limit");
        }

        const auto needsReplacement =
            iterator == geometries_.end() ||
            !iterator->second.vertexBuffer ||
            !iterator->second.indexBuffer ||
            iterator->second.format !=
                operation.vertexFormat ||
            operation.firstSize >
                iterator->second.vertexCapacity ||
            operation.secondSize >
                iterator->second.indexCapacity;
        if (needsReplacement) {
            const auto oldAllocation =
                iterator == geometries_.end() ?
                0 :
                iterator->second.vertexCapacity +
                    iterator->second.indexCapacity;
            if (operation.secondSize >
                (std::numeric_limits<std::size_t>::max)() -
                    operation.firstSize) {
                return RejectExecution(
                    "geometry allocation overflow");
            }
            const auto newAllocation =
                operation.firstSize +
                operation.secondSize;
            if (liveGeometryBytes_ < oldAllocation ||
                newAllocation > kMaximumGeometryBytes ||
                liveGeometryBytes_ - oldAllocation >
                    kMaximumGeometryBytes -
                        newAllocation) {
                return RejectExecution(
                    "geometry byte budget");
            }

            Geometry replacement;
            replacement.format = operation.vertexFormat;
            replacement.vertexCapacity =
                operation.firstSize;
            replacement.indexCapacity =
                operation.secondSize;
            replacement.vertexCount = vertexCount;
            replacement.indexBytes =
                operation.secondSize;
            if (!GpuGeometryIndexPolicy::
                    BuildInvalidIndexPrefix(
                        replacement.invalidIndexPrefix,
                        indexData,
                        operation.secondSize,
                        vertexCount)) {
                return RejectExecution(
                    "geometry index validation metadata");
            }

            D3D11_BUFFER_DESC vertexDescription{};
            vertexDescription.ByteWidth =
                static_cast<UINT>(operation.firstSize);
            vertexDescription.Usage = D3D11_USAGE_DYNAMIC;
            vertexDescription.BindFlags =
                D3D11_BIND_VERTEX_BUFFER;
            vertexDescription.CPUAccessFlags =
                D3D11_CPU_ACCESS_WRITE;

            D3D11_BUFFER_DESC indexDescription{};
            indexDescription.ByteWidth =
                static_cast<UINT>(operation.secondSize);
            indexDescription.Usage = D3D11_USAGE_DYNAMIC;
            indexDescription.BindFlags =
                D3D11_BIND_INDEX_BUFFER;
            indexDescription.CPUAccessFlags =
                D3D11_CPU_ACCESS_WRITE;

            D3D11_SUBRESOURCE_DATA vertexInitial{};
            vertexInitial.pSysMem = vertexData;
            D3D11_SUBRESOURCE_DATA indexInitial{};
            indexInitial.pSysMem = indexData;
            auto result = device_->CreateBuffer(
                &vertexDescription,
                &vertexInitial,
                replacement.vertexBuffer.GetAddressOf());
            if (SUCCEEDED(result)) {
                result = device_->CreateBuffer(
                    &indexDescription,
                    &indexInitial,
                    replacement.indexBuffer.GetAddressOf());
            }
            if (FAILED(result)) {
                return RejectExecution(
                    "D3D11 geometry-buffer creation",
                    result);
            }

            liveGeometryBytes_ =
                liveGeometryBytes_ - oldAllocation +
                newAllocation;
            if (iterator == geometries_.end()) {
                geometries_.emplace(
                    operation.id,
                    std::move(replacement));
            } else {
                iterator->second = std::move(replacement);
            }
            return true;
        }

        auto& geometry = iterator->second;
        if (geometry.invalidIndexPrefix.capacity() <
            indexCount + 1) {
            return RejectExecution(
                "geometry index validation capacity");
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        auto result = context_->Map(
            geometry.vertexBuffer.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped);
        if (FAILED(result) || !mapped.pData) {
            return RejectExecution(
                "D3D11 vertex-buffer map",
                result);
        }
        std::memcpy(
            mapped.pData,
            vertexData,
            operation.firstSize);
        context_->Unmap(geometry.vertexBuffer.Get(), 0);

        mapped = {};
        result = context_->Map(
            geometry.indexBuffer.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped);
        if (FAILED(result) || !mapped.pData) {
            return RejectExecution(
                "D3D11 index-buffer map",
                result);
        }
        std::memcpy(
            mapped.pData,
            indexData,
            operation.secondSize);
        context_->Unmap(geometry.indexBuffer.Get(), 0);

        if (!GpuGeometryIndexPolicy::
                BuildInvalidIndexPrefix(
                    geometry.invalidIndexPrefix,
                    indexData,
                    operation.secondSize,
                    vertexCount)) {
            return RejectExecution(
                "geometry index validation metadata");
        }
        geometry.vertexCount = vertexCount;
        geometry.indexBytes = operation.secondSize;
        return true;
    }

    bool D3D11GpuDriver::ExecuteOperation(
        const Operation& operation,
        const Batch& batch) noexcept
    {
        switch (operation.kind) {
        case OperationKind::CreateTexture: {
            if (textures_.contains(operation.id) ||
                textures_.size() >= kMaximumTextures ||
                operation.width == 0 ||
                operation.height == 0 ||
                operation.width > kMaximumDimension ||
                operation.height > kMaximumDimension ||
                !SupportedBitmapFormat(
                    operation.bitmapFormat)) {
                return RejectExecution(
                    "texture create lifecycle or description");
            }
            std::size_t pixels = 0;
            std::size_t allocation = 0;
            if (!CheckedProduct(
                    operation.width,
                    operation.height,
                    pixels) ||
                !CheckedProduct(
                    pixels,
                    BytesPerPixel(operation.bitmapFormat),
                allocation) ||
                liveTextureBytes_ >
                    kMaximumTextureBytes - allocation) {
                return RejectExecution(
                    "texture byte budget");
            }
            if (operation.emptyBitmap &&
                operation.bitmapFormat !=
                    ultralight::BitmapFormat::
                        BGRA8_UNORM_SRGB) {
                return RejectExecution(
                    "render-target texture format");
            }

            D3D11_TEXTURE2D_DESC description{};
            description.Width = operation.width;
            description.Height = operation.height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format =
                DxgiFormat(operation.bitmapFormat);
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags =
                D3D11_BIND_SHADER_RESOURCE |
                (operation.emptyBitmap ?
                     D3D11_BIND_RENDER_TARGET :
                     0u);

            D3D11_SUBRESOURCE_DATA initial{};
            const D3D11_SUBRESOURCE_DATA* initialPointer =
                nullptr;
            if (!operation.emptyBitmap) {
                if (operation.firstOffset >
                        batch.bytes.size() ||
                    operation.firstSize >
                        batch.bytes.size() -
                            operation.firstOffset) {
                    return RejectExecution(
                        "texture payload range");
                }
                initial.pSysMem =
                    batch.bytes.data() +
                    operation.firstOffset;
                initial.SysMemPitch = operation.rowBytes;
                initialPointer = &initial;
            }

            Texture texture;
            auto result = device_->CreateTexture2D(
                &description,
                initialPointer,
                texture.resource.GetAddressOf());
            if (SUCCEEDED(result)) {
                result = device_->CreateShaderResourceView(
                    texture.resource.Get(),
                    nullptr,
                    texture.view.GetAddressOf());
            }
            if (FAILED(result)) {
                return RejectExecution(
                    "D3D11 texture or shader-view creation",
                    result);
            }
            texture.width = operation.width;
            texture.height = operation.height;
            texture.bytes = allocation;
            texture.format = operation.bitmapFormat;
            texture.renderTarget = operation.emptyBitmap;
            textures_.emplace(
                operation.id,
                std::move(texture));
            liveTextureBytes_ += allocation;
            return true;
        }

        case OperationKind::UpdateTexture: {
            const auto iterator =
                textures_.find(operation.id);
            if (iterator == textures_.end() ||
                iterator->second.renderTarget ||
                iterator->second.pendingDestroy ||
                operation.emptyBitmap ||
                iterator->second.width != operation.width ||
                iterator->second.height != operation.height ||
                iterator->second.format !=
                    operation.bitmapFormat ||
                operation.firstOffset >
                    batch.bytes.size() ||
                operation.firstSize >
                    batch.bytes.size() -
                        operation.firstOffset) {
                return RejectExecution(
                    "texture update lifecycle or payload");
            }
            context_->UpdateSubresource(
                iterator->second.resource.Get(),
                0,
                nullptr,
                batch.bytes.data() +
                    operation.firstOffset,
                operation.rowBytes,
                0);
            return true;
        }

        case OperationKind::DestroyTexture: {
            const auto iterator =
                textures_.find(operation.id);
            if (iterator == textures_.end()) {
                return RejectExecution(
                    "texture destruction found no resource");
            }
            if (iterator->second.pendingDestroy) {
                return RejectExecution(
                    "texture destruction was already pending");
            }
            if (iterator->second.renderBufferId != 0) {
                iterator->second.pendingDestroy = true;
                return true;
            }
            if (liveTextureBytes_ <
                iterator->second.bytes) {
                return RejectExecution(
                    "texture byte accounting underflow");
            }
            liveTextureBytes_ -= iterator->second.bytes;
            textures_.erase(iterator);
            return true;
        }

        case OperationKind::CreateRenderBuffer: {
            if (renderBuffers_.contains(operation.id) ||
                renderBuffers_.size() >=
                    kMaximumRenderBuffers) {
                return RejectExecution(
                    "render-buffer create lifecycle or object limit");
            }
            auto texture = textures_.find(
                operation.renderBuffer.texture_id);
            if (texture == textures_.end() ||
                !texture->second.renderTarget ||
                texture->second.pendingDestroy ||
                texture->second.renderBufferId != 0 ||
                texture->second.format !=
                    ultralight::BitmapFormat::
                        BGRA8_UNORM_SRGB ||
                texture->second.width !=
                    operation.renderBuffer.width ||
                texture->second.height !=
                    operation.renderBuffer.height) {
                return RejectExecution(
                    "render-buffer backing texture state");
            }

            UINT formatSupport = 0;
            if (FAILED(
                    device_->CheckFormatSupport(
                        DXGI_FORMAT_B8G8R8A8_UNORM,
                        &formatSupport)) ||
                (formatSupport &
                    D3D11_FORMAT_SUPPORT_RENDER_TARGET) == 0 ||
                (formatSupport &
                    D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) == 0 ||
                (formatSupport &
                    D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE) == 0) {
                return RejectExecution(
                    "render-buffer BGRA8 render/resolve support");
            }

            UINT qualityLevels = 0;
            if (FAILED(
                    device_->
                        CheckMultisampleQualityLevels(
                            DXGI_FORMAT_B8G8R8A8_UNORM,
                            kMsaaSampleCount,
                            &qualityLevels)) ||
                qualityLevels == 0) {
                return RejectExecution(
                    "render-buffer required 8x MSAA support");
            }

            const auto allocation =
                GpuResourceBudget::
                    PlanRenderTargetAllocation(
                        liveRenderTargetPixels_,
                        operation.renderBuffer.width,
                        operation.renderBuffer.height);
            if (!allocation.valid) {
                return RejectExecution(
                    "render-buffer logical-pixel budget");
            }

            D3D11_TEXTURE2D_DESC description{};
            description.Width =
                operation.renderBuffer.width;
            description.Height =
                operation.renderBuffer.height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format =
                DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = kMsaaSampleCount;
            description.SampleDesc.Quality =
                static_cast<UINT>(
                    D3D11_STANDARD_MULTISAMPLE_PATTERN);
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags =
                D3D11_BIND_RENDER_TARGET;

            RenderBuffer renderBuffer;
            auto result = device_->CreateTexture2D(
                &description,
                nullptr,
                renderBuffer.multisample.GetAddressOf());
            if (SUCCEEDED(result)) {
                D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
                viewDescription.Format = description.Format;
                viewDescription.ViewDimension =
                    D3D11_RTV_DIMENSION_TEXTURE2DMS;
                result = device_->CreateRenderTargetView(
                    renderBuffer.multisample.Get(),
                    &viewDescription,
                    renderBuffer.view.GetAddressOf());
            }
            if (FAILED(result)) {
                return RejectExecution(
                    "D3D11 render-buffer target creation",
                    result);
            }

            renderBuffer.textureId =
                operation.renderBuffer.texture_id;
            renderBuffer.width =
                operation.renderBuffer.width;
            renderBuffer.height =
                operation.renderBuffer.height;
            renderBuffer.pixelCount = allocation.pixels;
            constexpr float transparent[4]{};
            context_->ClearRenderTargetView(
                renderBuffer.view.Get(),
                transparent);
            renderBuffer.dirty = true;
            texture->second.renderBufferId = operation.id;
            renderBuffers_.emplace(
                operation.id,
                std::move(renderBuffer));
            liveRenderTargetPixels_ += allocation.pixels;
            return true;
        }

        case OperationKind::DestroyRenderBuffer: {
            const auto iterator =
                renderBuffers_.find(operation.id);
            if (iterator == renderBuffers_.end() ||
                liveRenderTargetPixels_ <
                    iterator->second.pixelCount) {
                return RejectExecution(
                    "render-buffer destruction lifecycle or accounting");
            }
            const auto textureId =
                iterator->second.textureId;
            const auto texture =
                textures_.find(textureId);
            if (texture == textures_.end() ||
                texture->second.renderBufferId !=
                    operation.id) {
                return RejectExecution(
                    "render-buffer backing texture ownership");
            }
            if (texture->second.pendingDestroy &&
                liveTextureBytes_ <
                    texture->second.bytes) {
                return RejectExecution(
                    "pending texture byte accounting underflow");
            }

            const auto destroyTexture =
                texture->second.pendingDestroy;
            const auto textureBytes =
                texture->second.bytes;
            texture->second.renderBufferId = 0;
            liveRenderTargetPixels_ -=
                iterator->second.pixelCount;
            renderBuffers_.erase(iterator);
            if (destroyTexture) {
                liveTextureBytes_ -= textureBytes;
                textures_.erase(texture);
            }
            return true;
        }

        case OperationKind::CreateGeometry:
            return CreateOrUpdateGeometry(
                operation,
                batch,
                false);

        case OperationKind::UpdateGeometry:
            return CreateOrUpdateGeometry(
                operation,
                batch,
                true);

        case OperationKind::DestroyGeometry: {
            const auto iterator =
                geometries_.find(operation.id);
            if (iterator == geometries_.end()) {
                return RejectExecution(
                    "geometry destruction found no resource");
            }
            const auto allocation =
                iterator->second.vertexCapacity +
                iterator->second.indexCapacity;
            if (liveGeometryBytes_ < allocation) {
                return RejectExecution(
                    "geometry byte accounting underflow");
            }
            liveGeometryBytes_ -= allocation;
            geometries_.erase(iterator);
            return true;
        }
        }
        return RejectExecution(
            "unknown GPU operation kind");
    }

    bool D3D11GpuDriver::UpdateUniforms(
        const ultralight::GPUState& state) noexcept
    {
        if (!uniformBuffer_ ||
            state.viewport_width == 0 ||
            state.viewport_height == 0 ||
            state.viewport_width > kMaximumDimension ||
            state.viewport_height > kMaximumDimension ||
            !IsFinite(state)) {
            return RejectExecution(
                "uniform state or viewport validation");
        }

        try {
            ultralight::Matrix transform;
            transform.Set(state.transform);
            ultralight::Matrix projection;
            projection.SetOrthographicProjection(
                static_cast<double>(state.viewport_width),
                static_cast<double>(state.viewport_height),
                false);
            projection.Transform(transform);
            const auto projected =
                projection.GetMatrix4x4();

            ShaderUniforms uniforms;
            uniforms.state[1] =
                static_cast<float>(state.viewport_width);
            uniforms.state[2] =
                static_cast<float>(state.viewport_height);
            uniforms.state[3] = 1.0f;
            std::memcpy(
                uniforms.transform,
                projected.data,
                sizeof(uniforms.transform));
            std::memcpy(
                uniforms.scalars,
                state.uniform_scalar,
                sizeof(uniforms.scalars));
            for (std::size_t index = 0;
                 index < std::size(uniforms.vectors);
                 ++index) {
                uniforms.vectors[index][0] =
                    state.uniform_vector[index].x;
                uniforms.vectors[index][1] =
                    state.uniform_vector[index].y;
                uniforms.vectors[index][2] =
                    state.uniform_vector[index].z;
                uniforms.vectors[index][3] =
                    state.uniform_vector[index].w;
            }
            uniforms.clipSize = state.clip_size;
            for (std::uint8_t index = 0;
                 index < state.clip_size;
                 ++index) {
                std::memcpy(
                    uniforms.clips[index],
                    state.clip[index].data,
                    sizeof(uniforms.clips[index]));
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            const auto result = context_->Map(
                uniformBuffer_.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped);
            if (FAILED(result) || !mapped.pData) {
                return RejectExecution(
                    "D3D11 uniform-buffer map",
                    result);
            }
            std::memcpy(
                mapped.pData,
                &uniforms,
                sizeof(uniforms));
            context_->Unmap(uniformBuffer_.Get(), 0);
            return true;
        } catch (...) {
            return RejectExecution(
                "uniform transformation exception");
        }
    }

    bool D3D11GpuDriver::ResolveTexture(
        std::uint32_t textureId) noexcept
    {
        const auto texture = textures_.find(textureId);
        if (texture == textures_.end()) {
            return RejectExecution(
                "texture resolve found no resource");
        }
        if (texture->second.renderBufferId == 0) {
            return true;
        }
        const auto renderBuffer =
            renderBuffers_.find(
                texture->second.renderBufferId);
        if (renderBuffer == renderBuffers_.end() ||
            renderBuffer->second.textureId != textureId ||
            !texture->second.resource ||
            !renderBuffer->second.multisample) {
            return RejectExecution(
                "texture resolve ownership or resource state");
        }
        if (!renderBuffer->second.dirty) {
            return true;
        }

        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->ResolveSubresource(
            texture->second.resource.Get(),
            0,
            renderBuffer->second.multisample.Get(),
            0,
            DXGI_FORMAT_B8G8R8A8_UNORM);
        renderBuffer->second.dirty = false;
        return true;
    }

    bool D3D11GpuDriver::BindTexture(
        UINT slot,
        std::uint32_t textureId) noexcept
    {
        if (slot >= 3) {
            return RejectExecution(
                "texture binding slot");
        }
        ID3D11ShaderResourceView* view = nullptr;
        if (textureId != 0) {
            if (!ResolveTexture(textureId)) {
                return false;
            }
            const auto iterator = textures_.find(textureId);
            if (iterator == textures_.end() ||
                !iterator->second.view) {
                return RejectExecution(
                    "texture binding resource state");
            }
            view = iterator->second.view.Get();
        }
        context_->PSSetShaderResources(slot, 1, &view);
        return true;
    }

    bool D3D11GpuDriver::ExecuteCommand(
        const ultralight::Command& command) noexcept
    {
        const auto& state = command.gpu_state;
        const auto renderBuffer =
            renderBuffers_.find(
                state.render_buffer_id);
        if (renderBuffer == renderBuffers_.end() ||
            !renderBuffer->second.view ||
            !renderBuffer->second.multisample) {
            return RejectExecution(
                "command render-buffer state");
        }

        constexpr std::array<
            ID3D11ShaderResourceView*,
            3>
            emptyViews{};
        context_->PSSetShaderResources(
            0,
            static_cast<UINT>(emptyViews.size()),
            emptyViews.data());
        ID3D11RenderTargetView* target =
            renderBuffer->second.view.Get();
        context_->OMSetRenderTargets(
            1,
            &target,
            nullptr);
        if (command.command_type ==
            ultralight::CommandType::ClearRenderBuffer) {
            constexpr float clear[4]{};
            context_->ClearRenderTargetView(
                target,
                clear);
            renderBuffer->second.dirty = true;
            return true;
        }
        if (command.command_type !=
            ultralight::CommandType::DrawGeometry) {
            return RejectExecution(
                "unknown GPU command type");
        }
        if (state.viewport_width == 0 ||
            state.viewport_height == 0 ||
            state.viewport_width >
                renderBuffer->second.width ||
            state.viewport_height >
                renderBuffer->second.height ||
            !IsFinite(state)) {
            return RejectExecution(
                "command viewport or finite-state validation");
        }

        const auto geometry =
            geometries_.find(command.geometry_id);
        if (geometry == geometries_.end() ||
            !geometry->second.vertexBuffer ||
            !geometry->second.indexBuffer ||
            geometry->second.vertexCount == 0 ||
            !GpuGeometryIndexPolicy::DrawRangeIsValid(
                geometry->second.invalidIndexPrefix,
                geometry->second.indexBytes,
                command.indices_offset,
                command.indices_count)) {
            return RejectExecution(
                "command geometry or index range");
        }
        if (!GpuGeometryIndexPolicy::
                DrawRangeReferencesExistingVertices(
                    geometry->second.invalidIndexPrefix,
                    geometry->second.indexBytes,
                    command.indices_offset,
                    command.indices_count)) {
            return RejectExecution(
                "draw index exceeds vertex count");
        }

        const std::array<std::uint32_t, 3> textureIds{
            state.texture_1_id,
            state.texture_2_id,
            state.texture_3_id
        };
        if (state.shader_type !=
                ultralight::ShaderType::Fill &&
            state.shader_type !=
                ultralight::ShaderType::FillPath) {
            return RejectExecution(
                "command shader type");
        }
        if ((state.shader_type ==
                    ultralight::ShaderType::Fill &&
                geometry->second.format !=
                    ultralight::VertexBufferFormat::
                        _2f_4ub_2f_2f_28f) ||
            (state.shader_type ==
                    ultralight::ShaderType::FillPath &&
                geometry->second.format !=
                    ultralight::VertexBufferFormat::
                        _2f_4ub_2f)) {
            return RejectExecution(
                "command shader and vertex format pairing");
        }
        for (UINT slot = 0;
             slot < textureIds.size();
             ++slot) {
            if (!BindTexture(slot, textureIds[slot])) {
                return false;
            }
        }
        if (!UpdateUniforms(state)) {
            return false;
        }

        const auto& shader =
            state.shader_type ==
                    ultralight::ShaderType::Fill ?
                fillShader_ :
                pathShader_;
        context_->IASetInputLayout(shader.layout.Get());
        context_->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto stride = static_cast<UINT>(
            VertexStride(geometry->second.format));
        constexpr UINT offset = 0;
        auto* vertex =
            geometry->second.vertexBuffer.Get();
        context_->IASetVertexBuffers(
            0,
            1,
            &vertex,
            &stride,
            &offset);
        context_->IASetIndexBuffer(
            geometry->second.indexBuffer.Get(),
            DXGI_FORMAT_R32_UINT,
            0);

        context_->VSSetShader(
            shader.vertex.Get(),
            nullptr,
            0);
        context_->PSSetShader(
            shader.pixel.Get(),
            nullptr,
            0);
        context_->GSSetShader(nullptr, nullptr, 0);
        context_->HSSetShader(nullptr, nullptr, 0);
        context_->DSSetShader(nullptr, nullptr, 0);
        auto* uniforms = uniformBuffer_.Get();
        context_->VSSetConstantBuffers(
            0,
            1,
            &uniforms);
        context_->PSSetConstantBuffers(
            0,
            1,
            &uniforms);
        auto* sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);

        constexpr float blendFactor[4]{};
        context_->OMSetBlendState(
            state.enable_blend ?
                blend_.Get() :
                overwrite_.Get(),
            blendFactor,
            0xFFFFFFFFu);
        context_->OMSetDepthStencilState(
            depthDisabled_.Get(),
            0);
        context_->RSSetState(
            state.enable_scissor ?
                scissorRaster_.Get() :
                raster_.Get());

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(
            state.viewport_width);
        viewport.Height = static_cast<float>(
            state.viewport_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        if (state.enable_scissor) {
            const auto& source =
                state.scissor_rect;
            if (source.right < source.left ||
                source.bottom < source.top) {
                return RejectExecution(
                    "command scissor rectangle");
            }
            D3D11_RECT rectangle{
                static_cast<LONG>(source.left),
                static_cast<LONG>(source.top),
                static_cast<LONG>(source.right),
                static_cast<LONG>(source.bottom)
            };
            context_->RSSetScissorRects(1, &rectangle);
        }

        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->DrawIndexed(
            command.indices_count,
            command.indices_offset,
            0);
        renderBuffer->second.dirty = true;

        context_->PSSetShaderResources(
            0,
            static_cast<UINT>(emptyViews.size()),
            emptyViews.data());
        return true;
    }

    bool D3D11GpuDriver::ResolveAll() noexcept
    {
        constexpr std::array<
            ID3D11ShaderResourceView*,
            3>
            emptyViews{};
        context_->PSSetShaderResources(
            0,
            static_cast<UINT>(emptyViews.size()),
            emptyViews.data());
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        for (auto& [id, renderBuffer] : renderBuffers_) {
            if (!renderBuffer.dirty) {
                continue;
            }
            const auto texture =
                textures_.find(renderBuffer.textureId);
            if (texture == textures_.end() ||
                !texture->second.resource ||
                texture->second.renderBufferId != id) {
                return RejectExecution(
                    "final resolve destination ownership");
            }
            if (!renderBuffer.multisample) {
                return RejectExecution(
                    "final resolve source resource");
            }
            context_->ResolveSubresource(
                texture->second.resource.Get(),
                0,
                renderBuffer.multisample.Get(),
                0,
                DXGI_FORMAT_B8G8R8A8_UNORM);
            renderBuffer.dirty = false;
        }
        return true;
    }

    bool D3D11GpuDriver::ExecutePending() noexcept
    {
        if (fatalError_.load(std::memory_order_acquire) ||
            !ExecutionThreadIsValid("ExecutePending")) {
            return false;
        }
        if (!device_ || !context_) {
            return false;
        }
        const auto removalReason =
            device_->GetDeviceRemovedReason();
        if (FAILED(removalReason)) {
            Fail(
                "ExecutePending",
                "FO4VR D3D11 device was removed or reset",
                removalReason);
            return false;
        }
        if (!EnsurePipeline()) {
            return false;
        }

        Batch executing;
        {
            std::lock_guard lock(batchMutex_);
            if (!pendingReady_) {
                return true;
            }
            std::swap(executing, pending_);
            pendingReady_ = false;
        }

        const auto operationName =
            [](OperationKind kind) noexcept -> const char* {
            switch (kind) {
            case OperationKind::CreateTexture:
                return "CreateTexture";
            case OperationKind::UpdateTexture:
                return "UpdateTexture";
            case OperationKind::DestroyTexture:
                return "DestroyTexture";
            case OperationKind::CreateRenderBuffer:
                return "CreateRenderBuffer";
            case OperationKind::DestroyRenderBuffer:
                return "DestroyRenderBuffer";
            case OperationKind::CreateGeometry:
                return "CreateGeometry";
            case OperationKind::UpdateGeometry:
                return "UpdateGeometry";
            case OperationKind::DestroyGeometry:
                return "DestroyGeometry";
            }
            return "Unknown";
        };

        auto success = true;
        for (std::size_t index = 0;
             index < executing.operations.size();
             ++index) {
            const auto& operation =
                executing.operations[index];
            executionFailureStage_ = "unreported";
            executionFailureResult_ = S_OK;
            if (!ExecuteOperation(operation, executing)) {
                fatalError_.store(
                    true,
                    std::memory_order_release);
                if (!failureLogged_.exchange(
                        true,
                        std::memory_order_acq_rel)) {
                    logger::critical(
                        "PrismaUI GPU operation failed: index {}/{} kind={} id={} stage={} HRESULT=0x{:08X}; texture={}x{} format={} empty={}; renderBufferTexture={} renderBuffer={}x{}; payload={}+{}/{}+{}; liveTextures={} bytes={} liveRenderBuffers={} pixels={} liveGeometries={} bytes={}",
                        index,
                        executing.operations.size(),
                        operationName(operation.kind),
                        operation.id,
                        executionFailureStage_,
                        static_cast<std::uint32_t>(
                            executionFailureResult_),
                        operation.width,
                        operation.height,
                        static_cast<std::uint32_t>(
                            operation.bitmapFormat),
                        operation.emptyBitmap,
                        operation.renderBuffer.texture_id,
                        operation.renderBuffer.width,
                        operation.renderBuffer.height,
                        operation.firstOffset,
                        operation.firstSize,
                        operation.secondOffset,
                        operation.secondSize,
                        textures_.size(),
                        liveTextureBytes_,
                        renderBuffers_.size(),
                        liveRenderTargetPixels_,
                        geometries_.size(),
                        liveGeometryBytes_);
                }
                success = false;
                break;
            }
        }
        if (success) {
            for (std::size_t index = 0;
                 index < executing.commands.size();
                 ++index) {
                const auto& command =
                    executing.commands[index];
                executionFailureStage_ = "unreported";
                executionFailureResult_ = S_OK;
                if (!ExecuteCommand(command)) {
                    fatalError_.store(
                        true,
                        std::memory_order_release);
                    if (!failureLogged_.exchange(
                            true,
                            std::memory_order_acq_rel)) {
                        const auto& scissor =
                            command.gpu_state.scissor_rect;
                        logger::critical(
                            "PrismaUI GPU command failed: index {}/{} type={} stage={} HRESULT=0x{:08X}; renderBuffer={} geometry={} indices={}+{} shader={} viewport={}x{} textures={}/{}/{} scissor={} [{},{},{},{}]; liveTextures={} liveRenderBuffers={} liveGeometries={}",
                            index,
                            executing.commands.size(),
                            static_cast<std::uint32_t>(
                                command.command_type),
                            executionFailureStage_,
                            static_cast<std::uint32_t>(
                                executionFailureResult_),
                            command.gpu_state.render_buffer_id,
                            command.geometry_id,
                            command.indices_offset,
                            command.indices_count,
                            static_cast<std::uint32_t>(
                                command.gpu_state.shader_type),
                            command.gpu_state.viewport_width,
                            command.gpu_state.viewport_height,
                            command.gpu_state.texture_1_id,
                            command.gpu_state.texture_2_id,
                            command.gpu_state.texture_3_id,
                            command.gpu_state.enable_scissor,
                            scissor.left,
                            scissor.top,
                            scissor.right,
                            scissor.bottom,
                            textures_.size(),
                            renderBuffers_.size(),
                            geometries_.size());
                    }
                    success = false;
                    break;
                }
            }
        }
        if (success) {
            executionFailureStage_ = "unreported";
            executionFailureResult_ = S_OK;
            if (!ResolveAll()) {
                Fail(
                    "ExecutePending",
                    executionFailureStage_,
                    executionFailureResult_);
                success = false;
            }
        }

        {
            std::lock_guard lock(batchMutex_);
            if (!recycledReady_) {
                std::swap(executing, recycled_);
                recycledReady_ = true;
            }
        }
        return success;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        D3D11GpuDriver::GetTextureView(
            std::uint32_t textureId) const noexcept
    {
        if (fatalError_.load(std::memory_order_acquire) ||
            textureId == 0) {
            return nullptr;
        }
        const auto iterator = textures_.find(textureId);
        if (iterator == textures_.end()) {
            return nullptr;
        }
        return iterator->second.view;
    }
}
