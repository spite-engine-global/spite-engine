#include "EcsTables.hpp"

#include <memory>

#include "base/ServiceLocator.hpp"
#include "ecs/Chunk.hpp"

namespace spite
{
    namespace
    {
        // A key that names the archetype in both tables; buffer is reused across rows.
        void componentKey(const Aspect& aspect, std::string& out)
        {
            out.clear();
            for (const ComponentID id : aspect.getComponentIds())
            {
                if (!out.empty())
                {
                    out += ',';
                }
                out += getComponentName(id);
            }
        }

        void joinNames(const std::vector<ComponentID>& ids, std::string& out)
        {
            out.clear();
            for (const ComponentID id : ids)
            {
                out += out.empty() ? "" : ",";
                out += getComponentName(id);
            }
        }

        void joinSystems(const std::vector<SystemName>& names, std::string& out)
        {
            out.clear();
            for (const SystemName& name : names)
            {
                out += out.empty() ? "" : ",";
                if (name.moduleName && name.moduleName[0])
                {
                    out += name.moduleName;
                    out += "::";
                }
                out += name.systemName;
            }
        }

        void fillPhase(const std::optional<SystemPhaseDescriptor>& phase, u64& stage, std::string& stageName,
                       std::string& runsAfter, std::string& reads, std::string& writes)
        {
            stage = phase ? phase->stage : 0;
            stageName.clear();
            runsAfter.clear();
            reads.clear();
            writes.clear();
            if (!phase)
            {
                return;
            }
            const char* constant = CoreExecutionStages::constantName(phase->stage);
            stageName = constant ? constant : std::to_string(phase->stage);
            joinSystems(phase->runsAfter, runsAfter);
            joinNames(phase->reads, reads);
            joinNames(phase->writes, writes);
        }
    } // namespace

    void EcsTables::attach(const World& world, ServiceLocator& services)
    {
        if (services.contains<EcsTables>())
        {
            return;
        }
        services.registerService<EcsTables>(std::make_shared<EcsTables>(EcsTables{world}));
        services.registerTable<EcsTables>("archetypes", ArchetypeRow::spiteSchema(),
                                          [](const EcsTables& tables, const TableRowSink& sink)
                                          {
                                              ArchetypeRow row;
                                              for (const ArchetypeStats& stats : tables.world.getArchetypeStats())
                                              {
                                                  componentKey(stats.aspect, row.components);
                                                  row.componentCount = stats.componentCount;
                                                  row.entityCount = stats.entityCount;
                                                  row.chunkCount = stats.chunkCount;
                                                  row.memoryBytes = stats.memoryBytes;
                                                  row.wastedBytes = stats.wastedMemoryUnusedSlots;
                                                  row.utilization = stats.chunkUtilization;
                                                  sink(&row);
                                              }
                                          });
        services.registerTable<EcsTables>("chunks", ChunkRow::spiteSchema(),
                                          [](const EcsTables& tables, const TableRowSink& sink)
                                          {
                                              ChunkRow row;
                                              for (const ArchetypeStats& stats : tables.world.getArchetypeStats())
                                              {
                                                  componentKey(stats.aspect, row.archetype);
                                                  row.index = 0;
                                                  for (const Chunk* chunk :
                                                       tables.world.getArchetypeChunks(stats.aspect))
                                                  {
                                                      row.entityCount = chunk->size();
                                                      row.capacity = chunk->capacity();
                                                      row.memoryBytes = chunk->getStorageBlockSize();
                                                      row.wastedBytes = chunk->unusedSlotBytes();
                                                      sink(&row);
                                                      ++row.index;
                                                  }
                                              }
                                          });
        services.registerTable<EcsTables>("systems", SystemRow::spiteSchema(),
                                          [](const EcsTables& tables, const TableRowSink& sink)
                                          {
                                              SystemRow row;
                                              for (const SystemDescriptor& system : tables.world.getSystems())
                                              {
                                                  row.module = system.moduleName ? system.moduleName : "";
                                                  row.name = system.name;
                                                  row.enabled = system.enabled;
                                                  row.required = system.required;
                                                  fillPhase(system.update, row.updateStage, row.updateStageName,
                                                            row.updateRunsAfter, row.updateReads, row.updateWrites);
                                                  fillPhase(system.fixedUpdate, row.fixedStage, row.fixedStageName,
                                                            row.fixedRunsAfter, row.fixedReads, row.fixedWrites);
                                                  row.updateAvgMs = system.updateTiming.avgMs;
                                                  row.updateMaxMs = system.updateTiming.maxMs;
                                                  row.fixedAvgMs = system.fixedTiming.avgMs;
                                                  row.fixedMaxMs = system.fixedTiming.maxMs;
                                                  row.phases = system.update
                                                      ? (system.fixedUpdate ? "update,fixed" : "update")
                                                      : "fixed";
                                                  sink(&row);
                                              }
                                          });
    }

    void EcsTables::detach(ServiceLocator& services) { services.unregisterService<EcsTables>(); }
} // namespace spite
