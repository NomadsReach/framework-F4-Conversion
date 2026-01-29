# SKSEPlugin.cmake - Setup for SKSE plugin using CommonLibSSE-NG submodule

# CommonLibSSE-NG configuration
set(CommonLibPath "external/commonlibsse-ng")
set(CommonLibName "CommonLibSSE")

# Read CommonLibSSE version from vcpkg.json
set(COMMONLIB_VCPKG_JSON_PATH "${CMAKE_SOURCE_DIR}/${CommonLibPath}/vcpkg.json")
if(NOT EXISTS "${COMMONLIB_VCPKG_JSON_PATH}")
    message(FATAL_ERROR
        "CommonLibSSE-NG vcpkg.json not found at \"${COMMONLIB_VCPKG_JSON_PATH}\". "
        "Ensure the CommonLibSSE-NG submodule is checked out and includes vcpkg.json with a \"version-semver\" field.")
endif()
file(READ "${COMMONLIB_VCPKG_JSON_PATH}" COMMONLIB_VCPKG_JSON_CONTENT)
string(JSON COMMONLIBSSE_VERSION ERROR_VARIABLE _COMMONLIBSSE_VERSION_JSON_ERROR GET "${COMMONLIB_VCPKG_JSON_CONTENT}" "version-semver")
if(_COMMONLIBSSE_VERSION_JSON_ERROR)
    message(FATAL_ERROR
        "Failed to extract \"version-semver\" from \"${COMMONLIB_VCPKG_JSON_PATH}\": ${_COMMONLIBSSE_VERSION_JSON_ERROR}")
endif()
message(STATUS "Configuring CommonLibSSE-NG version ${COMMONLIBSSE_VERSION}")
set(COMMONLIBSSE_VERSION "${COMMONLIBSSE_VERSION}" CACHE STRING "CommonLibSSE-NG version" FORCE)

# Save original build type
set(_saved_build_type "${CMAKE_BUILD_TYPE}")

# Always build CommonLibSSE in Release mode to disable assertions and enable optimizations
set(CMAKE_BUILD_TYPE "Release")
add_definitions(-D_CRT_SECURE_NO_WARNINGS)
add_definitions(-DNDEBUG)

# Disable CommonLibSSE tests when building as subdirectory
set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)

# Check if CommonLibSSE has already been built
set(COMMONLIB_STAMP_FILE "${BUILD_ROOT}/external_builds/${CommonLibName}.stamp")
# Add CommonLibSSE-NG as a subdirectory with EXCLUDE_FROM_ALL
add_subdirectory("${CommonLibPath}" "${BUILD_ROOT}/external_builds/${CommonLibName}" EXCLUDE_FROM_ALL)

# Create stamp file after configuration (informational only)
if(NOT EXISTS "${COMMONLIB_STAMP_FILE}")
    file(WRITE "${COMMONLIB_STAMP_FILE}" "Configured on ${CMAKE_SYSTEM_NAME}")
endif()

# Include the CommonLibSSE helper cmake functions (provides add_commonlibsse_plugin macro)
include("${CommonLibPath}/cmake/CommonLibSSE.cmake")

# Restore original build type for the main project
set(CMAKE_BUILD_TYPE "${_saved_build_type}")

# Expose CommonLibSSE version to C++ code
add_compile_definitions(COMMONLIBSSE_VERSION="${COMMONLIBSSE_VERSION}")
