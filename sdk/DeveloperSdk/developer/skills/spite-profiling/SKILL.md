---
name: spite-profiling
description: Use when measuring SPITE frame time or comparing performance.
---

# Measure performance

## Before the capture

- Read `config section=application.timing`. For `vsyncEnabled`, `limitFPS` and `targetFPS`, compare `value` with `running`. If they disagree, do not capture.
- A bounded run starts with vsync off, whatever the saved setting says. Read the running value, not the project file.
- Make temporary changes with `setConfig`. Check its apply status and query the running values again.
- Never call `saveConfig` for an experiment. Restore the previous values afterwards.

## Capture

1. Discover `profileStart`, `profileStop` and `profileStatus`.
2. Start, run the workload, then stop.
3. Wait until `profileStatus` reports `stop.state` as `written`.
4. Check `lastReportValid` and `lastWarning`.
5. Compare the report's environment, including the actual present mode, and its provenance with the run you intended.

Use the engine's zones report. Never substitute timers of your own.

For a live session already owned by the user, inspect its existing capture through discovered `profileGpu` and `profileCallTree` queries instead of launching a replacement host. Label per-pass GPU observations separately from controlled comparisons. `profileClose` releases the capture handle and keeps its files; deleting files is a separate cleanup action within the user's authorized scope.

If rendering reports descriptor exhaustion, discover the graph and bindless `stats` tables through `tables`/`tableRows`. Graph `ready`, `compileFailures`, `sampledDemand` and `storageDemand` describe build status; `texturesPending` and `storageImagesPending` are included in live descriptor occupancy; `texturesRetired` and `storageImagesRetired` are permanently retired slots outside it; `rebuilds*` count graph rebuilds by cause. Allow retirement/recovery before captures. This does not establish the cause of a historical device-loss crash.

## Report

- Capture on a quiet machine. Record the device, the workload and what each metric means.
- Rank results only when they come from the same workload on a quiet machine.
- Call a pacing value observed only when you measured it. A requested value is not an observation.
- With cluster LOD on, the object-cull passes (`Cull_Objects_*`, and each shadow cascade's or light map's `*_Objects`) grow with the clusters of the meshes each view and shadow cascade contains, not with what is drawn. Compare a capture with `graphics.lod` off before blaming triangles. See "Full-detail LOD for chosen renderables" in the guide.

Sessions are in the spite-session skill. More detail is in `<sdk>/guides/AgentWorkflows.md` (`<sdk>` is `SdkRoot` from `spite project`), section "Pacing and profiling".
