cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(PrismaUI_F4 VERSION 1.0.0 LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
set(BUILD_ROOT "${CMAKE_SOURCE_DIR}/build")

include(CompilerFlags)
include(ExternalDependencies)

find_package(minhook CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")
list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/resource.rc")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files" FILES ${SOURCES})

add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_compile_definitions(${PROJECT_NAME} PRIVATE _UNICODE UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${ULTRALIGHT_INCLUDE_DIR}"
)

target_link_directories(${PROJECT_NAME} PRIVATE "${ULTRALIGHT_LIBRARY_DIR}")
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        CommonLibF4::CommonLibF4
        minhook::minhook
        Microsoft::DirectXTK
        spdlog::spdlog
        AppCore
        Ultralight
        UltralightCore
        WebCore
        delayimp
        comctl32
        shell32
        imm32
        bcrypt
        version
        psapi
)

target_link_options(${PROJECT_NAME}
    PRIVATE
        "/DELAYLOAD:UltralightCore.dll"
        "/DELAYLOAD:WebCore.dll"
        "/DELAYLOAD:Ultralight.dll"
        "/DELAYLOAD:AppCore.dll"
)

target_precompile_headers(${PROJECT_NAME} PRIVATE "src/PCH.h")

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

set(DIST_ROOT "${CMAKE_SOURCE_DIR}/dist")
set(DIST_VERSION_DIR "${DIST_ROOT}/PrismaUI_F4_${PROJECT_VERSION}")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_VERSION_DIR}/PrismaUI_F4/libs"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_VERSION_DIR}/F4SE/plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/" "${DIST_VERSION_DIR}/PrismaUI_F4"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ULTRALIGHT_RESOURCES_DIR}" "${DIST_VERSION_DIR}/PrismaUI_F4/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ULTRALIGHT_BINARY_DIR}" "${DIST_VERSION_DIR}/PrismaUI_F4/libs"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${PROJECT_NAME}>" "${DIST_VERSION_DIR}/F4SE/plugins/"
)
