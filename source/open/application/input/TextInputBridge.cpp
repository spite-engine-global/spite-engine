#include "application/input/TextInputBridge.hpp"

#include "application/input/InputSystem.hpp"

namespace spite
{
    void TextInputBridge::apply(InputSystem& input, ITextInputPlatform& platform)
    {
        const bool enabled = input.isTextInputRequested();
        if (enabled != m_enabled)
        {
            m_enabled = enabled;
            platform.setTextInput(enabled);
        }
        if (input.consumeClearCompositionRequest())
        {
            platform.clearComposition();
        }
        const std::optional<TextInputArea>& area = input.textInputArea();
        if (!enabled || !area)
        {
            m_sentArea.reset();
            return;
        }
        if (m_sentArea != area)
        {
            m_sentArea = area;
            platform.setTextInputArea(*area);
        }
    }
} // namespace spite
