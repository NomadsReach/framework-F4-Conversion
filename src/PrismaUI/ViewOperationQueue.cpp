#include "ViewOperationQueue.h"

#include "Core.h"

namespace PrismaUI::ViewOperationQueue {
using namespace Core;

namespace {

std::shared_ptr<PrismaView> GetView(Core::PrismaViewId viewId)
{
    std::shared_lock lock(viewsMutex);
    auto it = views.find(viewId);
    return it == views.end() ? nullptr : it->second;
}

}

bool EnqueueOperation(Core::PrismaViewId viewId, OperationFunc operation)
{
    if (!operation) return false;

    auto viewData = GetView(viewId);
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire)) return false;

    std::lock_guard lock(viewData->operationMutex);
    if (viewData->isDestroying.load(std::memory_order_acquire)) return false;
    if (viewData->pendingOperations.size() >= MAX_OPERATIONS_PER_VIEW) {
        logger::error("View [{}] operation queue is full", viewId);
        return false;
    }

    viewData->pendingOperations.push(std::move(operation));
    viewData->queuedOperationsCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ProcessNextOperation(Core::PrismaViewId viewId)
{
    auto viewData = GetView(viewId);
    if (!viewData || viewData->isDestroying.load(std::memory_order_acquire)) return;

    bool expected = false;
    if (!viewData->isProcessingOperation.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                                 std::memory_order_relaxed)) {
        return;
    }

    OperationFunc operation;
    {
        std::lock_guard lock(viewData->operationMutex);
        if (viewData->isDestroying.load(std::memory_order_acquire) || viewData->pendingOperations.empty()) {
            viewData->isProcessingOperation.store(false, std::memory_order_release);
            return;
        }

        operation = std::move(viewData->pendingOperations.front());
        viewData->pendingOperations.pop();
        viewData->queuedOperationsCount.fetch_sub(1, std::memory_order_relaxed);
    }

    try {
        ultralightThread.submit_with_priority(
            SingleThreadExecutor::Priority::HIGH,
            [viewId, viewData, operation = std::move(operation)]() mutable {
                struct Reset {
                    std::shared_ptr<PrismaView> view;
                    ~Reset()
                    {
                        view->isProcessingOperation.store(false, std::memory_order_release);
                    }
                } reset{viewData};

                if (viewData->isDestroying.load(std::memory_order_acquire)) return;

                {
                    std::shared_lock lock(viewsMutex);
                    auto it = views.find(viewId);
                    if (it == views.end() || it->second.get() != viewData.get()) return;
                }

                try {
                    operation();
                } catch (const std::exception& e) {
                    logger::error("View [{}] operation failed: {}", viewId, e.what());
                } catch (...) {
                    logger::error("View [{}] operation failed", viewId);
                }
            });
    } catch (const std::exception& e) {
        viewData->isProcessingOperation.store(false, std::memory_order_release);
        logger::error("View [{}] operation dispatch failed: {}", viewId, e.what());
    }
}

void ProcessAllViewOperations()
{
    std::vector<Core::PrismaViewId> viewIds;
    {
        std::shared_lock lock(viewsMutex);
        viewIds.reserve(views.size());
        for (const auto& [id, view] : views) {
            if (view && !view->isDestroying.load(std::memory_order_acquire)) viewIds.push_back(id);
        }
    }

    for (const auto viewId : viewIds) ProcessNextOperation(viewId);
}

void ClearOperations(Core::PrismaViewId viewId)
{
    auto viewData = GetView(viewId);
    if (!viewData) return;

    std::lock_guard lock(viewData->operationMutex);
    while (!viewData->pendingOperations.empty()) viewData->pendingOperations.pop();
    viewData->queuedOperationsCount.store(0, std::memory_order_relaxed);
}

size_t GetQueueSize(Core::PrismaViewId viewId)
{
    auto viewData = GetView(viewId);
    if (!viewData) return 0;

    std::lock_guard lock(viewData->operationMutex);
    return viewData->pendingOperations.size();
}

bool IsProcessing(Core::PrismaViewId viewId)
{
    auto viewData = GetView(viewId);
    return viewData && viewData->isProcessingOperation.load(std::memory_order_acquire);
}

}
