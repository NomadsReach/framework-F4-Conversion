local function contract_error(source)
    local uncommented = source:gsub("/%*.-%*/", "")
    uncommented = uncommented:gsub("//[^\r\n]*", "")
    local compact = uncommented:gsub("%s+", "")
    local query_start = compact:find("F4SEPlugin_Query(", 1, true)
    local load_start = query_start and
        compact:find("F4SE_PLUGIN_LOAD(", query_start, true)
    if not query_start or not load_start then
        return "Query/Load boundaries could not be located"
    end

    local query = compact:sub(query_start, load_start - 1)
    local compares_loader_to_vr =
        query:find("RuntimeVersion()", 1, true) and
        (query:find("RUNTIME_VR_1_2_72", 1, true) or
         query:find("RUNTIME_LATEST_VR", 1, true))
    if compares_loader_to_vr then
        return
            "QueryInterface::RuntimeVersion() is the F4SEVR 1.10.138 " ..
            "compatibility value and must not be compared with a VR " ..
            "executable-version constant"
    end

    local has_executable_gate =
        compact:find("REL::Module::IsVR()", 1, true) and
        compact:find(
            "REL::Module::get().version()==F4SE::RUNTIME_VR_1_2_72",
            1,
            true)
    local query_uses_executable_gate =
        (query:find("REL::Module::IsVR()", 1, true) and
         query:find(
             "REL::Module::get().version()==F4SE::RUNTIME_VR_1_2_72",
             1,
             true)) or
        query:find("IsExactSupportedRuntime()", 1, true)
    if not has_executable_gate or not query_uses_executable_gate then
        return
            "Query must reject non-VR or non-1.2.72 executables through " ..
            "REL::Module, directly or through IsExactSupportedRuntime()"
    end
end

local function fail(source_label, message)
    raise(
        "FO4VR query contract violation (" ..
        source_label ..
        "): " ..
        message)
end

function main(source_path, source_label)
    source_label = source_label or "plugin"

    if source_path == "self-test" then
        local valid = [[
            F4SEPlugin_Query() {
                return REL::Module::IsVR() &&
                    REL::Module::get().version() ==
                        F4SE::RUNTIME_VR_1_2_72;
            }
            F4SE_PLUGIN_LOAD()
        ]]
        local invalid = [[
            F4SEPlugin_Query() {
                return RuntimeVersion() ==
                    F4SE::RUNTIME_VR_1_2_72;
            }
            F4SE_PLUGIN_LOAD()
        ]]
        if contract_error(valid) then
            raise("FO4VR query contract self-test rejected valid input")
        end
        if not contract_error(invalid) then
            raise("FO4VR query contract self-test accepted invalid input")
        end
        print("FO4VR query contract validator self-test passed")
        return
    end

    if not source_path then
        fail(source_label, "source path was not provided")
    end

    local source = io.readfile(source_path)
    if not source then
        fail(source_label, "source file could not be read")
    end

    local message = contract_error(source)
    if message then
        fail(source_label, message)
    end
end
