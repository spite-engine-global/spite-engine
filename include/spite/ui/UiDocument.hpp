#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/ImageRange.hpp"
#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiExport.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class UiContext;

    // UI documents: code declares which elements a view has by id and the behaviour attached to each
    // (UiViewContract); a JSON document builds or enhances the tree (structure, layout, style classes, rect, text and
    // localization keys, image slots, value widget state, bindings), may add elements the code never names and is
    // re-applied by id-path diff, so an unchanged element keeps its entity and with it focus, scroll, text input and
    // tween progress. Documents are untrusted input: they are refused whole (the tree stays as it was) when over a
    // limit below, malformed, not UTF-8, holding a duplicate key, an unknown kind or a wrong value type, or missing an
    // id the contract requires; unknown keys and unresolvable bindings are reported and ignored. An element's identity
    // is its id path from the root ("hud/bar/hit"); siblings sharing an id are told apart by occurrence ("row",
    // "row#1"), so an id holds no '/' or '#'.
    //
    // Format: {"version": 1, "root": <element>}, an element being {"id", "kind", "class", "anchor", "offset", "size",
    // "pivot", "z", "layout", "item", "fit", "visible", "interactable", "clip", "text", "textKey", "textArgs", "rich",
    // "icon", "image", "checked", "value", "min", "max", "step", "contentSize", "bind", "opaque", "children"};
    // "UI document" lists the value shapes. An absent key is the kind's default value. A number of "layout",
    // "item" or "fit" may be a "$name" layout token (an array: one for every entry, or per entry), bound
    // through UiLayoutTokensComponent; a name the context's token table lacks loads and leaves the field at its
    // default. An "icon" names a Phosphor icon (UiIcons.hpp); a name the table lacks refuses the document.
    inline constexpr u32 UI_DOCUMENT_VERSION = 1;
    inline constexpr sizet UI_DOCUMENT_MAX_BYTES = sizet{4} << 20;
    inline constexpr u32 UI_DOCUMENT_MAX_DEPTH = 64; // element nesting
    inline constexpr u32 UI_DOCUMENT_MAX_NODES = 65536;
    inline constexpr sizet UI_DOCUMENT_MAX_ID = 128;
    inline constexpr sizet UI_DOCUMENT_MAX_TEXT = 16384;
    // An element's id path ("hud/bar/hit") and all paths together (each kept per element, twice): a deep tree of long
    // ids is refused instead of costing hundreds of MB.
    inline constexpr sizet UI_DOCUMENT_MAX_PATH = 1024;
    inline constexpr sizet UI_DOCUMENT_MAX_PATH_BYTES = sizet{8} << 20;
    // Lists in a report (problems, document-only ids) stop here; their totals stay exact.
    inline constexpr u32 UI_DOCUMENT_REPORT_CAP = 200;

    // One finding of a document parse or apply. `path` is the element's id path ("root/bar/hit") or the JSON place;
    // an error refused the document, a warning was ignored.
    struct UiDocumentProblem
    {
        std::string path;
        std::string message;
        bool error = false;
    };

    // What the last apply of a view did; `nodes` is the applied document's element count.
    struct UiDocumentStats
    {
        u64 applies = 0;
        u64 refused = 0;
        u32 nodes = 0;
        u32 created = 0;
        u32 updated = 0; // existing elements with at least one field written
        u32 unchanged = 0;
        u32 destroyed = 0;
        u32 fieldsWritten = 0;
        u64 keyChecks = 0; // duplicate-key comparisons and hash probes of the last read, refused or not
        double parseMs = 0.0;
        double applyMs = 0.0;
    };

    // The code side of a view: the element ids it needs and what it attaches to each, and the entities a document's
    // bindings may name. A document cannot add behaviour the contract does not declare.
    class SPITE_UI_API UiViewContract
    {
    public:
        // Runs whenever the id binds a new element (the first build, or a document that recreated it), after the
        // whole document applied: attach reactions and keep handles here. It may destroy widgets or the view.
        using AttachFn = std::function<void(const Widget&)>;
        // Builds the view's elements in the view's scope under its root when no document applies.
        using BuildFn = std::function<void(UiScope& scope, const Widget& root)>;

        struct Element
        {
            InternedString id{};
            UiWidgetKind kind = UiWidgetKind::Panel;
            bool required = true;
            bool slot = false;
            AttachFn attach;
        };

        // A document without an element of this id and kind is refused (reported).
        UiViewContract& require(InternedString id, UiWidgetKind kind, AttachFn attach = {});
        UiViewContract& optional(InternedString id, UiWidgetKind kind, AttachFn attach = {});
        // A slot: a required panel the document owns and places anywhere in its tree, whose children this
        // code builds and owns (a document lists none, an apply never touches them). It is matched by id, not id
        // path, so a document that moves it keeps its entity and content; one id per document. `fill` runs after an
        // apply whenever the id binds a new element or its UiSlotComponent fields (scroll, childClass) changed: parent
        // the content to the slot there and honour the fields.
        UiViewContract& slot(InternedString id, AttachFn fill);
        // An entity a document's "bind" may follow, by `name` ("bind": {"source": name, ...}).
        UiViewContract& source(std::string_view name, Entity entity);
        UiViewContract& fallback(BuildFn build);

        [[nodiscard]] std::span<const Element> elements() const { return m_elements; }
        [[nodiscard]] const Element* find(InternedString id) const;
        [[nodiscard]] Entity sourceOf(std::string_view name) const;
        [[nodiscard]] const BuildFn& fallbackBuild() const { return m_fallback; }
        [[nodiscard]] u32 countCallbacksIn(const ImageRange& image) const;

    private:
        std::vector<Element> m_elements;
        std::vector<std::pair<std::string, Entity>> m_sources;
        BuildFn m_fallback;
    };

    class UiDocuments;
    struct UiDocumentModel; // a parsed document (UiDocument.cpp)

    // One view built from documents: a scope owning the elements documents (or the fallback) created, the contract,
    // and the last applied document. Elements of other scopes under the root (a code-built tree a document enhances,
    // uiImport) are matched by id and written, never destroyed. Registered with the context's documents() under its
    // name. Apply only at the frame point (between frames, or in the control pump): it creates, writes and destroys
    // elements and runs the contract's attach functions.
    class SPITE_UI_API UiDocumentView
    {
    public:
        // The root element the documents build goes under `parent` (a root surface when undefined).
        UiDocumentView(UiContext& context, std::string name, UiViewContract contract,
                       Entity parent = Entity::undefined());
        ~UiDocumentView();
        UiDocumentView(const UiDocumentView&) = delete;
        UiDocumentView& operator=(const UiDocumentView&) = delete;

        // Reads and applies the file (at most UI_DOCUMENT_MAX_BYTES) and watches it from then on. A refused or
        // unreadable first document runs the contract's fallback build when the view has no tree yet. Whether it
        // applied.
        bool load(const std::filesystem::path& path);
        // Applies a document text by id-path diff; a refused one leaves the tree as it was (report() says why).
        bool apply(std::string_view text);
        // The contract's imperative build (clears the document); false without one.
        bool buildFallback();

        // File watch of the loaded path: poll (throttled to WATCH_INTERVAL; call at the frame point) re-applies the
        // file once its time stamp and size held for one interval. Whether it applied.
        static constexpr std::chrono::milliseconds WATCH_INTERVAL{250};
        void watch(bool enabled) { m_watch = enabled; }
        [[nodiscard]] bool watching() const { return m_watch; }
        bool poll();

        // The live tree under the root as a document (exportUiDocument).
        [[nodiscard]] std::string exportText() const;

        [[nodiscard]] const std::string& name() const { return m_name; }
        [[nodiscard]] const std::filesystem::path& path() const { return m_path; }
        [[nodiscard]] Entity root() const { return m_root; }
        [[nodiscard]] UiScope& scope() { return *m_scope; }
        [[nodiscard]] const UiViewContract& contract() const { return m_contract; }
        // The text of the document the tree was last built from; empty after a fallback build.
        [[nodiscard]] const std::string& documentText() const { return m_text; }
        // The element a contract id bound, invalid when none.
        [[nodiscard]] Widget element(InternedString id) const;

        // The last apply's findings, the contract ids no element answers (code only) and the document ids the
        // contract does not name (document only: decorative, or a typo), each capped at UI_DOCUMENT_REPORT_CAP.
        [[nodiscard]] std::span<const UiDocumentProblem> problems() const { return m_problems; }
        [[nodiscard]] std::span<const InternedString> codeOnly() const { return m_codeOnly; }
        [[nodiscard]] std::span<const std::string> documentOnly() const { return m_documentOnly; }
        [[nodiscard]] u32 documentOnlyCount() const { return m_documentOnlyCount; }
        [[nodiscard]] const UiDocumentStats& stats() const { return m_stats; }
        [[nodiscard]] bool adopted() const { return m_adopted; }
        // Whether the root still is a live element (an adopted view's code-built root may be destroyed under it).
        [[nodiscard]] bool rootAlive() const;

    private:
        friend class UiDocuments;
        struct Live;
        // Adopts a code-built root (UiDocuments::adopt).
        UiDocumentView(UiContext& context, Entity root);

        bool applyModel(std::unique_ptr<UiDocumentModel> model, std::string text, double parseMs,
                        std::vector<UiDocumentProblem> problems);
        void resolveContract(std::vector<std::pair<UiViewContract::AttachFn, Widget>>& attach);
        void runAttach(std::vector<std::pair<UiViewContract::AttachFn, Widget>>& attach);
        void refuse(std::vector<UiDocumentProblem> problems);

        UiContext* m_context = nullptr; // nulled when the context dies first
        std::string m_name;
        UiViewContract m_contract;
        Entity m_parent = Entity::undefined();
        Entity m_root = Entity::undefined();
        bool m_adopted = false;
        bool m_fallbackTree = false; // the tree under the root is the fallback build's, untracked
        std::unique_ptr<UiScope> m_scope;
        std::unique_ptr<UiDocumentModel> m_model;
        std::string m_text;
        std::unordered_map<std::string, std::unique_ptr<Live>> m_live; // by id path
        std::vector<Live*> m_placed; // by m_model node: the tracked element of each, null when none
        std::unordered_map<InternedString, Entity> m_bound; // contract id -> the element it bound
        std::unordered_map<InternedString, UiSlotComponent> m_slotFields; // contract slot -> the fields fill last saw
        std::vector<UiDocumentProblem> m_problems;
        std::vector<InternedString> m_codeOnly;
        std::vector<std::string> m_documentOnly;
        u32 m_documentOnlyCount = 0;
        UiDocumentStats m_stats;
        std::filesystem::path m_path;
        std::filesystem::directory_entry m_entry; // of m_path: refreshed by poll without allocating
        std::filesystem::file_time_type m_stamp{};
        u64 m_size = 0;
        bool m_watch = false;
        bool m_pending = false; // a change seen, waiting one interval to settle
        bool m_applying = false;
        std::chrono::steady_clock::time_point m_lastPoll{};
        std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    };

    // The document views of one UI (UiContext::documents()).
    class SPITE_UI_API UiDocuments
    {
    public:
        explicit UiDocuments(UiContext& context) : m_context(&context) {}
        ~UiDocuments();
        UiDocuments(const UiDocuments&) = delete;
        UiDocuments& operator=(const UiDocuments&) = delete;

        [[nodiscard]] std::span<UiDocumentView* const> views() const { return m_views; }
        // The view of that name; an adopted root's view is named "@<entity id>".
        [[nodiscard]] UiDocumentView* find(std::string_view name) const;
        // The view over a code-built element (what uiImport applies to), made on first use and owned here: documents
        // write its elements and add their own, never destroying the code's.
        UiDocumentView& adopt(Entity root);
        // Polls every watched view (the frame point); how many re-applied.
        u32 poll();
        [[nodiscard]] u32 countCallbacksIn(const ImageRange& image) const;

    private:
        friend class UiDocumentView;
        friend class UiContext;
        UiContext* m_context;
        std::vector<UiDocumentView*> m_views;
        std::vector<std::unique_ptr<UiDocumentView>> m_adopted;
    };

    // Reads a document file, never more than UI_DOCUMENT_MAX_BYTES + 1 bytes (a file that grows while read is
    // refused, not read whole). False plus `error` when unreadable or over the limit.
    [[nodiscard]] SPITE_UI_API bool readUiDocumentFile(const std::filesystem::path& path, std::string& text,
                                                       std::string& error);

    struct UiCompositionView
    {
        std::string name, path, parent, slot;
        std::vector<std::string> required;
        bool fillWidth = false;
        bool previewVisible = true;
    };

    struct UiCompositionAction
    {
        std::string view, id, event, command, args;
    };

    // A module's authoring entry point. Views are listed in mount order; actions are declarations
    // for inspection and for callbacks that a module explicitly supports.
    struct UiComposition
    {
        std::string name, baseStyle, theme, style;
        Vec2 screen{};
        std::vector<UiCompositionView> views;
        std::vector<UiCompositionAction> actions;

        [[nodiscard]] SPITE_UI_API std::vector<std::string> protectedIds(std::string_view view) const;
    };

    [[nodiscard]] SPITE_UI_API Expected<UiComposition> readUiComposition(const std::filesystem::path& path);

    // The live tree under `root` as a document: every element by id with its kind and the fields that differ from
    // the kind's defaults; the parts of a composite widget are not listed, and an element a document cannot build
    // (a dock, table, tree, menu, canvas, dropdown...) is written "opaque": a document keeps it and may restyle it.
    // Empty when the root is no element with an id, or when the tree could not be read back (an id with '/', '#' or
    // a control character, more than the document limits): then `problem`, when given, says why. Text that is no UTF-8
    // is written with replacement characters.
    [[nodiscard]] SPITE_UI_API std::string exportUiDocument(const UiContext& context, Entity root,
                                                            std::string* problem = nullptr);
} // namespace spite
