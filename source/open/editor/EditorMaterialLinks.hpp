#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;
    class UiScope;
    class EditorMaterialLinks
    {
    public:
        EditorMaterialLinks(Engine&, UiScope&, std::string_view id, u64 imageSlotBase);
        ~EditorMaterialLinks();
        Widget root() const;
        void setMaterials(const std::vector<std::string>& names);
        void onOpen(std::function<void(std::string_view)> callback);
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
