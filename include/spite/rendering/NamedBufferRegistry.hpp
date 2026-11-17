#pragma once

#include "rendering/GraphicsDescs.hpp"
#include "rendering/RenderingExport.hpp"
#include "rendering/RenderResourceHandles.hpp"
#include "base/InternedString.hpp"


namespace spite
{
    class IRenderDevice;

    class SPITE_RENDERING_API NamedBufferRegistry
    {
    public:
        NamedBufferRegistry(IRenderDevice& device);

        NamedBufferRegistry(const NamedBufferRegistry&) = delete;
        NamedBufferRegistry& operator=(const NamedBufferRegistry&) = delete;
        NamedBufferRegistry(NamedBufferRegistry&&) = delete;
        NamedBufferRegistry& operator=(NamedBufferRegistry&&) = delete;

        // Creates a buffer and registers it with a unique name.
        // Asserts if a buffer with the same name already exists.
        BufferHandle createBuffer(InternedString name, const BufferDesc& desc);

        BufferHandle getOrCreateBuffer(InternedString name, const BufferDesc& desc);

        BufferHandle getBuffer(InternedString name) const;

        const BufferDesc& getBufferDesc(InternedString name) const;

    private:
        IRenderDevice& m_device;
        std::unordered_map<InternedString, std::pair<BufferHandle, BufferDesc>> m_namedBuffers;
    };
} // namespace spite
