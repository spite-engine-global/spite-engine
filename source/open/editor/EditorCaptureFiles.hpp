#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>
#include "editor/EditorExport.hpp"
#include "engine/CaptureStore.hpp"
namespace spite
{
    class Engine;
    // Cold file actions live with the model; an optional view detaches its providers before destroying their data.
    class SPITE_EDITOR_API EditorCaptureFiles
    {
    public:
        explicit EditorCaptureFiles(Engine& engine);
        ~EditorCaptureFiles();
        void setView(std::function<CaptureId()> selected, std::function<std::vector<CaptureFile>()> files,
                     std::function<void(const std::string&)> refresh);
        void setReveal(std::function<bool(const std::filesystem::path&)> reveal, std::function<std::uint64_t()> nowMs);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
