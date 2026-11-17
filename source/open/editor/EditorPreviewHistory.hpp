#pragma once

#include "base/InternedString.hpp"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace spite
{
    struct EditorPreviewHistory
    {
        struct Item
        {
            u64 job;
            std::string type;
            InternedString name;
            std::filesystem::path baked;
            std::string status = "queued";
            std::string error;
        };
        std::vector<Item> items;
        u64 requested = 0, failed = 0;
        sizet active = static_cast<sizet>(-1);

        void pruneHistory()
        {
            constexpr sizet HISTORY_LIMIT = 128;
            const auto terminal = [](const Item& item) { return item.status != "queued" && item.status != "active"; };
            sizet count = std::ranges::count_if(items, terminal);
            if (count <= HISTORY_LIMIT)
                return;
            sizet index = 0, removedBeforeActive = 0;
            std::erase_if(items,
                          [&](const Item& item)
                          {
                              const bool remove = count > HISTORY_LIMIT && terminal(item);
                              if (remove)
                              {
                                  --count;
                                  if (index < active)
                                      ++removedBeforeActive;
                              }
                              ++index;
                              return remove;
                          });
            if (active != static_cast<sizet>(-1))
                active -= removedBeforeActive;
        }
    };
} // namespace spite
