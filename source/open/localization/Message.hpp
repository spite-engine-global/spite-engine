#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "localization/Localization.hpp"
#include "LocaleRules.hpp"

namespace spite::loc
{
    enum class NodeKind : u8
    {
        Text, // text[begin, begin + size)
        Argument, // {name} or {name, number}: name is text[begin, begin + size)
        Plural, // {name, plural, ...}: branches[firstBranch, firstBranch + branchCount)
        Pound // # inside a plural branch: the innermost plural's number
    };

    struct MessageNode
    {
        NodeKind kind = NodeKind::Text;
        u32 begin = 0;
        u32 size = 0;
        u32 firstBranch = 0;
        u32 branchCount = 0;
    };

    struct PluralBranch
    {
        bool exact = false; // =value, matched before the category
        PluralCategory category = PluralCategory::Other;
        std::int64_t value = 0;
        u32 firstNode = 0;
        u32 nodeCount = 0;
    };

    // A message parsed once: unescaped text and argument names packed into text, every sequence (the top level and
    // each plural branch) a contiguous node range.
    struct CompiledMessage
    {
        std::string text;
        std::vector<MessageNode> nodes;
        std::vector<PluralBranch> branches;
        u32 firstNode = 0;
        u32 nodeCount = 0;

        [[nodiscard]] bool isLiteral() const
        {
            return nodeCount == 0 || (nodeCount == 1 && nodes[firstNode].kind == NodeKind::Text);
        }
    };

    // Validates UTF-8 and the syntax; error.key stays empty.
    [[nodiscard]] std::expected<CompiledMessage, LocalizationError> compileMessage(std::string_view source);

    // Appends the formatted message; a missing argument renders as {name}.
    void formatMessage(const CompiledMessage& message, const LocaleRules& rules, std::span<const MessageArg> args,
                       std::string& out);
} // namespace spite::loc
