#pragma once

#include <memory>

namespace PrismaUI::Core {
    struct PrismaView;
}

namespace PrismaUI::RenderRetirement {
    void Enqueue(std::shared_ptr<Core::PrismaView> viewData);
    void Drain();
}
