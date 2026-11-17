#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "EditorPanelUi.hpp"
#include "asset/ModelImportSettings.hpp"

namespace spite
{
    class IFormatWriter;

    // What the Project panel's import settings tab shows of the selected model: its sidecar's settings
    // against the ones its bake used, why they cannot be applied, and its newest import job.
    struct ProjectImportModel
    {
        bool shown = false; // a model of the manifest is selected
        std::string name;
        std::string source; // the file it was baked from, as the manifest holds it
        std::string blocked; // "outside": baked from beyond the import root; "gone": its source file is missing
        ModelImportSettings settings; // the sidecar's (the bake's for a blocked "outside")
        ModelImportSettings baked; // what the bake used
        bool sidecarExists = false;
        bool needsReimport = false;
        std::string error; // the sidecar cannot be read
        u32 job = 0; // the model's newest import (0: none) and how it stands
        std::string jobState;
        std::string jobError;
        double bakeMs = 0.0;
        bool operator==(const ProjectImportModel&) const = default;
    };

    // The tab itself: the importer options as a property form, Reimport and Reset, the model's state and its newest
    // job as text lines, and the note that loaded instances keep their geometry. A thin client: an edit, Reset and
    // Reimport are control commands sent through `send` (setImportSettings, reimportModel) and the form follows what
    // show() is given, so nothing here holds a setting the sidecar does not.
    class ProjectImportView
    {
    public:
        using Tr = std::function<std::string(std::string_view key, std::span<const MessageArg> args)>;
        // A command line, the toast key of its refusal (null: none) and what it calls with the refusal's text.
        using Send = std::function<void(std::string line, const char* failedKey,
                                        std::function<void(const std::string& error)> refused)>;

        ProjectImportView(UiScope& scope, panelui::DensityFollower& density, Tr tr, Send send);
        ~ProjectImportView();
        ProjectImportView(const ProjectImportView&) = delete;
        ProjectImportView& operator=(const ProjectImportView&) = delete;

        [[nodiscard]] Widget root() const;
        // Writes the widgets that differ from the model; the form's rows being edited are left alone.
        void show(const ProjectImportModel& model);
        // The texts were written for another locale: show() writes them again.
        void relocalize();
        // The scroll content fits its text; writes only on a change. Call while the tab is shown.
        void fit();
        void write(IFormatWriter& out) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace spite
