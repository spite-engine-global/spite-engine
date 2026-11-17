#include <algorithm>
#include <cmath>
#include <memory>
#include "base/ContentRoot.hpp"
#include "ui/UiScope.hpp"
#include "uiview/UiViewContext.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "audio/AudioWorld.hpp"
#include "renderruntime/LightComponents.hpp"
#include <stdexcept>
#include "engine/Module.hpp"
#include "ecs/Query.hpp"
#include "application/input/InputSystem.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    struct ClockworkState : IComponent
    {
        int score = 0;
        int goal = 3;
        bool paused = false;
        bool won = false;
        float flash = 0.0f;
        bool audioStarted = false;
        SPITE_SCHEMA(ClockworkState, score, goal, paused, won, flash, audioStarted)
    };
    struct Salvage : IComponent
    {
        float startX = -4.0f;
        float speed = 1.0f;
        SPITE_SCHEMA(Salvage, startX, speed)
    };
} // namespace spite
namespace
{
    using namespace spite;
    struct Hud
    {
        std::unique_ptr<UiScope> scope;
    };
    void tick(SystemContext& context)
    {
        auto& services = context.services();
        if (!services.contains<InputSystem>())
            return;
        auto input = services.read<InputSystem>();
        auto players = context.query<Write<ClockworkState>, Write<TransformComponent>>();
        auto cargo = context.query<Read<Salvage>, Write<TransformComponent>>();
        for (auto [entity, state, player] : players->view<Entity, Write<ClockworkState>, Write<TransformComponent>>())
        {
            if (input->wasActionPressedThisFrame(InternedString("clock_restart")) ||
                input->wasActionPressedThisFrame(InternedString("clock_confirm")))
            {
                state.score = 0;
                state.won = false;
                state.paused = false;
                state.flash = 0;
                state.audioStarted = false;
                player.position = {-4.0f, 1.0f, 1.0f};
                for (auto [item, transform] : cargo->view<Read<Salvage>, Write<TransformComponent>>())
                    transform.position.x = item.startX;
            }
            if (input->wasActionPressedThisFrame(InternedString("clock_pause")))
                state.paused = !state.paused;
            const float dt = std::min(context.deltaTime, 0.1f);
            if (!state.paused)
                state.flash = std::max(0.0f, state.flash - dt);
            if (auto* light = context.tryGetComponent<PointLightComponent>(entity))
                light->intensity = state.flash * 30.0f;
            if (state.paused || state.won)
                continue;
            player.position.x = std::clamp(player.position.x +
                                               4.0f * dt *
                                                   (input->getActionValue(InternedString("clock_right")) -
                                                    input->getActionValue(InternedString("clock_left"))),
                                           -6.0f, 6.0f);
            player.position.z = std::clamp(player.position.z +
                                               4.0f * dt *
                                                   (input->getActionValue(InternedString("clock_down")) -
                                                    input->getActionValue(InternedString("clock_up"))),
                                           -1.0f, 5.0f);
            const bool collect = input->wasActionPressedThisFrame(InternedString("clock_collect"));
            for (auto [item, transform] : cargo->view<Read<Salvage>, Write<TransformComponent>>())
            {
                transform.position.x += item.speed * dt;
                if (transform.position.x > 6.0f)
                    transform.position.x = item.startX;
                const float dx = player.position.x - transform.position.x;
                const float dz = player.position.z - transform.position.z;
                if (collect && dx * dx + dz * dz < 2.25f)
                {
                    ++state.score;
                    state.flash = 0.4f;
                    if (services.contains<AudioWorld>())
                        state.audioStarted = services.write<AudioWorld>()->play(entity);
                    transform.position.x = item.startX;
                    if (state.score >= state.goal)
                        state.won = true;
                    break;
                }
            }
        }
    }
    bool load(const ModuleContext& context)
    {
        registerComponent<ClockworkState>(context.owner.id());
        registerComponent<Salvage>(context.owner.id());
        if (context.services.contains<InputSystem>())
        {
            auto input = context.services.write<InputSystem>();
            input->mapActionToKey(InternedString("clock_left"), KeyCode::A, InputActionType::Axis);
            input->mapActionToKey(InternedString("clock_right"), KeyCode::D, InputActionType::Axis);
            input->mapActionToKey(InternedString("clock_up"), KeyCode::W, InputActionType::Axis);
            input->mapActionToKey(InternedString("clock_down"), KeyCode::S, InputActionType::Axis);
            input->mapActionToKey(InternedString("clock_collect"), KeyCode::SPACE, InputActionType::Press);
            input->mapActionToKey(InternedString("clock_pause"), KeyCode::P, InputActionType::Press);
            input->mapActionToKey(InternedString("clock_restart"), KeyCode::R, InputActionType::Press);
            input->mapActionToKey(InternedString("clock_confirm"), KeyCode::RETURN, InputActionType::Press);
        }
        return true;
    }
    void unload(const ModuleContext& context)
    {
        if (!context.services.contains<InputSystem>())
            return;
        auto input = context.services.write<InputSystem>();
        for (auto name : {"clock_left", "clock_right", "clock_up", "clock_down", "clock_collect", "clock_pause",
                          "clock_restart", "clock_confirm"})
            input->unmapAction(InternedString(name));
    }
    void attachWorld(const ModuleContext& context, World world)
    {
        SystemDescriptor system;
        system.moduleName = "SalvageGame";
        system.name = "ClockworkSalvage";
        system.fn = tick;
        system.update =
            SystemPhaseDescriptor{.reads = {getComponentId<Salvage>()},
                                  .writes = {getComponentId<ClockworkState>(), getComponentId<TransformComponent>(),
                                             getComponentId<PointLightComponent>()}};
        world.registerSystem(system);
        auto& services = world.services();
        if (services.contains<UiContext>() && services.contains<UiViewContext>())
        {
            auto& ui = services.get<UiContext>();
            auto& view = services.get<UiViewContext>();
            const auto font =
                view.registerFont(InternedString("clockwork"), resolveContentPath("assets/fonts/Geist-Regular.ttf"));
            if (!font)
                throw std::runtime_error(font.error().message);
            const auto style = view.stylesheet().loadLayerText(
                InternedString("clockwork"),
                R"({"defaults":{"text":{"color":[1,1,1,1],"size":16,"font":"clockwork"}},"elements":{"hudPanel":{"background":{"color":[0.04,0.04,0.04,0.85]}}}})");
            if (!style)
                throw std::runtime_error(style.error().message);
            auto hud = std::make_shared<Hud>();
            hud->scope = std::make_unique<UiScope>(ui);
            UiRectDesc panelRect;
            panelRect.anchor = {0, 0};
            panelRect.pivot = {0, 0};
            panelRect.offset = {8, 5};
            panelRect.size = {620, 135};
            panelRect.zOffset = -1;
            hud->scope->createPanel(InternedString("hudPanel"), panelRect);
            UiRectDesc titleRect;
            titleRect.anchor = {0, 0};
            titleRect.pivot = {0, 0};
            titleRect.offset = {18, 15};
            titleRect.size = {480, 34};
            hud->scope->createLabel(InternedString("clock_title"), "CLOCKWORK SALVAGE   |   score / 3", titleRect);
            UiRectDesc scoreRect = titleRect;
            scoreRect.offset = {18, 50};
            scoreRect.size = {100, 30};
            auto query = world.createQueryBuilder().buildQuery<Read<ClockworkState>>();
            std::string initialScore = "0";
            for (const auto& state : query->view<Read<ClockworkState>>())
            {
                initialScore = std::to_string(state.score);
                break;
            }
            auto score = hud->scope->createLabel(InternedString("clock_score"), initialScore, scoreRect);
            UiRectDesc helpRect = titleRect;
            helpRect.offset = {18, 82};
            helpRect.size = {600, 30};
            hud->scope->createLabel(InternedString("clock_help"), "WASD move   SPACE collect   P pause   R restart",
                                    helpRect);
            UiRectDesc statusRect = titleRect;
            statusRect.offset = {18, 102};
            statusRect.size = {300, 30};
            auto won =
                hud->scope->createLabel(InternedString("clock_won"), "SHIFT COMPLETE!  Press R to restart", statusRect);
            auto paused = hud->scope->createLabel(InternedString("clock_paused"), "PAUSED", statusRect);
            hud->scope->commit();
            for (auto [entity, state] : query->view<Entity, Read<ClockworkState>>())
            {
                (void)state;
                const auto scoreBinding = ui.bindings().add(
                    *hud->scope, score.entity(), {.source = entity, .component = "ClockworkState", .field = "score"});
                if (!scoreBinding)
                    throw std::runtime_error(scoreBinding.error().message);
                const auto wonBinding = ui.bindings().add(
                    *hud->scope, won.entity(),
                    {.source = entity, .component = "ClockworkState", .field = "won", .target = UiBindTarget::Visible});
                if (!wonBinding)
                    throw std::runtime_error(wonBinding.error().message);
                const auto pausedBinding = ui.bindings().add(*hud->scope, paused.entity(),
                                                             {.source = entity,
                                                              .component = "ClockworkState",
                                                              .field = "paused",
                                                              .target = UiBindTarget::Visible});
                if (!pausedBinding)
                    throw std::runtime_error(pausedBinding.error().message);
                break;
            }
            services.registerService(hud, context.owner);
        }
    }
} // namespace
SPITE_MODULE_EXPORT const spite::ModuleDescriptor* SPITE_MODULE_ENTRY()
{
    static const spite::ModuleDescriptor descriptor{
        .name = "SalvageGame", .load = load, .unload = unload, .attachWorld = attachWorld};
    return &descriptor;
}
