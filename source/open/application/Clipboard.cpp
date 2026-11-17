#include "application/Clipboard.hpp"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>

namespace spite
{
    std::string getClipboardText()
    {
        char* text = SDL_GetClipboardText();
        std::string result = text ? text : "";
        SDL_free(text);
        return result;
    }

    void setClipboardText(const std::string_view text) { SDL_SetClipboardText(std::string(text).c_str()); }
} // namespace spite
