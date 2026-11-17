#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>

#include "control/ControlServer.hpp"

namespace spite
{
    inline std::optional<double> numberArg(const ControlArgs& args, const std::string_view key)
    {
        std::optional<std::string> text = args.json(key);
        if (!text)
        {
            return std::nullopt;
        }
        if (text->size() >= 2 && text->front() == '"')
        {
            *text = text->substr(1, text->size() - 2);
        }
        double value = 0.0;
        const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
        return error == std::errc{} && end == text->data() + text->size() ? std::optional<double>(value) : std::nullopt;
    }
} // namespace spite
