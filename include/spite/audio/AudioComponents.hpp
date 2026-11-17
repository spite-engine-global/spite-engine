#pragma once
#include "base/InternedString.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include <limits>

namespace spite
{
    // clip is a UTF-8 path relative to assets/audio under the content root. WAV, MP3 and FLAC are supported;
    // packaged games include clips referenced by included scenes. A spatial clip should be mono. Short sounds
    // decode at binding; set stream for long tracks. reloadAudio reopens an edited clip.
    struct AudioEmitterComponent : IEngineComponent
    {
        InternedString clip;
        f32 volume = 1.0f;
        bool playing = true;
        bool loop = false;
        bool spatial = true;
        bool stream = false;
        SPITE_SCHEMA(AudioEmitterComponent, clip, SPITE_FIELD(volume, fieldRange(0.0, 1.0)), playing, loop, spatial,
                     stream)
    };

    struct AudioVoiceSettingsComponent : IEngineComponent
    {
        InternedString category;
        f32 pitch = 1.0f;
        f32 minDistance = 1.0f;
        f32 maxDistance = std::numeric_limits<f32>::max();
        f32 rolloff = 1.0f;
        SPITE_SCHEMA(AudioVoiceSettingsComponent, category, SPITE_FIELD(pitch, fieldRange(0.01, 16.0)),
                     SPITE_FIELD(minDistance, fieldRange(0.01, 1.0e30)),
                     SPITE_FIELD(maxDistance, fieldRange(0.01, static_cast<double>(std::numeric_limits<f32>::max()))),
                     SPITE_FIELD(rolloff, fieldRange(0.0, 100.0)))
    };
    // When several listeners exist, the one with the lowest entity ID wins.
    struct AudioListenerComponent : IEngineComponent
    {
        bool enabled = true;
        SPITE_SCHEMA(AudioListenerComponent, enabled)
    };
} // namespace spite
