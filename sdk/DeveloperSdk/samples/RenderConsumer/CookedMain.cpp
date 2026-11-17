#include "engine/CookedGame.hpp"
#include "engine/Module.hpp"

extern "C" const spite::ModuleDescriptor* SPITE_MODULE_ENTRY();

const spite::ModuleDescriptor* spite::findLinkedModule(std::string_view name)
{
    return name == "RenderConsumer" ? SPITE_MODULE_ENTRY() : nullptr;
}

int main(int argc, char* argv[]) { return spite::runCookedGame(argc, argv, "RenderConsumer", "scenes/start.json"); }
