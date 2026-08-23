#pragma once

#pragma warning(push)
#include <RE/Fallout.h>
#include <F4SE/F4SE.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::literals;

namespace logger = spdlog;
