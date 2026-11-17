#pragma once

#include <span>

#include "application/ConfigStore.hpp"

namespace spite
{
    // The field tables of the application's config sections: doc, unit, bounds, enum choices and
    // live-versus-startup for every leaf of the schema, keyed by dotted path.
    [[nodiscard]] std::span<const ConfigFieldMeta> windowConfigFields();
    [[nodiscard]] std::span<const ConfigFieldMeta> graphicsConfigFields();
    [[nodiscard]] std::span<const ConfigFieldMeta> inputConfigFields();
    [[nodiscard]] std::span<const ConfigFieldMeta> timingConfigFields();
} // namespace spite
