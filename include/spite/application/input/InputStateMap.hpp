#pragma once

#include <unordered_map>

#include "application/ApplicationExport.hpp"
#include "base/Platform.hpp"

namespace spite
{
    // Per-key state: isPressed is the current frame, wasPressed the value
    // latched by reset() at the end of the previous frame; holdingTime
    // accumulates while the key is down and zeroes on release.
    struct ButtonState
    {
        bool isPressed{false};
        bool wasPressed{false};
        float holdingTime{0.f};
    };

    // Key state machine: key code -> ButtonState. Event-driven: the frame only
    // advances hold times (update); press/release transitions arrive through
    // setButton* calls.
    class SPITE_APPLICATION_API InputStateMap
    {
        std::unordered_map<u32, ButtonState> m_stateMap;

    public:
        InputStateMap() = default;

        [[nodiscard]] bool isDown(u32 scancode) const;
        [[nodiscard]] bool isJustPressed(u32 scancode) const;
        [[nodiscard]] bool isJustReleased(u32 scancode) const;
        [[nodiscard]] float holdingTime(u32 scancode) const;

        void setButtonState(u32 scancode, bool pressed);
        void setButtonPress(u32 scancode);
        void setButtonRelease(u32 scancode);
        void update(float deltaTime);
        void reset();
        void clear();

        [[nodiscard]] const std::unordered_map<u32, ButtonState>& getStateMap() const { return m_stateMap; }

    private:
        void registerKey(u32 scancode);
    };
} // namespace spite
