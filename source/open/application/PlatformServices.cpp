#include "application/PlatformServices.hpp"


#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <chrono>
#include <thread>

#include "application/input/InputSystem.hpp"
#include "input/SdlInputDevice.hpp"

namespace spite
{
    PlatformServices::PlatformServices(const bool headless)
    {
#ifdef SPITE_SHOWCASE_BUNDLE
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#endif
        // the engine draws the composition text itself (underlined at the caret); the input method keeps its
        // candidate window, which the text input area places
        SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
        if (!SDL_Init(headless ? SDL_INIT_EVENTS : SDL_INIT_VIDEO))
        {
            SERROR_LOG("SDL initialization failed: {}", SDL_GetError());
            SASSERT(false);
        }
        if (!headless && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            SWARNING_LOG("SDL audio is unavailable: {}", SDL_GetError());
        }
    }

    PlatformServices::~PlatformServices() { SDL_Quit(); }

    std::unique_ptr<InputSystem> PlatformServices::createInputSystem(const InputConfig& config, const Window* window)
    {
        auto input = std::make_unique<InputSystem>(config);
        const auto device = std::make_shared<SdlInputDevice>();
        if (window)
        {
            device->attach(*input, static_cast<SDL_Window*>(window->nativeWindowHandle()));
        }
        input->setDevicePoll([device](InputSystem& inputSystem) { device->poll(inputSystem); });
        return input;
    }

    void PlatformServices::sleep(float seconds) { std::this_thread::sleep_for(std::chrono::duration<float>(seconds)); }

} // namespace spite
