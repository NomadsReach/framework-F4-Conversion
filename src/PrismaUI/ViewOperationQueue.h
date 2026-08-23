#pragma once

#include <cstddef>
#include <functional>

#include "Core.h"

namespace PrismaUI::ViewOperationQueue {
using OperationFunc = std::function<void()>;
inline constexpr size_t MAX_OPERATIONS_PER_VIEW = 64;

bool EnqueueOperation(Core::PrismaViewId viewId, OperationFunc operation);
void ProcessNextOperation(Core::PrismaViewId viewId);
void ProcessAllViewOperations();
void ClearOperations(Core::PrismaViewId viewId);
size_t GetQueueSize(Core::PrismaViewId viewId);
bool IsProcessing(Core::PrismaViewId viewId);

}
