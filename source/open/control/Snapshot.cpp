#include "control/Snapshot.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <variant>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "base/Assert.hpp"
#include "base/ServiceLocator.hpp"
#include "base/io/FileSystem.hpp"
#include "ecs/Aspect.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/ComponentMetadata.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/World.hpp"
#include "serialization/FormatWalk.hpp"
#include "serialization/JsonFormat.hpp"

namespace spite
{
    namespace
    {
        struct Column
        {
            std::string name;
            const char* type;
        };

        // One column per scalar; vectors and quaternions split per component,
        // nested structs flatten with a prefix; fixed arrays, Mat4 and entity
        // lists are JSON text (json_extract reads them).
        void schemaColumns(const StructSchema& schema, const std::string& prefix, std::vector<Column>& out)
        {
            for (const FieldDesc& field : schema.fields)
            {
                const std::string name =
                    prefix.empty() ? std::string(field.name) : prefix + "_" + std::string(field.name);
                if (field.arrayCount > 0 || field.kind == FieldKind::EntityList || field.kind == FieldKind::Mat4)
                {
                    out.push_back({name, "TEXT"});
                    continue;
                }
                switch (field.kind)
                {
                case FieldKind::Struct:
                    schemaColumns(*field.structSchema, name, out);
                    break;
                case FieldKind::Vec2:
                case FieldKind::Vec3:
                case FieldKind::Vec4:
                case FieldKind::Quat:
                    {
                        const u32 count = field.kind == FieldKind::Vec2 ? 2 : field.kind == FieldKind::Vec3 ? 3 : 4;
                        for (u32 i = 0; i < count; ++i)
                        {
                            out.push_back({name + "_" + "xyzw"[i], "REAL"});
                        }
                        break;
                    }
                case FieldKind::F32:
                case FieldKind::F64:
                    out.push_back({name, "REAL"});
                    break;
                case FieldKind::String:
                case FieldKind::InternedString:
                    out.push_back({name, "TEXT"});
                    break;
                default:
                    out.push_back({name, "INTEGER"});
                    break;
                }
            }
        }

        using Value = std::variant<std::int64_t, double, std::string>;

        // Receives the schema walk and emits values in schemaColumns order.
        class FlatRow final : public IFormatWriter
        {
        public:
            std::vector<Value> values;

            void beginObject(const std::string_view name) override
            {
                if (m_depth > 0)
                {
                    m_capture->beginObject(name);
                    ++m_depth;
                }
            }
            void endObject() override
            {
                if (m_depth > 0)
                {
                    m_capture->endObject();
                    --m_depth;
                }
            }
            void beginArray(const std::string_view name, const u32 count) override
            {
                if (m_depth == 0)
                {
                    m_capture = std::make_unique<JsonFragmentWriter>();
                    m_capture->beginArray("v", count);
                }
                else
                {
                    m_capture->beginArray(name, count);
                }
                ++m_depth;
            }
            void endArray() override
            {
                m_capture->endArray();
                if (--m_depth == 0)
                {
                    std::string text;
                    m_capture->dump(text);
                    values.emplace_back(nlohmann::json::parse(text)["v"].dump());
                    m_capture.reset();
                }
            }
            void writeBool(const std::string_view name, const bool v) override
            {
                if (m_depth > 0)
                {
                    m_capture->writeBool(name, v);
                    return;
                }
                values.emplace_back(std::int64_t{v});
            }
            void writeU8(const std::string_view name, const u8 v) override
            {
                if (m_depth > 0)
                {
                    m_capture->writeU8(name, v);
                    return;
                }
                values.emplace_back(std::int64_t{v});
            }
            void writeU16(const std::string_view name, const u16 v) override
            {
                if (m_depth > 0)
                {
                    m_capture->writeU16(name, v);
                    return;
                }
                values.emplace_back(std::int64_t{v});
            }
            void writeU32(const std::string_view name, const u32 v) override
            {
                if (m_depth > 0)
                {
                    m_capture->writeU32(name, v);
                    return;
                }
                values.emplace_back(std::int64_t{v});
            }
            void writeU64(const std::string_view name, const u64 v) override
            {
                if (m_depth > 0)
                {
                    m_capture->writeU64(name, v);
                    return;
                }
                values.emplace_back(static_cast<std::int64_t>(v));
            }
            void writeI32(const std::string_view name, const i32 v) override
            {
                if (m_depth > 0)
                {
                    m_capture->writeI32(name, v);
                    return;
                }
                values.emplace_back(std::int64_t{v});
            }
            void writeF32(const std::string_view name, const f32 v) override
            {
                if (m_depth > 0)
                {
                    m_capture->writeF32(name, v);
                    return;
                }
                values.emplace_back(double{v});
            }
            void writeF64(const std::string_view name, const double v) override
            {
                if (m_depth > 0)
                {
                    m_capture->writeF64(name, v);
                    return;
                }
                values.emplace_back(v);
            }
            void writeFloats(const std::string_view name, const std::span<const f32> v) override
            {
                if (m_depth > 0)
                {
                    m_capture->writeFloats(name, v);
                    return;
                }
                if (v.size() == 16) // Mat4
                {
                    values.emplace_back(nlohmann::json(std::vector<f32>(v.begin(), v.end())).dump());
                    return;
                }
                for (const f32 component : v)
                {
                    values.emplace_back(double{component});
                }
            }
            void writeString(const std::string_view name, const std::string_view v) override
            {
                if (m_depth > 0)
                {
                    m_capture->writeString(name, v);
                    return;
                }
                values.emplace_back(std::string(v));
            }
            void writeEntity(const std::string_view name, const Entity v) override
            {
                if (m_depth > 0)
                {
                    m_capture->writeEntity(name, v);
                    return;
                }
                values.emplace_back(static_cast<std::int64_t>(v.id()));
            }
            void writeNull(const std::string_view name) override
            {
                SASSERTM(m_depth > 0, "The schema walk emits no top-level null.");
                m_capture->writeNull(name);
            }

