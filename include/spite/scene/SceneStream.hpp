#pragma once

#include <span>
#include <string>
#include <vector>

#include "SceneDocument.hpp"
#include "SceneExport.hpp"
#include "base/Expected.hpp"

namespace spite
{
    // Scene file streaming over the document seam
    //. Strict on structure (bad magic, unknown version, truncation
    // -> Expected error), tolerant on content - content tolerance logs a
    // SWARNING_LOG.
    //
    // Binary is a same-build cache: payloads are positional, each tagged with
    // its component's persisted layout hash; a component whose layout changed
    // since the save refuses the whole file. Unknown components stay verbatim.
    inline constexpr u8 SCENE_BINARY_MAGIC[4] = {'S', 'P', 'S', 'D'};
    inline constexpr u32 SCENE_BINARY_VERSION = 2;

    SPITE_SCENE_API void emitSceneBinary(const SceneDocument& document, std::vector<u8>& outBytes);
    SPITE_SCENE_API Expected<SceneDocument> parseSceneBinary(std::span<const u8> bytes);

    // JSON is the saved form: component payloads render as readable fields
    // through their schemas and parse back by field name against the current
    // schemas - a field the save lacks takes its default, a saved field the
    // schema no longer persists drops, a value of the wrong shape keeps the
    // default; each lands in SceneDocument::changes and the rest of the
    // component is kept. Components this process does not register as scene
    // data are skipped (emit: logged; parse: logged and a Skipped change).
    SPITE_SCENE_API void emitSceneJson(const SceneDocument& document, std::string& outText);
    SPITE_SCENE_API Expected<SceneDocument> parseSceneJson(std::string_view text);
} // namespace spite
