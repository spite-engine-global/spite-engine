#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <string>

#include "ecs/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTextMeasurer.hpp"

namespace spite
{
    namespace
    {
        constexpr sizet MAX_UNDO = 128;

        // HID scancodes (the UiKeyTransition domain).
        namespace Key
        {
            constexpr u32 A = 4;
            constexpr u32 C = 6;
            constexpr u32 V = 25;
            constexpr u32 X = 27;
            constexpr u32 Y = 28;
            constexpr u32 Z = 29;
            constexpr u32 RETURN = 40;
            constexpr u32 ESCAPE = 41;
            constexpr u32 BACKSPACE = 42;
            constexpr u32 TAB = 43;
            constexpr u32 F1 = 58;
            constexpr u32 F12 = 69;
            constexpr u32 HOME = 74;
            constexpr u32 DELETE = 76;
            constexpr u32 END = 77;
            constexpr u32 RIGHT = 79;
            constexpr u32 LEFT = 80;
            constexpr u32 KP_ENTER = 88;
        } // namespace Key

        bool isContinuation(const char c) { return (static_cast<u8>(c) & 0xC0u) == 0x80u; }

        // Bytes >= 0x80 count as word characters, so word stops land on
        // UTF-8 boundaries.
        bool isWordChar(const char c)
        {
            const auto byte = static_cast<u8>(c);
            return byte >= 0x80u || std::isalnum(byte) || c == '_';
        }

        u32 snap(const std::string_view text, u32 offset)
        {
            offset = std::min(offset, static_cast<u32>(text.size()));
            while (offset > 0 && offset < text.size() && isContinuation(text[offset]))
            {
                --offset;
            }
            return offset;
        }

        u32 previousBoundary(const std::string_view text, u32 offset)
        {
            while (offset > 0 && isContinuation(text[--offset]))
            {
            }
            return offset;
        }

        u32 nextBoundary(const std::string_view text, u32 offset)
        {
            if (offset >= text.size())
            {
                return static_cast<u32>(text.size());
            }
            ++offset;
            while (offset < text.size() && isContinuation(text[offset]))
            {
                ++offset;
            }
            return offset;
        }

        u32 wordLeft(const std::string_view text, u32 offset)
        {
            while (offset > 0 && !isWordChar(text[offset - 1]))
            {
                --offset;
            }
            while (offset > 0 && isWordChar(text[offset - 1]))
            {
                --offset;
            }
            return offset;
        }

        u32 wordRight(const std::string_view text, u32 offset)
        {
            while (offset < text.size() && isWordChar(text[offset]))
            {
                ++offset;
            }
            while (offset < text.size() && !isWordChar(text[offset]))
            {
                ++offset;
            }
            return offset;
        }

        // Single line: control characters (newlines from a paste) drop.
        std::string printable(const std::string_view text)
        {
            std::string result;
            result.reserve(text.size());
            for (const char c : text)
            {
                if (static_cast<u8>(c) >= 0x20u && c != 0x7F)
                {
                    result += c;
                }
            }
            return result;
        }

        std::optional<double> parseNumber(std::string_view text)
        {
            while (!text.empty() && text.front() == ' ')
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && text.back() == ' ')
            {
                text.remove_suffix(1);
            }
            if (!text.empty() && text.front() == '+')
            {
                text.remove_prefix(1);
            }
            double value = 0.0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
            {
                return std::nullopt;
            }
            return value;
        }

        double conform(const UiNumericComponent& numeric, const double value)
        {
            const double scale = std::pow(10.0, numeric.decimals);
            const double rounded = std::round(std::clamp(value, numeric.min, numeric.max) * scale) / scale;
            return std::clamp(rounded, numeric.min, numeric.max);
        }

