#include "audio/AudioWorld.hpp"
#include "audio/AudioComponents.hpp"
#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/Query.hpp"
#include "ecs/LifecycleEvents.hpp"
#include "ecs/World.hpp"
#include "world/TransformComponents.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#define NOMINMAX
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

namespace spite
{
    namespace
    {
        constexpr const char* SYSTEM = "Audio";
        struct Voice
        {
            ma_sound sound{};
            InternedString clip;
            InternedString category;
            float volume = -1.0f;
            float categoryGain = 1.0f;
            bool stream = false;
            bool initialized = false;
            bool lastPlaying = false;
            bool replayRequested = false;
            float pitch = 1.0f;
            float minDistance = 1.0f;
            float maxDistance = std::numeric_limits<float>::max();
            float rolloff = 1.0f;
            ~Voice()
            {
                if (initialized)
                    ma_sound_uninit(&sound);
            }
        };
    } // namespace

    AudioProbeResult probeAudio(const std::filesystem::path& path)
    {
        AudioProbeResult result;
        ma_decoder decoder{};
        const auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
#ifdef _WIN32
        const ma_result opened = ma_decoder_init_file_w(path.c_str(), &config, &decoder);
#else
        const ma_result opened = ma_decoder_init_file(path.c_str(), &config, &decoder);
#endif
        if (opened != MA_SUCCESS)
        {
            result.error = std::string("cannot decode audio: ") + ma_result_description(opened);
            return result;
        }
        struct DecoderGuard
        {
            ma_decoder* decoder;
            ~DecoderGuard() { ma_decoder_uninit(decoder); }
        } guard{&decoder};
        result.channels = decoder.outputChannels;
        result.sampleRate = decoder.outputSampleRate;
        if (!result.channels || result.channels > 256 || !result.sampleRate)
        {
            result.error = "invalid audio channel count or sample rate";
            return result;
        }
        ma_uint64 expected = 0;
        const ma_result length = ma_decoder_get_length_in_pcm_frames(&decoder, &expected);
        std::array<float, 4096> samples{};
        const ma_uint64 capacity = samples.size() / result.channels;
        for (;;)
        {
            ma_uint64 count = 0;
            const ma_result read = ma_decoder_read_pcm_frames(&decoder, samples.data(), capacity, &count);
            result.frames += count;
            if (read != MA_SUCCESS && read != MA_AT_END)
            {
                result.error = std::string("audio decode failed: ") + ma_result_description(read);
                break;
            }
            if (!count || read == MA_AT_END)
                break;
        }
        if (result.error.empty() && (!result.frames || (length == MA_SUCCESS && result.frames < expected)))
            result.error = "audio is empty or truncated";
        return result;
    }

    struct AudioWorld::State
    {
        World world;
        std::filesystem::path root;
        ma_engine engine{};
        bool ready = false;
        bool offline = false;
        float masterGain = 1.0f;
        std::unordered_map<InternedString, float> categoryGains;
        Handle emitterRemoved;
        Handle transformRemoved;
        std::unordered_map<u64, std::unique_ptr<Voice>> voices;
        State(World w, std::filesystem::path path) : world(w), root(std::move(path)) {}
        ~State()
        {
            voices.clear();
            if (ready)
                ma_engine_uninit(&engine);
        }
    };

    void registerAudioComponents()
    {
        registerComponent<AudioEmitterComponent>();
        registerComponent<AudioVoiceSettingsComponent>();
        registerComponent<AudioListenerComponent>();
    }

