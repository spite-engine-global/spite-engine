#include "Message.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace spite::loc
{
    namespace
    {
        // offset of the first byte that breaks UTF-8 (overlong forms, surrogates and code points past U+10FFFF
        // included), or npos
        sizet invalidUtf8(const std::string_view text)
        {
            const auto byteAt = [&](const sizet index) { return static_cast<u8>(text[index]); };
            sizet index = 0;
            while (index < text.size())
            {
                const u8 lead = byteAt(index);
                sizet length = 0;
                u32 minimum = 0;
                u32 codePoint = 0;
                if (lead < 0x80)
                {
                    ++index;
                    continue;
                }
                if ((lead & 0xE0) == 0xC0)
                {
                    length = 2, minimum = 0x80, codePoint = lead & 0x1F;
                }
                else if ((lead & 0xF0) == 0xE0)
                {
                    length = 3, minimum = 0x800, codePoint = lead & 0x0F;
                }
                else if ((lead & 0xF8) == 0xF0)
                {
                    length = 4, minimum = 0x10000, codePoint = lead & 0x07;
                }
                else
                {
                    return index;
                }
                if (index + length > text.size())
                {
                    return index;
                }
                for (sizet next = 1; next < length; ++next)
                {
                    if ((byteAt(index + next) & 0xC0) != 0x80)
                    {
                        return index;
                    }
                    codePoint = (codePoint << 6) | (byteAt(index + next) & 0x3F);
                }
                if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
                {
                    return index;
                }
                index += length;
            }
            return std::string_view::npos;
        }

        bool isNameChar(const char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }

        bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

        std::optional<PluralCategory> categoryNamed(const std::string_view name)
        {
            constexpr std::pair<std::string_view, PluralCategory> NAMES[]{
                {"zero", PluralCategory::Zero}, {"one", PluralCategory::One},   {"two", PluralCategory::Two},
                {"few", PluralCategory::Few},   {"many", PluralCategory::Many}, {"other", PluralCategory::Other}};
            for (const auto& [text, category] : NAMES)
            {
                if (text == name)
                    return category;
            }
            return std::nullopt;
        }

        class Parser
        {
        public:
            Parser(const std::string_view source, CompiledMessage& out) : m_source(source), m_out(out) {}

            bool message() { return sequence(false, m_out.firstNode, m_out.nodeCount); }

            LocalizationError takeError() { return std::move(m_error); }

        private:
            bool fail(const sizet offset, std::string reason)
            {
                m_error.offset = static_cast<u32>(offset);
                m_error.reason = std::move(reason);
                return false;
            }

            [[nodiscard]] bool atEnd() const { return m_pos >= m_source.size(); }
            [[nodiscard]] char peek() const { return atEnd() ? '\0' : m_source[m_pos]; }

            void skipSpace()
            {
                while (!atEnd() && isSpace(m_source[m_pos]))
                    ++m_pos;
            }

            std::string_view word()
            {
                const sizet begin = m_pos;
                while (!atEnd() && isNameChar(m_source[m_pos]))
                    ++m_pos;
                return m_source.substr(begin, m_pos - begin);
            }

            // stores s in the packed text; returns its begin
            u32 store(const std::string_view s)
            {
                const auto begin = static_cast<u32>(m_out.text.size());
                m_out.text.append(s);
                return begin;
            }

            static void appendText(std::vector<MessageNode>& nodes, CompiledMessage& out, const std::string_view s)
            {
                if (s.empty())
                    return;
                if (!nodes.empty() && nodes.back().kind == NodeKind::Text &&
                    nodes.back().begin + nodes.back().size == out.text.size())
                {
                    nodes.back().size += static_cast<u32>(s.size());
                }
                else
                {
                    nodes.push_back({NodeKind::Text, static_cast<u32>(out.text.size()), static_cast<u32>(s.size())});
                }
                out.text.append(s);
            }

            // A run of text, arguments and (inside a plural branch) #, up to the end or, in a branch, the '}' that
            // closes it (left unconsumed). The run's nodes are appended contiguously after any nested ones.
            bool sequence(const bool inPlural, u32& first, u32& count)
            {
                std::vector<MessageNode> nodes;
                while (!atEnd())
                {
                    const char c = m_source[m_pos];
                    if (c == '\'')
                    {
                        if (!quote(nodes, inPlural))
                            return false;
                    }
                    else if (c == '{')
                    {
                        if (!argument(nodes))
                            return false;
                    }
                    else if (c == '}')
                    {
                        if (inPlural)
                            break;
                        return fail(m_pos, "unmatched '}'");
                    }
                    else if (c == '#' && inPlural)
                    {
                        nodes.push_back({NodeKind::Pound});
                        ++m_pos;
                    }
                    else
                    {
                        const sizet next =
                            std::min(m_source.find_first_of(inPlural ? "'{}#" : "'{}", m_pos), m_source.size());
                        appendText(nodes, m_out, m_source.substr(m_pos, next - m_pos));
                        m_pos = next;
                    }
                }
                first = static_cast<u32>(m_out.nodes.size());
                count = static_cast<u32>(nodes.size());
                m_out.nodes.insert(m_out.nodes.end(), nodes.begin(), nodes.end());
                return true;
            }

            // ICU apostrophe mode DOUBLE_OPTIONAL: '' is one quote; a quote before a syntax character starts a quoted
            // literal up to the next single quote; any other quote is literal. An unterminated quoted literal is an
            // error (ICU would quote to the end, which hides a typo).
            bool quote(std::vector<MessageNode>& nodes, const bool inPlural)
            {
                const sizet open = m_pos++;
                const char next = peek();
                if (next == '\'')
                {
                    appendText(nodes, m_out, "'");
                    ++m_pos;
                    return true;
                }
                if (next != '{' && next != '}' && !(inPlural && next == '#'))
                {
                    appendText(nodes, m_out, "'");
                    return true;
                }
                while (true)
                {
                    const sizet close = m_source.find('\'', m_pos);
                    if (close == std::string_view::npos)
                        return fail(open, "unterminated quoted literal");
                    appendText(nodes, m_out, m_source.substr(m_pos, close - m_pos));
                    m_pos = close + 1;
                    if (peek() != '\'')
                        return true;
                    appendText(nodes, m_out, "'");
                    ++m_pos;
                }
            }

            bool argument(std::vector<MessageNode>& nodes)
            {
                const sizet open = m_pos++;
                skipSpace();
                const std::string_view name = word();
                if (name.empty())
                    return fail(m_pos, "expected an argument name");
                skipSpace();
                if (peek() == '}')
                {
                    ++m_pos;
                    nodes.push_back({NodeKind::Argument, store(name), static_cast<u32>(name.size())});
                    return true;
                }
                if (peek() != ',')
                    return atEnd() ? fail(open, "unterminated argument") : fail(m_pos, "expected ',' or '}'");
                ++m_pos;
                skipSpace();
                const sizet typeAt = m_pos;
                const std::string_view type = word();
                skipSpace();
                if (type == "number")
                {
                    if (peek() != '}')
                        return atEnd() ? fail(open, "unterminated argument")
                                       : fail(m_pos, "number styles are not supported");
                    ++m_pos;
                    nodes.push_back({NodeKind::Argument, store(name), static_cast<u32>(name.size())});
                    return true;
                }
                if (type != "plural")
                {
                    return fail(typeAt,
                                type.empty() ? "expected an argument type"
                                             : std::format("unsupported argument type '{}'", type));
                }
                if (peek() != ',')
                    return fail(m_pos, "expected ',' after plural");
                ++m_pos;
                return plural(nodes, open, name);
            }

            bool plural(std::vector<MessageNode>& nodes, const sizet open, const std::string_view name)
            {
                MessageNode node{NodeKind::Plural, store(name), static_cast<u32>(name.size())};
                std::vector<PluralBranch> branches;
                bool hasOther = false;
                while (true)
                {
                    skipSpace();
                    if (atEnd())
                        return fail(open, "unterminated argument");
                    if (peek() == '}')
                    {
                        ++m_pos;
                        break;
                    }
                    const sizet selectorAt = m_pos;
                    PluralBranch branch;
                    if (peek() == '=')
                    {
                        ++m_pos;
                        const char* begin = m_source.data() + m_pos;
                        const auto [end, error] =
                            std::from_chars(begin, m_source.data() + m_source.size(), branch.value);
                        if (error != std::errc{})
                            return fail(m_pos, "expected a number after '='");
                        m_pos += static_cast<sizet>(end - begin);
                        branch.exact = true;
                    }
                    else
                    {
                        const std::string_view selector = word();
                        const std::optional<PluralCategory> category = categoryNamed(selector);
                        if (!category)
                        {
                            return fail(selectorAt,
                                        selector.empty() ? std::string("expected a plural selector")
                                                         : std::format("unknown plural category '{}'", selector));
                        }
                        branch.category = *category;
                        hasOther |= branch.category == PluralCategory::Other;
                    }
                    for (const PluralBranch& existing : branches)
                    {
                        if (existing.exact == branch.exact &&
                            (branch.exact ? existing.value == branch.value : existing.category == branch.category))
                        {
                            return fail(selectorAt, "duplicate plural selector");
                        }
                    }
                    skipSpace();
                    if (peek() != '{')
                        return fail(m_pos, "expected '{' after a plural selector");
                    const sizet branchOpen = m_pos++;
                    if (m_depth >= MAX_MESSAGE_NESTING)
                        return fail(branchOpen, "nesting too deep");
                    ++m_depth;
                    const bool ok = sequence(true, branch.firstNode, branch.nodeCount);
                    --m_depth;
                    if (!ok)
                        return false;
                    if (atEnd())
                        return fail(branchOpen, "unterminated plural branch");
                    ++m_pos;
                    branches.push_back(branch);
                }
                if (!hasOther)
                    return fail(open, "plural without an 'other' branch");
                node.firstBranch = static_cast<u32>(m_out.branches.size());
                node.branchCount = static_cast<u32>(branches.size());
                m_out.branches.insert(m_out.branches.end(), branches.begin(), branches.end());
                nodes.push_back(node);
                return true;
            }

            std::string_view m_source;
            CompiledMessage& m_out;
            sizet m_pos = 0;
            u32 m_depth = 0;
            LocalizationError m_error;
        };

        class Formatter
        {
        public:
            Formatter(const CompiledMessage& message, const LocaleRules& rules, const std::span<const MessageArg> args,
                      std::string& out) : m_message(message), m_rules(rules), m_args(args), m_out(out)
            {
            }

            void range(const u32 first, const u32 count, const MessageNode* plural, const MessageArg* pluralArg)
            {
                for (u32 index = first; index < first + count; ++index)
                {
                    const MessageNode& node = m_message.nodes[index];
                    switch (node.kind)
                    {
                    case NodeKind::Text:
                        m_out.append(text(node));
                        break;
                    case NodeKind::Argument:
                        value(node, find(text(node)));
                        break;
                    case NodeKind::Pound:
                        value(*plural, pluralArg);
                        break;
                    case NodeKind::Plural:
                        {
                            const MessageArg* arg = find(text(node));
                            const PluralBranch& branch = select(node, arg);
                            range(branch.firstNode, branch.nodeCount, &node, arg);
                            break;
                        }
                    }
                }
            }

        private:
            [[nodiscard]] std::string_view text(const MessageNode& node) const
            {
                return std::string_view(m_message.text).substr(node.begin, node.size);
            }

            [[nodiscard]] const MessageArg* find(const std::string_view name) const
            {
                for (const MessageArg& arg : m_args)
                {
                    if (arg.name == name)
                        return &arg;
                }
                return nullptr;
            }

            void value(const MessageNode& node, const MessageArg* arg)
            {
                if (!arg)
                {
                    m_out.push_back('{');
                    m_out.append(text(node));
                    m_out.push_back('}');
                    return;
                }
                std::visit(
                    [&]<typename T>(const T& v)
                    {
                        if constexpr (std::is_same_v<T, std::string_view>)
                        {
                            m_out.append(v);
                        }
                        else
                        {
                            appendNumber(m_rules, v, m_out);
                        }
                    },
                    arg->value);
            }

            [[nodiscard]] const PluralBranch& select(const MessageNode& node, const MessageArg* arg) const
            {
                const std::span branches(m_message.branches.data() + node.firstBranch, node.branchCount);
                PluralCategory category = PluralCategory::Other; // a missing or string argument selects other
                if (const auto* integer = arg ? std::get_if<std::int64_t>(&arg->value) : nullptr)
                {
                    for (const PluralBranch& branch : branches)
                    {
                        if (branch.exact && branch.value == *integer)
                            return branch;
                    }
                    category = pluralCategory(m_rules, *integer);
                }
                else if (const auto* real = arg ? std::get_if<double>(&arg->value) : nullptr)
                {
                    for (const PluralBranch& branch : branches)
                    {
                        if (branch.exact && static_cast<double>(branch.value) == *real)
                            return branch;
                    }
                    category = pluralCategory(m_rules, *real);
                }
                const PluralBranch* other = nullptr;
                for (const PluralBranch& branch : branches)
                {
                    if (branch.exact)
                        continue;
                    if (branch.category == category)
                        return branch;
                    if (branch.category == PluralCategory::Other)
                        other = &branch;
                }
                return *other; // compileMessage refuses a plural without other
            }

            const CompiledMessage& m_message;
            const LocaleRules& m_rules;
            std::span<const MessageArg> m_args;
            std::string& m_out;
        };
    } // namespace

    std::expected<CompiledMessage, LocalizationError> compileMessage(const std::string_view source)
    {
        if (source.size() > MAX_MESSAGE_BYTES)
        {
            return std::unexpected(LocalizationError{{}, static_cast<u32>(MAX_MESSAGE_BYTES), "message too long"});
        }
        if (const sizet bad = invalidUtf8(source); bad != std::string_view::npos)
        {
            return std::unexpected(LocalizationError{{}, static_cast<u32>(bad), "invalid UTF-8"});
        }
        CompiledMessage message;
        Parser parser(source, message);
        if (!parser.message())
        {
            return std::unexpected(parser.takeError());
        }
        message.text.shrink_to_fit();
        message.nodes.shrink_to_fit();
        return message;
    }

    void formatMessage(const CompiledMessage& message, const LocaleRules& rules, const std::span<const MessageArg> args,
                       std::string& out)
    {
        Formatter(message, rules, args, out).range(message.firstNode, message.nodeCount, nullptr, nullptr);
    }
} // namespace spite::loc

namespace spite
{
    std::optional<LocalizationError> validateMessage(const std::string_view message)
    {
        auto compiled = loc::compileMessage(message);
        if (compiled)
        {
            return std::nullopt;
        }
        return std::move(compiled.error());
    }

    std::expected<void, LocalizationError> formatMessage(const std::string_view locale, const std::string_view message,
                                                         const std::span<const MessageArg> args, std::string& out)
    {
        auto compiled = loc::compileMessage(message);
        if (!compiled)
        {
            return std::unexpected(std::move(compiled.error()));
        }
        loc::formatMessage(*compiled, loc::localeRules(locale), args, out);
        return {};
    }
} // namespace spite