        private:
            std::unique_ptr<JsonFragmentWriter> m_capture;
            u32 m_depth = 0;
        };

        constexpr const char* COMPONENT_TABLES = "snapshot_components";

        std::string sqlIdentifier(const std::string_view identifier) { return "\"" + std::string(identifier) + "\""; }

        class Database
        {
        public:
            explicit Database(const std::string& path, const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
            {
                if (sqlite3_open_v2(path.c_str(), &m_db, flags, nullptr) != SQLITE_OK)
                {
                    m_error = "cannot open " + path + ": " + sqlite3_errmsg(m_db);
                }
            }
            ~Database() { sqlite3_close(m_db); }
            Database(const Database&) = delete;
            Database& operator=(const Database&) = delete;

            [[nodiscard]] const std::string& error() const { return m_error; }

            // null on error
            sqlite3_stmt* prepare(const std::string& sql)
            {
                sqlite3_stmt* statement = nullptr;
                if (m_error.empty() && sqlite3_prepare_v2(m_db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
                {
                    m_error = sqlite3_errmsg(m_db);
                }
                return statement;
            }

            bool insertBlob(sqlite3_stmt* statement, const SnapshotBlob& blob)
            {
                sqlite3_bind_text(statement, 1, blob.name.data(), static_cast<int>(blob.name.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int64(statement, 2, static_cast<std::int64_t>(blob.data.size()));
                sqlite3_bind_blob64(statement, 3, blob.data.data(), blob.data.size(), SQLITE_STATIC);
                if (sqlite3_step(statement) != SQLITE_DONE)
                {
                    m_error = sqlite3_errmsg(m_db);
                }
                sqlite3_reset(statement);
                return m_error.empty();
            }

            bool exec(const std::string& sql)
            {
                if (m_error.empty() && sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
                {
                    m_error = sqlite3_errmsg(m_db);
                }
                return m_error.empty();
            }

            // Creates the table and returns its insert statement (null on error).
            sqlite3_stmt* createTable(const std::string& name, const std::vector<Column>& columns)
            {
                std::string create = "CREATE TABLE " + sqlIdentifier(name) + "(";
                std::string insert = "INSERT INTO " + sqlIdentifier(name) + " VALUES(";
                for (sizet i = 0; i < columns.size(); ++i)
                {
                    create += (i ? "," : "") + sqlIdentifier(columns[i].name) + " " + columns[i].type;
                    insert += i ? ",?" : "?";
                }
                sqlite3_stmt* statement = nullptr;
                if (exec(create + ")") &&
                    sqlite3_prepare_v2(m_db, (insert + ")").c_str(), -1, &statement, nullptr) != SQLITE_OK)
                {
                    m_error = sqlite3_errmsg(m_db);
                }
                return statement;
            }

            bool insert(sqlite3_stmt* statement, const std::vector<Value>& values)
            {
                for (sizet i = 0; i < values.size(); ++i)
                {
                    const int slot = static_cast<int>(i) + 1;
                    if (const auto* integer = std::get_if<std::int64_t>(&values[i]))
                    {
                        sqlite3_bind_int64(statement, slot, *integer);
                    }
                    else if (const auto* real = std::get_if<double>(&values[i]))
                    {
                        sqlite3_bind_double(statement, slot, *real);
                    }
                    else
                    {
                        const std::string& text = std::get<std::string>(values[i]);
                        sqlite3_bind_text(statement, slot, text.data(), static_cast<int>(text.size()),
                                          SQLITE_TRANSIENT);
                    }
                }
                if (sqlite3_step(statement) != SQLITE_DONE)
                {
                    m_error = sqlite3_errmsg(m_db);
                }
                sqlite3_reset(statement);
                return m_error.empty();
            }

        private:
            sqlite3* m_db = nullptr;
            std::string m_error;
        };

        using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

        // Creates the table for schema rows (an entity column first when withEntity) and returns its insert.
        Statement createSchemaTable(Database& db, const std::string& name, const StructSchema& schema,
                                    const bool withEntity)
        {
            std::vector<Column> columns;
            if (withEntity)
            {
                columns.push_back({"entity", "INTEGER"});
            }
            schemaColumns(schema, {}, columns);
            return {db.createTable(name, columns), sqlite3_finalize};
        }

        bool insertSchemaRow(Database& db, sqlite3_stmt* insert, const StructSchema& schema,
                             const std::optional<Entity> entity, const void* row)
        {
            FlatRow flat;
            if (entity)
            {
                flat.values.emplace_back(static_cast<std::int64_t>(entity->id()));
            }
            writeValue(schema, row, flat, FieldSet::All);
            return db.error().empty() && db.insert(insert, flat.values);
        }

        struct Table
        {
            std::string name;
            const StructSchema* schema = nullptr;
            ComponentID component = INVALID_COMPONENT_ID; // or a service table:
            std::string_view service;
            std::string_view serviceTable;
        };

        std::vector<Table> availableTables(const ServiceLocator* services)
        {
            std::vector<Table> tables;
            std::vector<ComponentID> ids(getRegisteredComponentCount());
            ids.resize(getComponentIds(ids.data(), static_cast<u32>(ids.size())));
            for (const ComponentID id : ids)
            {
                const ComponentMetadata& metadata = getComponentMetadata(id);
                if (metadata.schema)
                {
                    tables.push_back({std::string(metadata.name.toView()), metadata.schema, id});
                }
            }
            if (services)
            {
                for (const ServiceInfo& service : services->listServices())
                {
                    for (const ServiceTableInfo& table : service.tables)
                    {
                        tables.push_back({std::string(service.name) + "_" + std::string(table.name), table.rowSchema,
                                          INVALID_COMPONENT_ID, service.name, table.name});
                    }
                }
            }
            return tables;
        }
    } // namespace

    std::string writeSnapshot(const World& world, const ServiceLocator* services, const std::string& path,
                              const std::vector<std::string>& only, IFormatWriter& out, const u32 pageSize)
    {
        std::vector<Table> tables = availableTables(services);
        if (!only.empty())
        {
            for (const std::string& name : only)
            {
                if (std::ranges::none_of(tables, [&](const Table& table) { return table.name == name; }))
                {
                    return "unknown table '" + name + "'";
                }
            }
            std::erase_if(tables,
                          [&](const Table& table) { return std::ranges::find(only, table.name) == only.end(); });
        }

        std::error_code ignored;
        std::filesystem::remove(pathFromUtf8(path), ignored);
        Database db(path);
        // one consistent file per frame point; durability does not matter
        db.exec(std::format("PRAGMA page_size={}; PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; BEGIN;", pageSize));
        sqlite3_stmt* entities = db.createTable("entities", {{"entity", "INTEGER PRIMARY KEY"}});
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> entitiesGuard(entities, sqlite3_finalize);
        // which tables hold one row per entity: appended tables may have an entity column of their own
        sqlite3_stmt* componentTables = db.createTable(COMPONENT_TABLES, {{"name", "TEXT PRIMARY KEY"}});
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> componentTablesGuard(componentTables,
                                                                                              sqlite3_finalize);
        std::unordered_set<u64> listed;

        out.beginArray("tables", static_cast<u32>(tables.size()));
        for (const Table& table : tables)
        {
            const Statement insert =
                createSchemaTable(db, table.name, *table.schema, table.component != INVALID_COMPONENT_ID);
            u64 rows = 0;
            const auto writeRow = [&](const std::optional<Entity> entity, const void* row)
            {
                if (insertSchemaRow(db, insert.get(), *table.schema, entity, row))
                {
                    ++rows;
                }
            };

            if (table.component != INVALID_COMPONENT_ID)
            {
                db.insert(componentTables, {table.name});
                for (const Chunk* chunk : world.getChunksContaining(Aspect(table.component)))
                {
                    const int index = chunk->getComponentIndex(table.component);
                    const std::span<const Entity> chunkEntities = chunk->entities();
                    for (sizet i = 0; i < chunk->size(); ++i)
                    {
                        // const chunk access: a snapshot marks nothing modified
                        writeRow(chunkEntities[i], chunk->getConstComponentDataPtrByIndex(index, i));
                        if (listed.insert(chunkEntities[i].id()).second)
                        {
                            db.insert(entities, {static_cast<std::int64_t>(chunkEntities[i].id())});
                        }
                    }
                }
            }
            else
            {
                services->queryTable(table.service, table.serviceTable,
                                     [&](const void* row) { writeRow(std::nullopt, row); });
            }
            out.beginObject("");
            out.writeString("name", table.name);
            out.writeU64("rows", rows);
            out.endObject();
        }
        out.endArray();
        db.exec("COMMIT;");
        if (!db.error().empty())
        {
            return "snapshot failed: " + db.error();
        }
        out.writeString("path", utf8Path(std::filesystem::absolute(pathFromUtf8(path))));
        return {};
    }

    std::string appendSnapshot(const std::string& path, const std::span<const SnapshotTable> tables,
                               const std::string_view blobTable, const std::span<const SnapshotBlob> blobs)
    {
        Database db(path, SQLITE_OPEN_READWRITE);
        // a journal, so a failed append rolls back instead of corrupting the snapshot already written
        db.exec("PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF; BEGIN;");
        for (const SnapshotTable& table : tables)
        {
            const Statement insert = createSchemaTable(db, table.name, *table.schema, false);
            for (const void* row : table.rows)
            {
                insertSchemaRow(db, insert.get(), *table.schema, std::nullopt, row);
            }
        }
        if (!blobs.empty())
        {
            const std::string name = sqlIdentifier(blobTable);
            db.exec("CREATE TABLE " + name + "(name TEXT PRIMARY KEY, bytes INTEGER, data BLOB)");
            const Statement insert(db.prepare("INSERT INTO " + name + " VALUES(?,?,?)"), sqlite3_finalize);
            for (const SnapshotBlob& blob : blobs)
            {
                if (db.error().empty())
                {
                    db.insertBlob(insert.get(), blob);
                }
            }
        }
        db.exec("COMMIT;");
        return db.error().empty() ? std::string{} : "appending to the snapshot failed: " + db.error();
    }

    std::string readSnapshotEntity(const std::string& path, const u64 entity, IFormatWriter& out)
    {
        Database db(path, SQLITE_OPEN_READONLY);
        std::vector<std::string> tables;
        {
            const Statement list(db.prepare(std::string("SELECT name FROM ") + COMPONENT_TABLES + " ORDER BY name"),
                                 sqlite3_finalize);
            while (list && sqlite3_step(list.get()) == SQLITE_ROW)
            {
                tables.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(list.get(), 0)));
            }
        }
        for (const std::string& table : tables)
        {
            const Statement select(db.prepare("SELECT * FROM " + sqlIdentifier(table) + " WHERE entity = ?"),
                                   sqlite3_finalize);
            if (!select)
            {
                break;
            }
            sqlite3_bind_int64(select.get(), 1, static_cast<std::int64_t>(entity));
            if (sqlite3_step(select.get()) != SQLITE_ROW)
            {
                continue;
            }
            out.beginObject(table);
            for (int column = 0; column < sqlite3_column_count(select.get()); ++column)
            {
                const std::string_view name = sqlite3_column_name(select.get(), column);
                if (name == "entity")
                {
                    continue;
                }
                switch (sqlite3_column_type(select.get(), column))
                {
                case SQLITE_INTEGER:
                    {
                        const std::int64_t value = sqlite3_column_int64(select.get(), column);
                        if (value >= 0)
                        {
                            out.writeU64(name, static_cast<u64>(value));
                        }
                        else
                        {
                            out.writeF64(name, static_cast<double>(value));
                        }
                        break;
                    }
                case SQLITE_FLOAT:
                    out.writeF64(name, sqlite3_column_double(select.get(), column));
                    break;
                case SQLITE_TEXT:
                    out.writeString(name, reinterpret_cast<const char*>(sqlite3_column_text(select.get(), column)));
                    break;
                case SQLITE_BLOB:
                    out.writeU64(name, static_cast<u64>(sqlite3_column_bytes(select.get(), column)));
                    break;
                default:
                    out.writeNull(name);
                    break;
                }
            }
            out.endObject();
        }
        return db.error().empty() ? std::string{} : "reading the snapshot failed: " + db.error();
    }
} // namespace spite