        std::string formatNumber(const double value, const u8 decimals)
        {
            std::string text = std::format("{:.{}f}", value, decimals);
            if (decimals > 0)
            {
                text.erase(text.find_last_not_of('0') + 1);
                if (text.back() == '.')
                {
                    text.pop_back();
                }
            }
            return text == "-0" ? "0" : text;
        }
    } // namespace

    // -- TextInput / NumberField ------------------------------------------------

    void TextInput::setText(const std::string_view text)
    {
        if (!isValid())
        {
            return;
        }
        m_scope->world().getComponent<UiValueComponent>(m_entity).text.assign(text); // keeps its capacity
        auto& edit = m_scope->world().getComponent<UiTextEditComponent>(m_entity);
        edit.caret = edit.anchor = static_cast<u32>(text.size());
        m_scope->textReset(m_entity);
        m_scope->markLayoutDirty(m_entity);
    }

    std::string_view TextInput::value() const
    {
        return isValid() ? std::string_view(m_scope->world().getConstComponent<UiValueComponent>(m_entity).text)
                         : std::string_view{};
    }

    void TextInput::setPlaceholder(const std::string_view text)
    {
        if (!isValid())
        {
            return;
        }
        World& world = m_scope->world();
        const auto* existing = world.tryGetConstComponent<UiPlaceholderComponent>(m_entity);
        if (existing && !existing->key.isValid() && existing->text == text)
        {
            return;
        }
        if (existing && (existing->key.isValid() || text.empty()))
        {
            world.removeComponent<UiPlaceholderComponent>(m_entity); // a localized one leaves the loc owners
            existing = nullptr;
        }
        if (!text.empty())
        {
            if (existing)
            {
                world.getComponent<UiPlaceholderComponent>(m_entity).text.assign(text);
            }
            else
            {
                UiPlaceholderComponent placeholder;
                placeholder.text.assign(text);
                world.addComponent<UiPlaceholderComponent>(m_entity, std::move(placeholder));
            }
        }
        m_scope->hierarchy().markPaintDirty(m_entity);
    }

    void TextInput::setLocalizedPlaceholder(const InternedString key)
    {
        if (!isValid())
        {
            return;
        }
        World& world = m_scope->world();
        const auto* existing = world.tryGetConstComponent<UiPlaceholderComponent>(m_entity);
        if (!key.isValid())
        {
            if (existing)
            {
                world.removeComponent<UiPlaceholderComponent>(m_entity);
                m_scope->hierarchy().markPaintDirty(m_entity);
            }
            return;
        }
        if (existing && existing->key == key)
        {
            return;
        }
        if (existing && !existing->key.isValid())
        {
            world.removeComponent<UiPlaceholderComponent>(m_entity); // only a keyed one is a loc owner: add it afresh
            existing = nullptr;
        }
        if (existing)
        {
            world.getComponent<UiPlaceholderComponent>(m_entity).key = key;
        }
        else
        {
            UiPlaceholderComponent placeholder;
            placeholder.key = key;
            world.addComponent<UiPlaceholderComponent>(m_entity, std::move(placeholder));
        }
        m_scope->hierarchy().refreshLocalized(m_entity);
    }

    std::string_view TextInput::placeholder() const
    {
        const auto* component =
            isValid() ? m_scope->world().tryGetConstComponent<UiPlaceholderComponent>(m_entity) : nullptr;
        return component ? std::string_view(component->text) : std::string_view{};
    }

    void TextInput::bind(std::string* variable)
    {
        if (UiScope::TextInputData* data = isValid() ? m_scope->textData(m_entity) : nullptr)
        {
            data->bound = variable;
        }
    }

    void TextInput::onChange(std::function<void(std::string_view)> fn)
    {
        if (UiScope::TextInputData* data = isValid() ? m_scope->textData(m_entity) : nullptr)
        {
            data->onChange = std::move(fn);
        }
    }

    void TextInput::onCommit(std::function<void(std::string_view)> fn)
    {
        if (UiScope::TextInputData* data = isValid() ? m_scope->textData(m_entity) : nullptr)
        {
            data->onCommit = std::move(fn);
        }
    }

    void TextInput::onSubmit(std::function<void(std::string_view)> fn)
    {
        if (UiScope::TextInputData* data = isValid() ? m_scope->textData(m_entity) : nullptr)
        {
            data->onSubmit = std::move(fn);
        }
    }

    void TextInput::onCancel(std::function<void()> fn)
    {
        if (UiScope::TextInputData* data = isValid() ? m_scope->textData(m_entity) : nullptr)
        {
            data->onCancel = std::move(fn);
        }
    }

    void TextInput::onCancelRequest(std::function<bool()> fn)
    {
        if (UiScope::TextInputData* data = isValid() ? m_scope->textData(m_entity) : nullptr)
        {
            data->cancelTaken = std::move(fn);
        }
    }

    void TextInput::onKey(std::function<void(u32, u8)> fn)
    {
        if (UiScope::TextInputData* data = isValid() ? m_scope->textData(m_entity) : nullptr)
        {
            data->onKey = std::move(fn);
        }
    }

    void TextInput::insert(const std::string_view text)
    {
        if (isValid())
        {
            m_scope->textInsert(m_entity, text);
        }
    }

    void TextInput::apply(const UiTextCommand command, const bool extendSelection)
    {
        if (isValid())
        {
            m_scope->textCommand(m_entity, command, extendSelection);
        }
    }

    void TextInput::select(const u32 anchor, const u32 caret)
    {
        if (isValid())
        {
            m_scope->textSelect(m_entity, anchor, caret);
        }
    }

    u32 TextInput::caret() const
    {
        return isValid() ? m_scope->world().getConstComponent<UiTextEditComponent>(m_entity).caret : 0;
    }

    u32 TextInput::anchor() const
    {
        return isValid() ? m_scope->world().getConstComponent<UiTextEditComponent>(m_entity).anchor : 0;
    }

    void NumberField::setNumber(const double value)
    {
        if (isValid())
        {
            m_scope->setNumber(m_entity, value, false);
        }
    }

    double NumberField::number() const
    {
        return isValid() ? m_scope->world().getConstComponent<UiNumericComponent>(m_entity).value : 0.0;
    }

    void NumberField::onNumber(std::function<void(double)> fn)
    {
        if (UiScope::TextInputData* data = isValid() ? m_scope->textData(m_entity) : nullptr)
        {
            data->onNumber = std::move(fn);
        }
    }

    void NumberField::onInvalid(std::function<void(std::string_view)> fn)
    {
        if (UiScope::TextInputData* data = isValid() ? m_scope->textData(m_entity) : nullptr)
        {
            data->onInvalid = std::move(fn);
        }
    }

    // -- UiScope text internals -------------------------------------------------

    TextInput UiScope::createTextInput(const InternedString id, const UiRectDesc& desc)
    {
        return TextInput(this, createTextElement(id, UiWidgetKind::TextInput, desc, false), m_generation);
    }

    NumberField UiScope::createNumberField(const InternedString id, const UiNumberDesc& desc)
    {
        const Entity entity = createTextElement(id, UiWidgetKind::NumberField, desc.rect, true);
        UiNumericComponent numeric;
        numeric.min = desc.min;
        numeric.max = desc.max;
        numeric.step = desc.step;
        numeric.decimals = desc.decimals;
        m_world->addComponent<UiNumericComponent>(entity, numeric);
        setNumber(entity, desc.value, false);
        return NumberField(this, entity, m_generation);
    }

    Entity UiScope::createTextElement(const InternedString id, const UiWidgetKind kind, const UiRectDesc& desc,
                                      const bool numeric)
    {
        const Entity entity = createElement(id, kind, desc);
        m_world->getComponent<UiStateComponent>(entity).tabStop = true;
        m_world->addComponent<UiValueComponent>(entity, UiValueComponent{});
        m_world->addComponent<UiTextEditComponent>(entity, UiTextEditComponent{});
        m_textInputs[entity.id()].numeric = numeric;
        m_textInputs[entity.id()].selectOnFocus = numeric;

        setReaction(entity, UiEventKind::TextInput, [this, entity](const void* payload)
                    { textInsert(entity, static_cast<const UiTextInputEvent*>(payload)->text); });
        setReaction(entity, UiEventKind::TextComposition,
                    [this, entity](const void* payload)
                    {
                        const auto* event = static_cast<const UiTextCompositionEvent*>(payload);
                        textCompose(entity, event->text, event->cursor, event->selected);
                    });
        setReaction(entity, UiEventKind::FocusGained, [this, entity](const void*) { textFocus(entity, true); });
        setReaction(entity, UiEventKind::FocusLost, [this, entity](const void*) { textFocus(entity, false); });
        setReaction(entity, UiEventKind::PointerDown,
                    [this, entity](const void* payload)
                    {
                        const auto* event = static_cast<const UiPointerDownEvent*>(payload);
                        TextInputData* data = textData(entity);
                        if (event->button != 0 || !data)
                        {
                            return;
                        }
                        if (data->focusPress)
                        {
                            data->focusPress = false;
                            return;
                        }
                        const u32 caret = caretAt(entity, event->position);
                        textSelect(entity, caret, caret);
                    });
        if (numeric)
        {
            setReaction(entity, UiEventKind::DragStart, [this, entity](const void* payload)
                        { numberDrag(entity, static_cast<const UiDragStartEvent*>(payload)->delta); });
            setReaction(entity, UiEventKind::DragMove, [this, entity](const void* payload)
                        { numberDrag(entity, static_cast<const UiDragMoveEvent*>(payload)->delta); });
            setReaction(entity, UiEventKind::DragEnd,
                        [this, entity](const void*)
                        {
                            if (m_context->router().focused() == entity)
                            {
                                m_context->router().clearFocus();
                            }
                        });
        }
        else
        {
            const auto dragSelect = [this, entity](const Vec2 position)
            {
                textSelect(entity, m_world->getConstComponent<UiTextEditComponent>(entity).anchor,
                           caretAt(entity, position));
            };
            setReaction(entity, UiEventKind::DragStart, [dragSelect](const void* payload)
                        { dragSelect(static_cast<const UiDragStartEvent*>(payload)->position); });
            setReaction(entity, UiEventKind::DragMove, [dragSelect](const void* payload)
                        { dragSelect(static_cast<const UiDragMoveEvent*>(payload)->position); });
        }

        // Every key is the field's while it edits — printable keys type (they
        // arrive again as text), the rest are commands or ignored — except a
        // Ctrl or Alt chord editing does not map: unhandled, it reaches editor
        // hotkeys and gameplay (Ctrl+D duplicates; Ctrl+Z still undoes the text).
        // Ctrl+Alt (AltGr) types, so it is the field's. The function keys never
        // type or edit: F6 and Shift+F6 leave the field (landmark navigation),
        // the rest reach hotkeys, whatever the modifiers.
        setKeyReaction(entity,
                       [this, entity](const UiKeyDownEvent& event)
                       {
                           if (event.scancode >= Key::F1 && event.scancode <= Key::F12)
                           {
                               return false;
                           }
                           const bool shift = (event.modifiers & UiKeyModifier::SHIFT) != 0;
                           // Windows reports AltGr as Ctrl+Alt: typing, never a chord
                           constexpr u8 ALT_GR = UiKeyModifier::CTRL | UiKeyModifier::ALT;
                           const bool altGr = (event.modifiers & ALT_GR) == ALT_GR;
                           const bool ctrl = (event.modifiers & UiKeyModifier::CTRL) != 0 && !altGr;
                           bool handled = true;
                           const auto run = [&](const UiTextCommand command, const bool extend = false)
                           { textCommand(entity, command, extend); };
                           switch (event.scancode)
                           {
                           case Key::LEFT:
                               run(ctrl ? UiTextCommand::WordLeft : UiTextCommand::Left, shift);
                               break;
                           case Key::RIGHT:
                               run(ctrl ? UiTextCommand::WordRight : UiTextCommand::Right, shift);
                               break;
                           case Key::HOME:
                               run(UiTextCommand::Home, shift);
                               break;
                           case Key::END:
                               run(UiTextCommand::End, shift);
                               break;
                           case Key::BACKSPACE:
                               run(ctrl ? UiTextCommand::DeleteWordBackward : UiTextCommand::DeleteBackward);
                               break;
                           case Key::DELETE:
                               run(ctrl ? UiTextCommand::DeleteWordForward : UiTextCommand::DeleteForward);
                               break;
                           case Key::RETURN:
                           case Key::KP_ENTER:
                               if (!event.repeat)
                               {
                                   run(UiTextCommand::Commit);
                               }
                               break;
                           case Key::ESCAPE:
                               if (!event.repeat)
                               {
                                   run(UiTextCommand::Cancel);
                               }
                               break;
                           default:
                               {
                                   bool mapped = ctrl;
                                   switch (ctrl ? event.scancode : 0)
                                   {
                                   case Key::A:
                                       run(UiTextCommand::SelectAll);
                                       break;
                                   case Key::C:
                                       run(UiTextCommand::Copy);
                                       break;
                                   case Key::X:
                                       run(UiTextCommand::Cut);
                                       break;
                                   case Key::V:
                                       run(UiTextCommand::Paste);
                                       break;
                                   case Key::Z:
                                       run(shift ? UiTextCommand::Redo : UiTextCommand::Undo);
                                       break;
                                   case Key::Y:
                                       run(UiTextCommand::Redo);
                                       break;
                                   default:
                                       mapped = false;
                                       break;
                                   }
                                   const TextInputData* data = mapped ? nullptr : textData(entity);
                                   const bool onKey = data && data->onKey;
                                   if (onKey)
                                   {
                                       const auto fn = data->onKey; // may replace itself
                                       fn(event.scancode, event.modifiers);
                                   }
                                   handled = mapped || altGr || (event.modifiers & ALT_GR) == 0;
                                   // without an onKey (a console's completion), Tab is focus navigation
                                   if (event.scancode == Key::TAB && !ctrl && !onKey)
                                   {
                                       handled = false;
                                   }
                                   break;
                               }
                           }
                           return handled;
                       });
        return entity;
    }

    UiScope::TextInputData* UiScope::textData(const Entity entity)
    {
        const auto it = m_textInputs.find(entity.id());
        return it == m_textInputs.end() ? nullptr : &it->second;
    }

    void UiScope::textCommand(const Entity entity, const UiTextCommand command, const bool extend)
    {
        TextInputData* data = textData(entity);
        if (!data)
        {
            return;
        }
        // caret, anchor and text are writable through the control protocol:
        // snap them in place, without ending a typing run as textSelect does
        const std::string text = m_world->getConstComponent<UiValueComponent>(entity).text;
        auto& edit = m_world->getComponent<UiTextEditComponent>(entity);
        edit.caret = snap(text, edit.caret);
        edit.anchor = snap(text, edit.anchor);
        const u32 caret = edit.caret;
        const u32 anchor = edit.anchor;
        const u32 selectionStart = std::min(caret, anchor);
        const u32 selectionEnd = std::max(caret, anchor);
        const bool selection = selectionStart != selectionEnd;
        const auto move = [&](const u32 to) { textSelect(entity, extend ? anchor : to, to); };
        // Deletes the selection, else [from, to).
        const auto erase = [&](const u32 from, const u32 to)
        {
            const u32 start = selection ? selectionStart : from;
            const u32 end = selection ? selectionEnd : to;
            if (start != end)
            {
                textEdit(entity, text.substr(0, start) + text.substr(end), start, TextEditKind::Deleting);
            }
        };

        switch (command)
        {
        case UiTextCommand::Left:
            move(selection && !extend ? selectionStart : previousBoundary(text, caret));
            break;
        case UiTextCommand::Right:
            move(selection && !extend ? selectionEnd : nextBoundary(text, caret));
            break;
        case UiTextCommand::WordLeft:
            move(wordLeft(text, caret));
            break;
        case UiTextCommand::WordRight:
            move(wordRight(text, caret));
            break;
        case UiTextCommand::Home:
            move(0);
            break;
        case UiTextCommand::End:
            move(static_cast<u32>(text.size()));
            break;
        case UiTextCommand::DeleteBackward:
            erase(previousBoundary(text, caret), caret);
            break;
        case UiTextCommand::DeleteForward:
            erase(caret, nextBoundary(text, caret));
            break;
        case UiTextCommand::DeleteWordBackward:
            erase(wordLeft(text, caret), caret);
            break;
        case UiTextCommand::DeleteWordForward:
            erase(caret, wordRight(text, caret));
            break;
        case UiTextCommand::SelectAll:
            textSelect(entity, 0, static_cast<u32>(text.size()));
            break;
        case UiTextCommand::Copy:
        case UiTextCommand::Cut:
            if (selection)
            {
                m_context->setClipboardText(
                    std::string_view(text).substr(selectionStart, selectionEnd - selectionStart));
                if (command == UiTextCommand::Cut)
                {
                    textEdit(entity, text.substr(0, selectionStart) + text.substr(selectionEnd), selectionStart,
                             TextEditKind::Other);
                }
            }
            break;
        case UiTextCommand::Paste:
            {
                const std::string pasted = printable(m_context->clipboardText());
                if (!pasted.empty() || selection)
                {
                    textEdit(entity, text.substr(0, selectionStart) + pasted + text.substr(selectionEnd),
                             selectionStart + static_cast<u32>(pasted.size()), TextEditKind::Other);
                }
                break;
            }
        case UiTextCommand::Undo:
            textRestore(entity, data->undo, data->redo);
            break;
        case UiTextCommand::Redo:
            textRestore(entity, data->redo, data->undo);
            break;
        case UiTextCommand::Commit:
            textCommit(entity);
            if (const TextInputData* submitted = textData(entity); submitted && submitted->onSubmit)
            {
                const auto onSubmit = submitted->onSubmit;
                onSubmit(std::string(m_world->getConstComponent<UiValueComponent>(entity).text));
            }
            break;
        case UiTextCommand::Cancel:
            if (data->cancelTaken)
            {
                const auto taken = data->cancelTaken;
                if (taken())
                {
                    break;
                }
            }
            if (data->onCancel)
            {
                const auto onCancel = data->onCancel; // it may close the list that owns this input
                onCancel();
            }
            if (!m_world->isEntityValid(entity))
            {
                break;
            }
            if (text != data->committed)
            {
                const std::string committed = data->committed;
                textEdit(entity, committed, static_cast<u32>(committed.size()), TextEditKind::Other);
            }
            if (m_context->router().focused() == entity)
            {
                m_context->router().clearFocus();
            }
            break;
        }
    }

    void UiScope::textInsert(const Entity entity, const std::string_view text)
    {
        const std::string inserted = printable(text);
        const auto& edit = m_world->getConstComponent<UiTextEditComponent>(entity);
        const std::string& value = m_world->getConstComponent<UiValueComponent>(entity).text;
        // snapped: caret and text are writable through the control protocol
        const u32 caret = snap(value, edit.caret);
        const u32 anchor = snap(value, edit.anchor);
        const u32 start = std::min(caret, anchor);
        const u32 end = std::max(caret, anchor);
        if (inserted.empty() && start == end)
        {
            return;
        }
        textEdit(entity, value.substr(0, start) + inserted + value.substr(end),
                 start + static_cast<u32>(inserted.size()), TextEditKind::Typing);
    }

    void UiScope::textSelect(const Entity entity, const u32 anchor, const u32 caret)
    {
        const std::string_view text = m_world->getConstComponent<UiValueComponent>(entity).text;
        auto& edit = m_world->getComponent<UiTextEditComponent>(entity);
        const u32 snappedAnchor = snap(text, anchor);
        const u32 snappedCaret = snap(text, caret);
        if (TextInputData* data = textData(entity))
        {
            data->lastEdit = TextEditKind::None; // a move ends a typing run
        }
        if (edit.anchor == snappedAnchor && edit.caret == snappedCaret)
        {
            return;
        }
        edit.anchor = snappedAnchor;
        edit.caret = snappedCaret;
        hierarchy().markPaintDirty(entity);
    }

    void UiScope::textEdit(const Entity entity, std::string text, const u32 caret, const TextEditKind kind)
    {
        TextInputData& data = *textData(entity);
        auto& value = m_world->getComponent<UiValueComponent>(entity);
        auto& edit = m_world->getComponent<UiTextEditComponent>(entity);
        if (kind == TextEditKind::Other || kind != data.lastEdit)
        {
            data.undo.push_back({value.text, edit.caret, edit.anchor});
            if (data.undo.size() > MAX_UNDO)
            {
                data.undo.erase(data.undo.begin());
            }
        }
        data.redo.clear();
        data.lastEdit = kind;
        value.text = std::move(text);
        edit.caret = edit.anchor = caret;
        textChanged(entity);
    }

    void UiScope::textRestore(const Entity entity, std::vector<TextSnapshot>& from, std::vector<TextSnapshot>& to)
    {
        if (from.empty())
        {
            return;
        }
        auto& value = m_world->getComponent<UiValueComponent>(entity);
        auto& edit = m_world->getComponent<UiTextEditComponent>(entity);
        to.push_back({value.text, edit.caret, edit.anchor});
        TextSnapshot snapshot = std::move(from.back());
        from.pop_back();
        value.text = std::move(snapshot.text);
        edit.caret = snapshot.caret;
        edit.anchor = snapshot.anchor;
        textData(entity)->lastEdit = TextEditKind::None;
        textChanged(entity);
    }

    void UiScope::textChanged(const Entity entity)
    {
        // by value: callbacks may build UI and move component storage
        const std::string text = m_world->getConstComponent<UiValueComponent>(entity).text;
        if (auto* numeric = m_world->tryGetComponent<UiNumericComponent>(entity))
        {
            numeric->valid = parseNumber(text).has_value();
        }
        markLayoutDirty(entity);
        const TextInputData* data = textData(entity);
        if (data->bound)
        {
            *data->bound = text;
        }
        if (data->onChange)
        {
            // copy: the callback may replace itself
            const auto onChange = data->onChange;
            onChange(text);
        }
    }

    void UiScope::textCommit(const Entity entity)
    {
        TextInputData* data = textData(entity);
        if (!data)
        {
            return;
        }
        if (data->numeric)
        {
            const auto& numeric = m_world->getConstComponent<UiNumericComponent>(entity);
            const std::string typed = m_world->getConstComponent<UiValueComponent>(entity).text;
            const std::optional<double> parsed = parseNumber(typed);
            const double last = numeric.value; // read before the owner's hook, which may move the component storage
            if (!parsed && data->onInvalid && typed != data->committed)
            {
                const auto onInvalid = data->onInvalid; // by value: the owner may rebuild the field
                onInvalid(typed);
                data = textData(entity);
                if (!data)
                {
                    return;
                }
            }
            setNumber(entity, parsed ? *parsed : last, true);
            data = textData(entity);
            if (!data)
            {
                return;
            }
        }
        const std::string text = m_world->getConstComponent<UiValueComponent>(entity).text;
        if (text == data->committed)
        {
            return;
        }
        data->committed = text;
        if (data->onCommit)
        {
            const auto onCommit = data->onCommit;
            onCommit(text);
        }
    }

    void UiScope::textCompose(const Entity entity, const std::string_view text, const u32 cursor, const u32 selected)
    {
        auto& edit = m_world->getComponent<UiTextEditComponent>(entity);
        if (!edit.editing && !text.empty())
        {
            return; // composition belongs to the input holding focus
        }
        if (edit.composition == text && edit.compositionCursor == cursor && edit.compositionSelected == selected)
        {
            return;
        }
        edit.composition = printable(text);
        // the cursor offsets index the platform's text: control characters dropped from it shift them, so clamp
        edit.compositionCursor = std::min<u32>(cursor, static_cast<u32>(edit.composition.size()));
        edit.compositionSelected =
            std::min<u32>(selected, static_cast<u32>(edit.composition.size()) - edit.compositionCursor);
        hierarchy().markPaintDirty(entity);
    }

    void UiScope::textFocus(const Entity entity, const bool gained)
    {
        TextInputData* data = textData(entity);
        if (!data)
        {
            return;
        }
        // the hook may destroy the input: it is the last thing a focus change does
        const auto notify = [this, entity, gained]
        {
            if (const TextInputData* hooked = textData(entity); hooked && hooked->onFocus)
            {
                const auto onFocus = hooked->onFocus;
                onFocus(gained);
            }
            if (const TextInputData* hooked = textData(entity); hooked && hooked->ownerFocus)
            {
                const auto ownerFocus = hooked->ownerFocus;
                ownerFocus(gained);
            }
        };
        if (!gained)
        {
            textCommit(entity);
            if (m_world->isEntityValid(entity))
            {
                auto& edit = m_world->getComponent<UiTextEditComponent>(entity);
                edit.editing = false;
                edit.composition.clear();
                edit.compositionCursor = edit.compositionSelected = 0;
                edit.anchor = edit.caret;
                hierarchy().markPaintDirty(entity);
            }
            notify();
            return;
        }
        textReset(entity);
        const std::string& text = m_world->getConstComponent<UiValueComponent>(entity).text;
        // the press that focused a number field keeps its select-all
        data->focusPress = data->selectOnFocus && m_context->router().pressed() == entity;
        auto& edit = m_world->getComponent<UiTextEditComponent>(entity);
        edit.editing = true;
        edit.caret = static_cast<u32>(text.size());
        edit.anchor = data->selectOnFocus ? 0 : edit.caret;
        hierarchy().markPaintDirty(entity);
        notify();
    }

    void UiScope::textReset(const Entity entity)
    {
        TextInputData* data = textData(entity);
        if (!data)
        {
            return;
        }
        data->committed = m_world->getConstComponent<UiValueComponent>(entity).text;
        data->undo.clear();
        data->redo.clear();
        data->lastEdit = TextEditKind::None;
    }

    u32 UiScope::caretAt(const Entity entity, const Vec2 position) const
    {
        const std::string_view text = m_world->getConstComponent<UiValueComponent>(entity).text;
        const UiTextMeasurer* measurer = hierarchy().textMeasurer();
        if (!measurer)
        {
            return static_cast<u32>(text.size());
        }
        const UiRect rect = hierarchy().absoluteRectOf(entity);
        const float scale = hierarchy().layoutScale();
        return measurer->caretAt(entity, text, (position.x - rect.position.x) / scale, rect.size.x / scale);
    }

    void UiScope::numberDrag(const Entity entity, const Vec2 delta)
    {
        auto& edit = m_world->getComponent<UiTextEditComponent>(entity);
        if (edit.editing)
        {
            edit.editing = false; // dragging, not typing: no caret
            edit.caret = edit.anchor = 0;
            hierarchy().markPaintDirty(entity);
        }
        const auto& numeric = m_world->getConstComponent<UiNumericComponent>(entity);
        setNumber(entity, numeric.value + delta.x / hierarchy().layoutScale() * numeric.step, true);
    }

    void UiScope::setNumber(const Entity entity, const double value, const bool notify)
    {
        auto& numeric = m_world->getComponent<UiNumericComponent>(entity);
        const double conformed = conform(numeric, value);
        const bool changed = conformed != numeric.value;
        numeric.value = conformed;
        numeric.valid = true;
        std::string text = formatNumber(conformed, numeric.decimals);
        auto& current = m_world->getComponent<UiValueComponent>(entity);
        if (text != current.text)
        {
            auto& edit = m_world->getComponent<UiTextEditComponent>(entity);
            edit.caret = edit.anchor = static_cast<u32>(text.size());
            current.text = std::move(text);
            if (notify)
            {
                textChanged(entity);
            }
            else
            {
                markLayoutDirty(entity);
            }
        }
        if (!notify)
        {
            textReset(entity);
        }
        const TextInputData* data = textData(entity);
        if (notify && changed && data && data->onNumber)
        {
            const auto onNumber = data->onNumber;
            onNumber(conformed);
        }
    }
} // namespace spite
