#pragma once

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "EditorPanelUi.hpp"
#include "ui/UiForm.hpp"

namespace spite
{
    class IFormatWriter;
    struct BakedMaterialWriteData;

    // An entity bound to a material, as the Edit tab lists it.
    struct ProjectMaterialUser
    {
        u64 id = 0;
        std::string name;
        bool operator==(const ProjectMaterialUser&) const = default;
    };

    // What the Project panel's material Edit tab shows of the selected material: its fields as the
    // descriptor holds them and the models and entities that use it.
    struct ProjectMaterialModel
    {
        static constexpr sizet MAX_USERS = 32; // the entities listed (and reported); entityCount is the whole

        bool shown = false; // a material of the manifest is selected
        std::string name;
        std::string error; // the descriptor cannot be read
        std::vector<UiFormValue> values; // the descriptor as the form's rows show it: materialValues()
        std::vector<std::string> models; // the models that list the material
        std::vector<ProjectMaterialUser> entities;
        u32 entityCount = 0;
        bool scanned =
            false; // the entities were read: only while the Edit tab shows, else entities and count are empty
        bool blocked = false; // removing it is refused: a model or an entity holds it
        bool operator==(const ProjectMaterialModel&) const = default;
    };

    // The form's rows are BakedMaterialWriteData's schema fields, in declaration order, one per field of a supported
    // kind (bool, number, enum, vector, colour, interned string): a number as a number row over the schema's range, an
    // enum as a choice of its names, a COLOR vector as a swatch and its channels, an InternedString as a texture name
    // with a Pick list of the manifest's textures. Labels and docs are the keys
    // "editor-project.material.<field>[.doc]". A field added to the schema is a row with no change here (and two keys
    // in assets/loc/en/editor/project.json). materialValues is what a descriptor shows: one value per row, in the rows'
    // order.
    [[nodiscard]] std::vector<UiFormValue> materialValues(const BakedMaterialWriteData& data);

    // The tab itself: the schema's rows as a property form, Duplicate, and who uses the material. A thin client: an
    // edit is updateMaterial by the material's name, sent through `send`; the form follows what show() is given, so a
    // refused edit, an undo or an LLM's edit all show, and nothing here holds a field the descriptor does not.
    class ProjectMaterialView
    {
    public:
        using Tr = std::function<std::string(std::string_view key, std::span<const MessageArg> args)>;
        // A command line, the toast key of its refusal (null: none) and what it calls with the refusal's text.
        using Send = std::function<void(std::string line, const char* failedKey,
                                        std::function<void(const std::string& error)> refused)>;

        ProjectMaterialView(UiScope& scope, panelui::DensityFollower& density, Tr tr, Send send,
                            std::function<std::vector<std::string>()> textures, std::function<void()> duplicate);
        ~ProjectMaterialView();
        ProjectMaterialView(const ProjectMaterialView&) = delete;
        ProjectMaterialView& operator=(const ProjectMaterialView&) = delete;

        [[nodiscard]] Widget root() const;
        // Writes the widgets that differ from the model; the form's rows being edited are left alone.
        void show(const ProjectMaterialModel& model);
        // The texts were written for another locale: show() writes them again.
        void relocalize();
        // The scroll content fits its rows; writes only on a change. Call while the tab is shown.
        void fit();
        // The end of a pointer gesture ends the undo step a drag folds into; call each frame the tab is shown.
        void tick();
        // Closes the picker and the texture list (the tab is left).
        void close();
        void write(IFormatWriter& out) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace spite
