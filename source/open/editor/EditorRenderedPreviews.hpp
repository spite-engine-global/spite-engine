#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include "base/InternedString.hpp"
#include "rendering/Extent2D.hpp"

namespace spite
{
    class Engine;

    class EditorRenderedPreviews
    {
    public:
        enum class Status
        {
            Idle,
            Loading,
            Capturing,
            Ready,
            Missing,
            Failed
        };
        explicit EditorRenderedPreviews(Engine& engine, u32 viewIndex = 2);
        ~EditorRenderedPreviews();
        EditorRenderedPreviews(const EditorRenderedPreviews&) = delete;
        EditorRenderedPreviews& operator=(const EditorRenderedPreviews&) = delete;

        void requestModel(InternedString name, std::filesystem::path path);
        void requestMaterial(InternedString name, std::filesystem::path path);
        void requestCachedModel(InternedString name, std::filesystem::path cacheDirectory,
                                bool generateIfMissing = false);
        void requestCachedMaterial(InternedString name, std::filesystem::path cacheDirectory,
                                   bool generateIfMissing = false);
        void openModel(InternedString name);
        void openMaterial(InternedString name);
        void openScene(const std::filesystem::path& path);
        void requestCachedScene(const std::filesystem::path& source, const std::filesystem::path& cacheDirectory);
        void setViewportExtent(Extent2D extent);
        void orbit(float yawDelta, float pitchDelta);
        void zoom(float wheelDelta);
        void update();
        void cancel();
        [[nodiscard]] Status status() const;
        [[nodiscard]] const std::filesystem::path& path() const;
        [[nodiscard]] const std::string& error() const;

    private:
        void startModel(InternedString name, std::filesystem::path path, InternedString material);
        void requestCached(InternedString name, std::filesystem::path cacheDirectory, bool material,
                           bool generateIfMissing);
        struct State;
        std::shared_ptr<State> m_state;
    };
} // namespace spite
