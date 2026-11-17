# Overrides the built-in triplet of the same name. shader-slang ships only
# prebuilt DLLs and rejects static linkage; it is used by build tools, not
# linked into engine modules.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "shader-slang")
    set(VCPKG_CRT_LINKAGE dynamic)
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