    AudioWorld::AudioWorld(World& world, std::filesystem::path root, bool enabled, bool offline) :
        m_state(std::make_unique<State>(world, std::move(root)))
    {
        registerAudioComponents();
        registerComponent<TransformMatrixComponent>();
        if (enabled)
        {
            ma_engine_config config = ma_engine_config_init();
            config.noDevice = offline;
            if (offline)
            {
                config.channels = 2;
                config.sampleRate = 48000;
            }
            m_state->offline = offline;
            m_state->ready = ma_engine_init(&config, &m_state->engine) == MA_SUCCESS;
            if (!m_state->ready)
                SWARNING_LOG("audio: playback device unavailable");
        }
        world.services().registerService(this);
        m_state->emitterRemoved = world.lifecycleEvents().onComponentRemoved(
            getComponentId<AudioEmitterComponent>(), [this](Entity entity) { m_state->voices.erase(entity.id()); });
        m_state->transformRemoved = world.lifecycleEvents().onComponentRemoved(
            getComponentId<TransformMatrixComponent>(), [this](Entity entity) { m_state->voices.erase(entity.id()); });
        world.registerSystem(SystemDescriptor{
            .name = SYSTEM,
            .fn = &updateAudio,
            .update = SystemPhaseDescriptor{
                .reads = {getComponentId<AudioEmitterComponent>(), getComponentId<AudioVoiceSettingsComponent>(),
                          getComponentId<AudioListenerComponent>(), getComponentId<TransformMatrixComponent>(),
                          getComponentId<InactiveComponent>()},
                .stage = CoreExecutionStages::PRE_RENDER}});
    }

    AudioWorld::~AudioWorld()
    {
        m_state->world.lifecycleEvents().unsubscribe(m_state->emitterRemoved);
        m_state->world.lifecycleEvents().unsubscribe(m_state->transformRemoved);
        m_state->world.removeSystem({nullptr, SYSTEM});
        m_state->world.services().unregisterService<AudioWorld>();
    }
    bool AudioWorld::available() const { return m_state->ready; }
    bool AudioWorld::setMasterGain(float gain)
    {
        if (!std::isfinite(gain) || gain < 0.0f || gain > 1.0f)
            return false;
        if (m_state->ready && ma_engine_set_volume(&m_state->engine, gain) != MA_SUCCESS)
            return false;
        m_state->masterGain = gain;
        return true;
    }
    float AudioWorld::masterGain() const { return m_state->masterGain; }
    float AudioWorld::categoryGain(InternedString category) const
    {
        const auto it = m_state->categoryGains.find(category);
        return it == m_state->categoryGains.end() ? 1.0f : it->second;
    }
    bool AudioWorld::setCategoryGain(InternedString category, float gain)
    {
        if (!std::isfinite(gain) || gain < 0.0f || gain > 1.0f)
            return false;
        m_state->categoryGains[category] = gain;
        for (auto& [id, voice] : m_state->voices)
            if (voice->initialized && voice->category == category)
            {
                voice->categoryGain = gain;
                ma_sound_set_volume(&voice->sound, voice->volume * gain);
            }
        return true;
    }
    AudioVoiceStatus AudioWorld::status(Entity entity) const
    {
        const auto it = m_state->voices.find(entity.id());
        if (it == m_state->voices.end() || !it->second->initialized)
            return {};
        ma_sound* sound = &it->second->sound;
        return {true, bool(ma_sound_is_playing(sound)), bool(ma_sound_at_end(sound))};
    }
    AudioVoiceDetails AudioWorld::details(Entity entity) const
    {
        const auto it = m_state->voices.find(entity.id());
        if (it == m_state->voices.end() || !it->second->initialized)
            return {};
        const auto& voice = *it->second;
        return {voice.pitch,   voice.minDistance, voice.maxDistance,
                voice.rolloff, voice.category,    voice.volume * voice.categoryGain * m_state->masterGain};
    }
    bool AudioWorld::renderOffline(float* samples, u64 frames)
    {
        if (!m_state->ready || !m_state->offline || !samples)
            return false;
        return ma_engine_read_pcm_frames(&m_state->engine, samples, frames, nullptr) == MA_SUCCESS;
    }
    bool AudioWorld::play(Entity entity)
    {
        if (!m_state->world.isEntityAlive(entity) || !m_state->world.hasComponent<AudioEmitterComponent>(entity))
            return false;
        m_state->world.getComponent<AudioEmitterComponent>(entity).playing = true;
        const auto it = m_state->voices.find(entity.id());
        if (it != m_state->voices.end() && it->second->initialized && ma_sound_at_end(&it->second->sound))
            it->second->replayRequested = true;
        return true;
    }
    bool AudioWorld::pause(Entity entity)
    {
        if (!m_state->world.isEntityAlive(entity) || !m_state->world.hasComponent<AudioEmitterComponent>(entity))
            return false;
        m_state->world.getComponent<AudioEmitterComponent>(entity).playing = false;
        return true;
    }
    bool AudioWorld::stop(Entity entity)
    {
        if (!pause(entity))
            return false;
        const auto it = m_state->voices.find(entity.id());
        if (it != m_state->voices.end() && it->second->initialized)
        {
            it->second->lastPlaying = false;
            ma_sound_stop(&it->second->sound);
            ma_sound_seek_to_pcm_frame(&it->second->sound, 0);
        }
        return true;
    }

