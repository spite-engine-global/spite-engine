#ifndef GPU_ATTRIBUTES_SLANG
#define GPU_ATTRIBUTES_SLANG

[__AttributeUsage(_AttributeTargets.Struct)]
struct CppStructAttribute {};

[__AttributeUsage(_AttributeTargets.Var)]
struct CppConstantAttribute {};

// [GpuTable("Name")] on a [CppStruct]: render graph buffer Name holds an array
// of the struct. The gpuBuffer control query reads its rows back decoded with
// the reflected layout (include/spite/rendering/GpuStructLayouts.hpp).
[__AttributeUsage(_AttributeTargets.Struct)]
struct GpuTableAttribute
{
    string buffer;
};

#endif
