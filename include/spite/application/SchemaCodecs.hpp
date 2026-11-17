#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "application/ConfigStore.hpp"
#include "serialization/Serializer.hpp"

namespace spite
{
    // ConfigStore codecs for a SPITE_SCHEMA type: the file is the schema JSON,
    // decoded on top of a copy of the base so missing fields keep their base
    // values and unknown keys are ignored. A module registering a section of a
    // schema type (physics.world) uses it too.
    template <typename T>
    ConfigCodecs schemaCodecs()
    {
        return {[](const void* value)
                {
                    std::string text;
                    serializeJson(T::spiteSchema(), value, text, CONFIG_FILE_INDENT);
                    return text;
                },
                [](const void* base, std::string_view text) -> std::shared_ptr<void>
                {
                    auto value = std::make_shared<T>(*static_cast<const T*>(base));
                    std::string error;
                    if (!text.empty() && !deserializeJson(T::spiteSchema(), value.get(), text, error))
                    {
                        return nullptr;
                    }
                    return value;
                }};
    }
} // namespace spite
