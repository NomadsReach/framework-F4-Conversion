local function strip_comments(source)
    local uncommented = source:gsub("/%*.-%*/", "")
    return uncommented:gsub("//[^\r\n]*", "")
end

local function method_body(source, method_name)
    local signature = "D3D11GpuDriver::" .. method_name .. "()"
    local method_start = source:find(signature, 1, true)
    if not method_start then
        return nil, method_name .. " could not be located"
    end

    local body_start = source:find("{", method_start, true)
    if not body_start then
        return nil, method_name .. " body could not be located"
    end

    local depth = 0
    for index = body_start, #source do
        local character = source:sub(index, index)
        if character == "{" then
            depth = depth + 1
        elseif character == "}" then
            depth = depth - 1
            if depth == 0 then
                return source:sub(body_start + 1, index - 1)
            end
        end
    end

    return nil, method_name .. " body is unterminated"
end

local function contract_error(source)
    source = strip_comments(source)
    local allocators = {
        {
            method = "NextTextureId",
            counter = "nextTextureId_"
        },
        {
            method = "NextRenderBufferId",
            counter = "nextRenderBufferId_"
        },
        {
            method = "NextGeometryId",
            counter = "nextGeometryId_"
        }
    }

    for _, allocator in ipairs(allocators) do
        local body, error_message =
            method_body(source, allocator.method)
        if not body then
            return error_message
        end

        local compact = body:gsub("%s+", "")
        if compact:find(
                "SynchronizationCallIsValid",
                1,
                true) then
            return
                allocator.method ..
                " must remain callable outside BeginSynchronize/EndSynchronize"
        end
        if not compact:find(
                "fatalError_.load(std::memory_order_acquire)",
                1,
                true) then
            return
                allocator.method ..
                " must fail closed after a fatal GPU-driver error"
        end
        if not compact:find(
                allocator.counter .. "==0",
                1,
                true) or
            not compact:find(
                "return" .. allocator.counter .. "++;",
                1,
                true) then
            return
                allocator.method ..
                " must preserve monotonic non-zero ID allocation"
        end
    end
end

local function fail(message)
    raise("FO4VR GPU-driver contract violation: " .. message)
end

function main(source_path)
    if source_path == "self-test" then
        local valid = [[
            std::uint32_t D3D11GpuDriver::NextTextureId() {
                if (fatalError_.load(std::memory_order_acquire) ||
                    nextTextureId_ == 0) {
                    return 0;
                }
                return nextTextureId_++;
            }
            std::uint32_t D3D11GpuDriver::NextRenderBufferId() {
                if (fatalError_.load(std::memory_order_acquire) ||
                    nextRenderBufferId_ == 0) {
                    return 0;
                }
                return nextRenderBufferId_++;
            }
            std::uint32_t D3D11GpuDriver::NextGeometryId() {
                if (fatalError_.load(std::memory_order_acquire) ||
                    nextGeometryId_ == 0) {
                    return 0;
                }
                return nextGeometryId_++;
            }
        ]]
        local invalid = valid:gsub(
            "if %(fatalError_",
            "if (!SynchronizationCallIsValid(\"NextTextureId\") || " ..
            "fatalError_",
            1)
        if contract_error(valid) then
            raise(
                "FO4VR GPU-driver contract self-test rejected valid input")
        end
        if not contract_error(invalid) then
            raise(
                "FO4VR GPU-driver contract self-test accepted invalid input")
        end
        print("FO4VR GPU-driver contract validator self-test passed")
        return
    end

    if not source_path then
        fail("source path was not provided")
    end

    local source = io.readfile(source_path)
    if not source then
        fail("source file could not be read")
    end

    local message = contract_error(source)
    if message then
        fail(message)
    end
end
