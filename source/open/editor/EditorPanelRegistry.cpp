#include "EditorPanelRegistry.hpp"

#include <algorithm>

#include "base/Assert.hpp"

namespace spite
{
    namespace
    {
        // constructed on first use: registrars in other translation units run before or after any static here
        std::vector<EditorPanelDesc>& table()
        {
            static std::vector<EditorPanelDesc> panels;
            return panels;
        }
    } // namespace

    void registerEditorPanel(EditorPanelDesc desc)
    {
        SASSERTM(!desc.name.empty(), "A panel needs a name.");
        SASSERTM(static_cast<bool>(desc.make) != desc.view.has_value(), "Panel {} needs a builder or a view.",
                 desc.name);
        if (std::ranges::any_of(table(), [&](const EditorPanelDesc& other) { return other.name == desc.name; }))
        {
            SERROR_LOG("Editor panel {} is registered twice; the second registration is ignored", desc.name);
            return;
        }
        table().push_back(std::move(desc));
    }

    bool unregisterEditorPanel(const std::string_view name)
    {
        return std::erase_if(table(), [name](const EditorPanelDesc& panel) { return panel.name == name; }) != 0;
    }

    std::vector<EditorPanelDesc> editorPanels()
    {
        std::vector<EditorPanelDesc> panels = table();
        std::ranges::sort(panels, [](const EditorPanelDesc& a, const EditorPanelDesc& b)
                          { return a.order != b.order ? a.order < b.order : a.name < b.name; });
        return panels;
    }
} // namespace spite
