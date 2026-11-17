#pragma once

#include "ui/UiComponents.hpp"

namespace spite
{
    struct UiPaintRow
    {
        const UiIdentityComponent* identity = nullptr;
        const UiStateComponent* state = nullptr;
        const UiImageComponent* image = nullptr;
        const UiLabelComponent* label = nullptr;
        const UiValueComponent* value = nullptr;
        const UiTextEditComponent* edit = nullptr;
        const UiStyleAnimComponent* anim = nullptr;
    };

} // namespace spite
