#pragma once
#include <filesystem>
#include <memory>
#include <limits>
#include <string>
#include "audio/AudioExport.hpp"
#include "base/InternedString.hpp"
#include "ecs/EcsTypes.hpp"
namespace spite
{
    class World;
    class ControlServer;
    SPITE_AUDIO_API void registerAudioCommands(ControlServer& control);
    class SystemContext;
    SPITE_AUDIO_API void registerAudioComponents();
    SPITE_AUDIO_API void updateAudio(SystemContext& ctx);

    struct AudioProbeResult
    {
        std::string error;
        u32 channels = 0;
        u32 sampleRate = 0;
        u64 frames = 0;
    };

    SPITE_AUDIO_API AudioProbeResult probeAudio(const std::filesystem::path& path);

    struct AudioVoiceStatus
    {
        bool loaded = false;
        bool playing = false;
        bool ended = false;
    };

    struct AudioVoiceDetails
    {
        float pitch = 1.0f;
        float minDistance = 1.0f;
        float maxDistance = std::numeric_limits<float>::max();
        float rolloff = 1.0f;
        InternedString category;
        float effectiveGain = 0.0f;
    };

    // One play world's device and sounds. Destroy before the world. A failed device
    // leaves the service available but silent; available() exposes the failure.
    class SPITE_AUDIO_API AudioWorld
    {
    public:
        AudioWorld(World& world, std::filesystem::path root, bool enabled = true, bool offline = false);
        ~AudioWorld();
        AudioWorld(const AudioWorld&) = delete;
        AudioWorld& operator=(const AudioWorld&) = delete;
        bool available() const;
        // Linear gain in [0, 1]; invalid values are rejected.
        bool setMasterGain(float gain);
        float masterGain() const;
        bool setCategoryGain(InternedString category, float gain);
        float categoryGain(InternedString category) const;
        AudioVoiceStatus status(Entity entity) const;
        AudioVoiceDetails details(Entity entity) const;
        // Offline mode renders interleaved stereo float frames without an output device.
        bool renderOffline(float* samples, u64 frames);
        bool play(Entity entity);
        bool pause(Entity entity);
        bool stop(Entity entity);
        // Drops the cached voice so the next frame reopens the clip.
        bool reload(Entity entity);

    private:
        friend void updateAudio(SystemContext&);
        void update(SystemContext& ctx);
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
