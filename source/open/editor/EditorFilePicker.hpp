#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "EditorDialogs.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class ControlServer;
    class IFormatWriter;
    class Localization;
    class UiScope;

    // The content of EditorDialogs' file dialog: a location bar (parent folder, the roots and recent
    // folders, breadcrumb), the folder's entries as a FilteredTable (name, type, size, modified), a name field and a
    // status line, built under the dialog's host panel; the dialog's confirm button is the caller's. It lists
    // folders through ControlServer::listDirectory, so a person browses what listDirectory shows an LLM, and every
    // choice is validated by resolveChoice, a path relative to a named root.
    //
    // Main thread, shown for one request at a time. Navigation runs from update() (never inside the click or
    // activation that asked), so the table is not rebuilt inside its own handler.
    class FilePicker
    {
    public:
        struct Hooks
        {
            std::function<void()> confirm; // Enter in the name field, a file activated in an open dialog
            std::function<void()> cancel; // Esc in the name field
        };

        FilePicker(UiScope& scope, ControlServer& control, const Localization* localization, const Widget& host,
                   const Button& confirm, Hooks hooks);
        ~FilePicker();
        FilePicker(const FilePicker&) = delete;
        FilePicker& operator=(const FilePicker&) = delete;

        // Starts the dialog for a request: location, name, filters, confirm label. rowsHeight is the table's.
        void open(const EditorPick& request, float rowsHeight);
        // Per frame while shown: navigation, the table's pending commands, a locale change.
        void update();

        // Goes to the folder "<root>/<directory>" now (the pickerGo command); the refusal, empty on success.
        [[nodiscard]] std::string navigate(std::string_view rooted);
        // What the dialog shows as a choice ("<root>/<path>", the root alone for the root folder), or why it is not
        // one (also shown in the dialog). Empty error: encoded is set.
        [[nodiscard]] std::string current(std::string& encoded);
        // A choice named by text (answerDialog path=) against the open request.
        [[nodiscard]] std::string choose(std::string_view rooted, std::string& encoded);
        // What a request answers without a person: its starting folder and name, validated as any choice; a save
        // never lands on an existing file (only a person's or a command's explicit choice replaces).
        [[nodiscard]] std::string autoChoice(const EditorPick& request, std::string& encoded) const;
        // A choice was taken: its folder joins the recent folders.
        void remember(std::string_view encoded);
        // The element that takes the focus when the dialog opens.
        [[nodiscard]] Entity initialFocus() const;
        // The open dialog's state for the dialogs query (members of an object the caller began).
        void write(IFormatWriter& out) const;
        // A queued request's description (members of an object the caller began).
        static void writeRequest(const EditorPick& request, IFormatWriter& out);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace spite
