#include "EditorAnimation.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <nlohmann/json.hpp>

#include "animation/AnimationComponents.hpp"
#include "asset/AssetManifest.hpp"
#include "assetruntime/AssetRuntime.hpp"
#include "control/ControlServer.hpp"
#include "ecs/World.hpp"
#include "engine/Engine.hpp"
#include "serialization/FormatWalk.hpp"
#include "world/EntityName.hpp"
#include "world/EntityTransformManager.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using Binding = AnimationStateComponent::Binding;

        constexpr std::string_view SCRUB_MERGE = "animation.scrub";
        constexpr std::string_view SPEED_MERGE = "animation.speed";
        // a looping clip's end is the pose just short of it: its time would wrap to the start
        constexpr f32 LOOP_END = 1.0f - 1e-4f;
        constexpr u32 MAX_ANCESTORS = 64;

        std::string_view viewOf(const InternedString text)
        {
            return text.isValid() ? text.toView() : std::string_view{};
        }

        // everything the view reads that moves, without a string: equal to the last one means nothing changed
        struct Probe
        {
            Entity animator = Entity::undefined();
            const World* world = nullptr;
            InternedString skeleton;
            InternedString clip;
            u32 joints = 0;
            bool hasPlayer = false;
            Binding binding = Binding::Unbound;
            f32 duration = 0.0f;
            f32 time = 0.0f;
            f32 speed = 1.0f;
            f32 blend = 0.0f;
            bool playing = false;
            bool loop = false;
            bool advancing = true;
            bool bones = false;
            bool selected = false;
            u64 manifest = 0;
            bool operator==(const Probe&) const = default;
        };
    } // namespace

    struct EditorAnimation::State
    {
        State(Engine& engine, const std::string& selectionWorld, const u64& selectionRevision,
              const std::vector<Entity>& selection) :
            engine(&engine), selectionWorld(&selectionWorld), selectionRevision(&selectionRevision),
            selection(&selection)
        {
        }

        Engine* engine;
        const std::string* selectionWorld;
        const u64* selectionRevision;
        const std::vector<Entity>* selection;
        ControlServer* control = nullptr;

        bool bones = false;
        AnimationView current;
        Probe seen;
        bool haveSeen = false;
        u64 revision = 0;

        // the animator's resolution: recomputed when the selection, the shown world or the hierarchy moved
        u64 resolvedSelection = ~0ull;
        const World* resolvedWorld = nullptr;
        u64 resolvedStructure = ~0ull;
        Entity animator = Entity::undefined();

        // the clips of the current skeleton, recomputed when the manifest or the skeleton changed
        InternedString clipsSkeleton;
        u64 clipsManifest = ~0ull;
        std::vector<AnimationClipRow> clips;

        Entity resolve(const World& world)
        {
            EntityTransformManager& transforms = engine->gameTransforms();
            if (*selectionWorld != engine->gameWorldName())
            {
                return Entity::undefined();
            }
            for (const Entity picked : *selection)
            {
                Entity at = picked;
                for (u32 depth = 0; depth < MAX_ANCESTORS && at.isValid() && world.isEntityAlive(at); ++depth)
                {
                    if (world.hasComponent<SkinComponent>(at))
                    {
                        return at;
                    }
                    at = transforms.parentOf(at);
                }
            }
            return Entity::undefined();
        }

        Entity animatorOf(const World& world)
        {
            EntityTransformManager& transforms = engine->gameTransforms();
            const bool keyed = resolvedSelection == *selectionRevision && resolvedWorld == &world &&
                resolvedStructure == transforms.structureGeneration();
            if (keyed &&
                (!animator.isValid() || (world.isEntityAlive(animator) && world.hasComponent<SkinComponent>(animator))))
            {
                return animator;
            }
            transforms.compile();
            resolvedSelection = *selectionRevision;
            resolvedWorld = &world;
            resolvedStructure = transforms.structureGeneration();
            animator = resolve(world);
            return animator;
        }

        Probe read(const World& world, const Entity entity) const
        {
            Probe probe;
            probe.world = &world;
            probe.bones = bones;
            probe.selected = !selection->empty() && *selectionWorld == engine->gameWorldName();
            probe.manifest = engine->assets().manifest().generation();
            probe.animator = entity;
            if (!entity.isValid())
            {
                return probe;
            }
            probe.skeleton = world.getConstComponent<SkinComponent>(entity).skeleton;
            if (const auto* palette = world.tryGetConstComponent<SkinPaletteComponent>(entity))
            {
                probe.joints = palette->jointCount;
            }
            if (const auto* player = world.tryGetConstComponent<AnimationPlayerComponent>(entity))
            {
                probe.hasPlayer = true;
                probe.clip = player->clip;
                probe.time = player->time;
                probe.speed = player->speed;
                probe.playing = player->playing;
                probe.loop = player->loop;
            }
            if (const auto* state = world.tryGetConstComponent<AnimationStateComponent>(entity))
            {
                probe.binding = state->binding;
                probe.duration = state->duration;
                probe.blend = state->blend;
            }
            else
            {
                probe.binding = Binding::Unbound;
            }
            probe.advancing = animatorsAdvance(world);
            return probe;
        }

        void readClips(const Probe& probe)
        {
            if (clipsManifest == probe.manifest && clipsSkeleton == probe.skeleton)
            {
                return;
            }
            clipsManifest = probe.manifest;
            clipsSkeleton = probe.skeleton;
            clips.clear();
            if (!probe.skeleton.isValid())
            {
                return;
            }
            for (const auto& [name, entry] : engine->assets().manifest().getAnimations())
            {
                if (entry.skeleton == probe.skeleton)
                {
                    clips.push_back({std::string(name.toView()), entry.duration});
                }
            }
            std::ranges::sort(clips, {}, &AnimationClipRow::name);
        }

        void refresh()
        {
            const World& world = engine->gameWorld();
            const Probe probe = read(world, animatorOf(world));
            if (haveSeen && probe == seen)
            {
                return;
            }
            haveSeen = true;
            seen = probe;
            readClips(probe);
            AnimationView next;
            next.world = std::string(engine->gameWorldName());
            next.animator = probe.animator;
            next.previewable = next.world == "scene";
            next.bones = bones;
            next.selected = probe.selected;
            if (probe.animator.isValid())
            {
                next.name = std::string(entityName(world, probe.animator));
                next.skeleton = std::string(viewOf(probe.skeleton));
                next.joints = probe.joints;
                next.hasPlayer = probe.hasPlayer;
                next.clip = std::string(viewOf(probe.clip));
                next.binding = !probe.hasPlayer          ? "none"
                    : probe.binding == Binding::Bound    ? "bound"
                    : probe.binding == Binding::Rejected ? "rejected"
                                                         : "unbound";
                next.duration = probe.duration;
                next.time = probe.time;
                next.speed = probe.speed;
                next.blend = probe.blend;
                next.playing = probe.playing;
                next.loop = probe.loop;
                next.advancing = probe.advancing;
                next.clips = clips;
            }
            if (next != current)
            {
                current = std::move(next);
                ++revision;
            }
        }

        std::string line(const std::string_view command, Json args, const bool ofAnimator = true)
        {
            refresh();
            if (!current.hasAnimator())
            {
                return {};
            }
            if (ofAnimator)
            {
                args["id"] = current.animator.id();
            }
            args["world"] = current.world;
            return Json{{"command", command}, {"args", std::move(args)}}.dump();
        }
    };

    EditorAnimation::EditorAnimation(Engine& engine, const std::string& selectionWorld, const u64& selectionRevision,
                                     const std::vector<Entity>& selection) :
        m_state(std::make_unique<State>(engine, selectionWorld, selectionRevision, selection))
    {
        m_state->control = &engine.control();
    }

    EditorAnimation::~EditorAnimation() = default;

    void EditorAnimation::registerCommands(const std::string_view owner)
    {
        State& state = *m_state;
        state.control->registerQuery(
            "animationView", owner,
            "the Animation panel's data: the shown world (scene or play), selected (the selection holds entities of "
            "it), the selected animator (entity, name, skeleton, joints; entity 0 and nothing else when the selection "
            "holds none: it is the first selected entity with a SkinComponent or a descendant of one), its player "
            "(player, clip, binding none|unbound|"
            "bound|rejected, duration, time, normalizedTime, speed, blend, playing, loop), advancing (the world moves "
            "its playheads: the edit world only while previewAnimation is on), previewable (the edit world), bones "
            "(the Scene view draws the skeleton: sceneOverlay bones), which controls act (canPlay, canPause, "
            "canStop, canSeek), clips: the clips baked for its skeleton as {name, duration}, and revision, which moves "
            "with every change of these",
            {},
            [this](const ControlArgs&, IFormatWriter& out)
            {
                const AnimationView& shown = view();
                out.writeString("world", shown.world);
                out.writeBool("selected", shown.selected);
                out.writeU64("entity", shown.animator.isValid() ? shown.animator.id() : 0);
                out.writeString("name", shown.name);
                out.writeString("skeleton", shown.skeleton);
                out.writeU32("joints", shown.joints);
                out.writeBool("player", shown.hasPlayer);
                out.writeString("clip", shown.clip);
                out.writeString("binding", shown.binding);
                out.writeF32("duration", shown.duration);
                out.writeF32("time", shown.time);
                out.writeF32("normalizedTime", shown.normalized());
                out.writeF32("speed", shown.speed);
                out.writeF32("blend", shown.blend);
                out.writeBool("playing", shown.playing);
                out.writeBool("loop", shown.loop);
                out.writeBool("advancing", shown.advancing);
                out.writeBool("previewable", shown.previewable);
                out.writeBool("bones", shown.bones);
                out.writeBool("canPlay", shown.canPlay());
                out.writeBool("canPause", shown.canPause());
                out.writeBool("canStop", shown.canStop());
                out.writeBool("canSeek", shown.canSeek());
                out.writeU64("revision", revision());
                out.beginArray("clips", static_cast<u32>(shown.clips.size()));
                for (const AnimationClipRow& row : shown.clips)
                {
                    out.beginObject({});
                    out.writeString("name", row.name);
                    out.writeF32("duration", row.duration);
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });
    }

    const AnimationView& EditorAnimation::view() const
    {
        m_state->refresh();
        return m_state->current;
    }

    u64 EditorAnimation::revision() const
    {
        m_state->refresh();
        return m_state->revision;
    }

    bool EditorAnimation::bonesShown() const { return m_state->bones; }

    void EditorAnimation::showBones(const bool shown) { m_state->bones = shown; }

    void EditorAnimation::bones(std::vector<AnimatorBone>& out) const
    {
        if (!m_state->bones)
        {
            return;
        }
        if (const AnimationView& shown = view(); shown.hasAnimator())
        {
            animatorBones(m_state->engine->gameWorld(), shown.animator, out);
        }
    }

    std::string EditorAnimation::playLine(const std::string_view clip, const f32 fade) const
    {
        Json args = Json::object();
        if (!clip.empty())
        {
            args["clip"] = clip;
            if (fade > 0.0f)
            {
                args["fade"] = fade;
            }
        }
        return m_state->line("playAnimation", std::move(args));
    }

    std::string EditorAnimation::pauseLine() const { return m_state->line("pauseAnimation", Json::object()); }

    std::string EditorAnimation::stopLine() const { return m_state->line("stopAnimation", Json::object()); }

    std::string EditorAnimation::speedLine(const f32 speed) const
    {
        return m_state->line("setAnimationSpeed", {{"speed", speed}, {"merge", SPEED_MERGE}});
    }

    std::string EditorAnimation::seekLine(const f32 fraction) const
    {
        const AnimationView& shown = view();
        const f32 clamped = std::clamp(fraction, 0.0f, 1.0f);
        const f32 at = shown.loop && clamped >= 1.0f ? LOOP_END : clamped;
        return m_state->line("seekAnimation", {{"time", at * shown.duration}, {"merge", SCRUB_MERGE}});
    }

    std::string EditorAnimation::previewLine(const bool on) const
    {
        return view().previewable ? m_state->line("previewAnimation", {{"on", on}}, false) : std::string{};
    }

    std::string EditorAnimation::bonesLine(const bool on)
    {
        return Json{{"command", "sceneOverlay"}, {"args", {{"name", "bones"}, {"on", on}}}}.dump();
    }
} // namespace spite
