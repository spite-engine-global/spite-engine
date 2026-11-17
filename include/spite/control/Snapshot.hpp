#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/Platform.hpp"
#include "control/ControlExport.hpp"

namespace spite
{
    class IFormatWriter;
    class ServiceLocator;
    class World;
    struct StructSchema;

    // A table a caller adds to a snapshot file: each row points at a value schema describes.
    struct SnapshotTable
    {
        std::string name;
        const StructSchema* schema = nullptr;
        std::vector<const void*> rows;
    };

    // Raw bytes stored as one row (name TEXT PRIMARY KEY, bytes INTEGER, data BLOB) of a blob table.
    struct SnapshotBlob
    {
        std::string name;
        std::span<const std::byte> data;
    };

    // Writes a SQLite snapshot of the world's schema components and
    // the services' tables to path, replacing any file there; only, when not
    // empty, names the tables to write. Reports the file and its row counts to
    // out; returns an error message, empty on success. pageSize (a power of two, 512 to 65536) is the SQLite page
    // of the file: a file that blobs of megabytes follow is written and read several times faster in 64 KiB pages,
    // at 64 KiB per table.
    SPITE_CONTROL_API std::string writeSnapshot(const World& world, const ServiceLocator* services,
                                                const std::string& path, const std::vector<std::string>& only,
                                                IFormatWriter& out, u32 pageSize = 4096);

    // Adds tables, and blobs as the table blobTable, to an existing snapshot file in one transaction; a name the
    // file already has fails. Returns an error message, empty on success.
    SPITE_CONTROL_API std::string appendSnapshot(const std::string& path, std::span<const SnapshotTable> tables,
                                                 std::string_view blobTable, std::span<const SnapshotBlob> blobs);

    // The entity's row of every table in a snapshot file with an `entity` column, each as an object named after its
    // table holding the row's columns (the entity column left out; BLOB columns as their size). Returns an error
    // message, empty on success.
    SPITE_CONTROL_API std::string readSnapshotEntity(const std::string& path, u64 entity, IFormatWriter& out);
} // namespace spite
