#pragma once

#pragma warning(push)
#pragma warning(disable : 4200 4201 4324)
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#pragma warning(pop)

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

using namespace std::literals;

namespace logger = F4SE::log;

#define PRISMA_DLLEXPORT __declspec(dllexport)
