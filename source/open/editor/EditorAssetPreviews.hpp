#pragma once

#include <filesystem>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

namespace spite
{
    class EditorAssetPreviews
    {
    public:
        enum class Status
        {
            Missing,
            Pending,
            Ready,
            Error
        };
        struct Result
        {
            Status status = Status::Missing;
            std::filesystem::path path;
            std::string error;
        };

        explicit EditorAssetPreviews(std::filesystem::path cacheDirectory, unsigned maxSize = 256);
        ~EditorAssetPreviews();
        EditorAssetPreviews(const EditorAssetPreviews&) = delete;
        EditorAssetPreviews& operator=(const EditorAssetPreviews&) = delete;

        // key identifies a visible asset; bakedTexture and dependencies must be the actual baked files.
        void request(const std::string& key, const std::filesystem::path& bakedTexture,
                     const std::vector<std::filesystem::path>& dependencies = {});
        [[nodiscard]] Result status(const std::string& key) const;
        void cancel(const std::string& key);
        void clear();
        // Starts at most one worker and collects at most one completion; call on the editor thread.
        void update();

    private:
        struct Entry
        {
            std::filesystem::path source;
            std::vector<std::filesystem::path> dependencies;
            Result result;
            unsigned long long generation;
        };
        struct Completed
        {
            std::string key;
            unsigned long long generation;
            Result result;
        };
        std::filesystem::path m_cacheDirectory;
        unsigned m_maxSize;
        std::unordered_map<std::string, Entry> m_entries;
        std::vector<std::string> m_queue;
        std::future<Completed> m_worker;
        unsigned long long m_generation = 0;
    };
} // namespace spite
