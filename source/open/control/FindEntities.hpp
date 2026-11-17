#pragma once

#include <functional>
#include <span>
#include <string>

#include "base/Platform.hpp"
#include "control/ControlServer.hpp"

namespace spite
{
    class IFormatWriter;
    struct Entity;

    // The `find` query: the world's entities matching every given filter, in
    // ascending id order, a page at a time. Reads
    // through const access only; returns an error message, empty on success.
    // source, when given, maps a row's id to the id it was cloned from (0 = none): a nonzero one is the row's `source`.
    // extra, when given, writes more members into each row (its place in the hierarchy).
    std::string findEntities(const World& world, const ControlArgs& args, IFormatWriter& out,
                             const std::function<u64(u64)>* source = nullptr,
                             const std::function<void(Entity, IFormatWriter&)>* extra = nullptr);

    // The `hierarchyAudit` query: the world's scene (non-UI) entities with no name and the
    // transform entities in no tree, counted overall and per group; args groups, samples. hierarchy is the
    // hierarchy reader; without one no entity counts as unrooted.
    std::string auditEntities(const World& world, const ControlArgs& args, IFormatWriter& out,
                              const ControlServer::HierarchyReaderFn* hierarchy);

    // What a layout command wrote, audited: members `audit` (entities, unrooted, unnamed) and, when either is
    // nonzero, `warnings` (one sentence each).
    void auditCreated(const World& world, std::span<const Entity> created, IFormatWriter& out,
                      const ControlServer::HierarchyReaderFn* hierarchy);
} // namespace spite
