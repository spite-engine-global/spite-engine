#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "base/ApplicationConfig.hpp"
#include "base/ServiceLocator.hpp"
#include "ecs/World.hpp"
#include "engine/EngineExport.hpp"

namespace spite
{
    class UiContext;
    class UiViewContext;
    class UiTextureResolver;

    // A UI-only world a tool builds its UI in (Engine::addUiWorld): its own world services under the
    // engine's root, one worker, a UiContext and its view. The engine runs it before the scene world, draws its UI onto
    // the swapchain over the game UI, routes input to it before the game, serves it over control under its name and
    // resolves its UI textures. Owned by the engine; a game run creates none.
    class SPITE_ENGINE_API UiWorld
    {
    public:
        ~UiWorld();
        UiWorld(const UiWorld&) = delete;
        UiWorld& operator=(const UiWorld&) = delete;

        [[nodiscard]] const std::string& name() const { return m_name; }
        [[nodiscard]] World& world() { return m_world; }
        [[nodiscard]] ServiceLocator& services() { return m_services; }
        [[nodiscard]] UiContext& ui() { return *m_ui; }
        [[nodiscard]] UiViewContext& uiView() { return *m_uiView; }

        // A dock panel of this UI that shows a render view's target: `screenshot panel=<name>` captures that view's
        // output instead of the panel's part of the presented frame. nullopt unmaps it. A probe capture view is
        // ignored (it has no output): panelView still answers what it did before.
        void setPanelView(std::string_view panel, std::optional<u32> view);
        [[nodiscard]] std::optional<u32> panelView(std::string_view panel) const;

    private:
        friend class Engine;
        UiWorld(std::string name, ServiceLocator& root, const GraphicsConfig& graphics);

        std::string m_name;
        ServiceLocator m_services;
        const GraphicsConfig& m_graphics;
        World m_world;
        std::unique_ptr<UiContext> m_ui;
        std::unique_ptr<UiViewContext> m_uiView;
        std::unique_ptr<UiTextureResolver> m_textures;
        std::unordered_map<std::string, u32> m_panelViews;
    };
} // namespace spite