    bool AudioWorld::reload(Entity entity)
    {
        if (!m_state->world.isEntityAlive(entity) || !m_state->world.hasComponent<AudioEmitterComponent>(entity))
            return false;
        m_state->voices.erase(entity.id());
        return true;
    }
    void updateAudio(SystemContext& ctx) { ctx.services().write<AudioWorld>()->update(ctx); }
    void AudioWorld::update(SystemContext& ctx)
    {
        auto& s = *m_state;
        if (!s.ready)
            return;
        Entity listener = Entity::undefined();
        ctx.query<Read<AudioListenerComponent>, Read<TransformMatrixComponent>>()->forEachChunkConst(
            [&](const Chunk* chunk)
            {
                const auto listeners = chunk->readColumn<AudioListenerComponent>();
                const auto matrices = chunk->readColumn<TransformMatrixComponent>();
                for (sizet i = 0; i < chunk->size(); ++i)
                {
                    const Entity entity = chunk->entity(i);
                    if (!listeners[i].enabled || (listener.isValid() && entity.id() >= listener.id()))
                        continue;
                    listener = entity;
                    const Mat4& m = matrices[i].matrix;
                    const Vec3 pos(m[3]);
                    const Vec3 forward = -Vec3(m[2]);
                    const Vec3 up(m[1]);
                    ma_engine_listener_set_position(&s.engine, 0, pos.x, pos.y, pos.z);
                    ma_engine_listener_set_direction(&s.engine, 0, forward.x, forward.y, forward.z);
                    ma_engine_listener_set_world_up(&s.engine, 0, up.x, up.y, up.z);
                }
            });
        if (!listener.isValid())
        {
            ma_engine_listener_set_position(&s.engine, 0, 0.0f, 0.0f, 0.0f);
            ma_engine_listener_set_direction(&s.engine, 0, 0.0f, 0.0f, -1.0f);
            ma_engine_listener_set_world_up(&s.engine, 0, 0.0f, 1.0f, 0.0f);
        }
        std::unordered_set<u64> seen;
        ctx.query<Read<AudioEmitterComponent>, Read<TransformMatrixComponent>>()->forEachChunkConst(
            [&](const Chunk* chunk)
            {
                const auto emitters = chunk->readColumn<AudioEmitterComponent>();
                const auto matrices = chunk->readColumn<TransformMatrixComponent>();
                for (sizet i = 0; i < chunk->size(); ++i)
                {
                    const Entity entity = chunk->entity(i);
                    const auto& e = emitters[i];
                    seen.insert(entity.id());
                    auto it = s.voices.find(entity.id());
                    if (it != s.voices.end() && (it->second->clip != e.clip || it->second->stream != e.stream))
                    {
                        s.voices.erase(it);
                        it = s.voices.end();
                    }
                    if (it == s.voices.end() && e.clip.isValid())
                    {
                        auto voice = std::make_unique<Voice>();
                        voice->clip = e.clip;
                        voice->stream = e.stream;
                        std::filesystem::path relative;
                        try
                        {
                            relative = pathFromUtf8(e.clip.toView());
                        }
                        catch (const std::system_error&)
                        {
                            SWARNING_LOG("audio: invalid UTF-8 clip path");
                            s.voices.emplace(entity.id(), std::move(voice));
                            continue;
                        }
                        bool safe = !relative.has_root_path() && !relative.empty();
                        for (const auto& part : relative)
                            if (part == ".." || part == ".")
                                safe = false;
                        const auto path = s.root / relative;
                        if (!safe)
                        {
                            SWARNING_LOG("audio: invalid clip path {}", e.clip.toView());
                            s.voices.emplace(entity.id(), std::move(voice));
                            continue;
                        }
                        const ma_uint32 flags = e.stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
#ifdef _WIN32
                        const auto result =
                            ma_sound_init_from_file_w(&s.engine, path.c_str(), flags, nullptr, nullptr, &voice->sound);
#else
                        const auto result =
                            ma_sound_init_from_file(&s.engine, path.c_str(), flags, nullptr, nullptr, &voice->sound);
#endif
                        if (result != MA_SUCCESS)
                        {
                            SWARNING_LOG("audio: cannot load clip {}", e.clip.toView());
                            s.voices.emplace(entity.id(), std::move(voice));
                            continue;
                        }
                        voice->initialized = true;
                        it = s.voices.emplace(entity.id(), std::move(voice)).first;
                    }
                    if (it == s.voices.end() || !it->second->initialized)
                        continue;
                    ma_sound* sound = &it->second->sound;
                    auto& voice = *it->second;
                    const float volume = std::isfinite(e.volume) ? std::clamp(e.volume, 0.0f, 1.0f) : 0.0f;
                    const AudioVoiceSettingsComponent defaults;
                    const auto& settings = s.world.hasComponent<AudioVoiceSettingsComponent>(entity)
                        ? static_cast<const World&>(s.world).getComponent<AudioVoiceSettingsComponent>(entity)
                        : defaults;
                    const auto category = settings.category;
                    if (voice.volume < 0.0f || voice.category != category)
                    {
                        voice.category = category;
                        voice.categoryGain = categoryGain(category);
                        voice.volume = volume;
                        ma_sound_set_volume(sound, volume * voice.categoryGain);
                    }
                    else if (voice.volume != volume)
                    {
                        voice.volume = volume;
                        ma_sound_set_volume(sound, volume * voice.categoryGain);
                    }
                    ma_sound_set_looping(sound, e.loop);
                    ma_sound_set_spatialization_enabled(sound, e.spatial);
                    const Vec3 pos(matrices[i].matrix[3]);
                    ma_sound_set_position(sound, pos.x, pos.y, pos.z);
                    const float pitch = std::isfinite(settings.pitch) ? std::clamp(settings.pitch, 0.01f, 16.0f) : 1.0f;
                    const float minDistance =
                        std::isfinite(settings.minDistance) ? std::clamp(settings.minDistance, 0.01f, 1.0e30f) : 1.0f;
                    const float maxDistance = std::isfinite(settings.maxDistance)
                        ? std::max(settings.maxDistance, minDistance)
                        : std::numeric_limits<float>::max();
                    const float rolloff =
                        std::isfinite(settings.rolloff) ? std::clamp(settings.rolloff, 0.0f, 100.0f) : 1.0f;
                    if (voice.pitch != pitch)
                    {
                        ma_sound_set_pitch(sound, pitch);
                        voice.pitch = pitch;
                    }
                    if (voice.minDistance != minDistance)
                    {
                        ma_sound_set_min_distance(sound, minDistance);
                        voice.minDistance = minDistance;
                    }
                    if (voice.maxDistance != maxDistance)
                    {
                        ma_sound_set_max_distance(sound, maxDistance);
                        voice.maxDistance = maxDistance;
                    }
                    if (voice.rolloff != rolloff)
                    {
                        ma_sound_set_rolloff(sound, rolloff);
                        voice.rolloff = rolloff;
                    }
                    if (e.playing && !voice.lastPlaying && ma_sound_at_end(sound))
                        voice.replayRequested = true;
                    voice.lastPlaying = e.playing;
                    if (e.playing && !ctx.simulationPaused && !ma_sound_is_playing(sound) &&
                        (!ma_sound_at_end(sound) || voice.replayRequested))
                    {
                        if (ma_sound_start(sound) == MA_SUCCESS)
                            voice.replayRequested = false;
                    }
                    else if ((!e.playing || ctx.simulationPaused) && ma_sound_is_playing(sound))
                        ma_sound_stop(sound);
                }
            });
        std::erase_if(s.voices, [&](const auto& pair) { return !seen.contains(pair.first); });
    }
} // namespace spite
