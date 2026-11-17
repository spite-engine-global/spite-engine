#include "base/WritableLayout.hpp"
#include "EditorMaterialLinks.hpp"
#include "EditorPanelUi.hpp"
#include "EditorRenderedPreviews.hpp"
#include "editor/EditorPreviewJobs.hpp"
#include "engine/Engine.hpp"
#include "assetruntime/AssetRuntime.hpp"
#include "asset/AssetManifest.hpp"
#include "base/io/FileSystem.hpp"
#include "uiview/UiViewContext.hpp"
#include <algorithm>
#include <filesystem>
#include <unordered_map>

namespace spite
{
    using namespace panelui;
    struct EditorMaterialLinks::State
    {
        struct Row
        {
            Button button;
            Image image;
            Label icon, name;
            std::string key;
            std::filesystem::path current, attempted;
            u32 bank = 0;
            bool pending = false, ready = false;
        };
        Engine& engine;
        UiScope& scope;
        u64 base;
        VirtualList list;
        EditorRenderedPreviews lookup;
        std::vector<Row> rows;
        std::vector<std::string> names;
        std::function<void(std::string_view)> open;
        std::unordered_map<std::string, std::filesystem::path> results;
        std::string active;
        u64 revision = 0, generation = 0;
        Density density;
        State(Engine& e, UiScope& s, u64 b) : engine(e), scope(s), base(b), lookup(e), density(Density::read(s)) {}
        u64 slot(u32 row, u32 bank) const { return base + u64(bank) * 0x10000 + row; }
        void clear(u32 i)
        {
            auto& r = rows[i];
            auto& view = editorUi(engine).uiView();
            view.clearImage(slot(i, 0));
            view.clearImage(slot(i, 1));
            r.current.clear();
            r.attempted.clear();
            r.pending = r.ready = false;
            r.bank = 0;
            if (r.image.isValid())
                r.image.setVisible(false);
            if (r.icon.isValid())
                r.icon.setVisible(true);
        }
        void place(Row& r)
        {
            const float p = density.md, icon = density.control;
            const float width = std::max(0.0f, list.size().x - THUMB);
            r.icon.setOffset({p, p});
            r.icon.setSize({icon, icon});
            r.image.setOffset({p, p});
            r.image.setSize({icon, icon});
            r.name.setOffset({p + icon + p, p});
            r.name.setSize({std::max(0.0f, width - (p + icon + p) - p), icon});
        }
    };
    EditorMaterialLinks::EditorMaterialLinks(Engine& engine, UiScope& scope, std::string_view id, u64 base) :
        m_state(std::make_unique<State>(engine, scope, base))
    {
        auto& s = *m_state;
        const float height = s.density.control + 2.0f * s.density.md;
        UiVirtualListDesc desc;
        desc.rect = topLeft({}, {0.0f, height * 3.0f});
        desc.rect.layoutItem = UiLayoutItemComponent{.shrink = 0.0f, .basis = height * 3.0f};
        desc.rowHeight = height;
        desc.createRow = [&s](u32 i) -> Widget
        {
            if (i >= s.rows.size())
                s.rows.resize(i + 1);
            s.clear(i);
            s.rows[i] = {};
            auto& r = s.rows[i];
            r.button = s.scope.createButton("materialLinks.row", "", topLeft());
            r.button.setTooltip(InternedString("editor-project.tip.openMaterialPreview"));
            s.scope.world().getComponent<UiStateComponent>(r.button.entity()).clipsChildren = true;
            r.image = s.scope.createImage("materialLinks.image", topLeft());
            r.image.setParent(r.button);
            r.image.setVisible(false);
            r.image.setInteractable(false);
            r.icon = s.scope.createLabel("materialLinks.icon", "", topLeft());
            r.icon.setIcon("palette");
            r.icon.setParent(r.button);
            r.icon.setInteractable(false);
            r.name = s.scope.createLabel("materialLinks.name", "", topLeft());
            r.name.setStyleClass(InternedString("list.row.text.ellipsis"));
            r.name.setParent(r.button);
            r.name.setInteractable(false);
            s.scope.world().getComponent<UiStateComponent>(r.name.entity()).clipsChildren = true;
            r.button.onClick(
                [&s, i]
                {
                    if (s.open && !s.rows[i].key.empty())
                        s.open(s.rows[i].key);
                });
            return r.button;
        };
        desc.bindRow = [&s](u32 i, u32 row)
        {
            auto& r = s.rows[i];
            if (r.key != s.names[row])
            {
                s.clear(i);
                r.key = s.names[row];
            }
            r.name.setText(r.key);
            r.button.setAccessibleName(r.key);
            s.place(r);
        };
        s.list = scope.createVirtualList(InternedString(id), std::move(desc));
        s.list.setVisible(false);
    }
    EditorMaterialLinks::~EditorMaterialLinks()
    {
        for (u32 i = 0; i < m_state->rows.size(); ++i)
            m_state->clear(i);
        m_state->lookup.cancel();
    }
    Widget EditorMaterialLinks::root() const { return m_state->list; }
    void EditorMaterialLinks::onOpen(std::function<void(std::string_view)> callback)
    {
        m_state->open = std::move(callback);
    }
    void EditorMaterialLinks::setMaterials(const std::vector<std::string>& names)
    {
        auto& s = *m_state;
        if (s.names == names)
            return;
        for (u32 i = 0; i < s.rows.size(); ++i)
        {
            s.clear(i);
            s.rows[i].key.clear();
        }
        s.lookup.cancel();
        s.active.clear();
        s.results.clear();
        s.names = names;
        s.list.setVisible(!names.empty());
        s.list.setRowCount(static_cast<u32>(names.size()));
        s.list.refreshRows();
    }
    void EditorMaterialLinks::update()
    {
        auto& s = *m_state;
        const u64 rev = s.engine.services().contains<EditorPreviewJobs>()
            ? s.engine.services().get<EditorPreviewJobs>().revision()
            : 0;
        const u64 gen = s.engine.assets().manifest().generation();
        if (rev != s.revision || gen != s.generation)
        {
            s.revision = rev;
            s.generation = gen;
            s.lookup.cancel();
            s.active.clear();
            s.results.clear();
            for (auto& r : s.rows)
                r.attempted.clear();
        }
        if (!s.list.isVisible())
            return;
        s.lookup.update();
        if (!s.active.empty())
        {
            const auto status = s.lookup.status();
            if (status == EditorRenderedPreviews::Status::Ready || status == EditorRenderedPreviews::Status::Missing ||
                status == EditorRenderedPreviews::Status::Failed)
            {
                s.results[s.active] =
                    status == EditorRenderedPreviews::Status::Ready ? s.lookup.path() : std::filesystem::path{};
                s.active.clear();
            }
        }
        auto& view = editorUi(s.engine).uiView();
        for (u32 i = 0; i < s.rows.size(); ++i)
        {
            auto& r = s.rows[i];
            if (!r.button.isVisible() || r.key.empty())
            {
                if (r.ready || r.pending)
                    s.clear(i);
                continue;
            }
            s.place(r);
            if (s.active.empty() && !s.results.contains(r.key))
            {
                s.active = r.key;
                s.lookup.requestCachedMaterial(InternedString(std::string_view(r.key)),
                                               (writableDirectory(WritableFolder::Cache, {}, false) / "previews"),
                                               false);
            }
            auto found = s.results.find(r.key);
            if (found != s.results.end() && !found->second.empty() && !r.pending && found->second != r.attempted)
            {
                r.attempted = found->second;
                view.registerImageFile(s.slot(i, 1 - r.bank), utf8Path(found->second));
                r.pending = true;
            }
            if (!r.pending)
                continue;
            const auto info = view.imageInfo(s.slot(i, 1 - r.bank));
            if (!info || info->state == UiImageState::Pending)
                continue;
            r.pending = false;
            if (info->state == UiImageState::Ready && info->width && info->height)
            {
                view.clearImage(s.slot(i, r.bank));
                r.bank = 1 - r.bank;
                r.current = r.attempted;
                r.ready = true;
                r.image.setImageSlot(s.slot(i, r.bank));
                r.image.setVisible(true);
                r.icon.setVisible(false);
            }
            else
                view.clearImage(s.slot(i, 1 - r.bank));
        }
    }
} // namespace spite
