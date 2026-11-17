# Building tooling from an SDK

Configure this directory with a C++23 compiler matching the SDK ABI and CMake's
`CMAKE_PREFIX_PATH` pointing to the SDK and dependencies. The editor requires
nlohmann-json and stb; the Windows CLI also requires SQLite3. Build the
`SpiteEditorSource` target in Release. It uses SDK headers and import libraries
without a core source tree. Use the matching SDK's stock editor host.

For example, from a Windows SDK root in the pinned MSVC x64 developer environment,
with Ninja and the dependencies installed:

```powershell
cmake -S tooling -B tooling-build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=$PWD;C:/vcpkg/installed/x64-windows-static"
cmake --build tooling-build
cmake --install tooling-build --prefix "$PWD"
```

Installation replaces the SDK's editor module while keeping its matching host
and core runtime binaries. Launch `bin/SpiteEditor.exe`
with the SDK root as `--content-root`. The profiler reads and compares captures
through the existing public engine APIs; capture storage and analysis remain in
the core binary. A separately packaged source CLI can be enabled explicitly with
`SPITE_BUILD_SOURCE_CLI`.
