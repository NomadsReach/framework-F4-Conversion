#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/View.h>
#pragma warning(pop)

#include "Core.h"

namespace PrismaUI::PapyrusBridge {

void InjectBridge(ultralight::View* caller, Core::PrismaViewId viewId);

}
