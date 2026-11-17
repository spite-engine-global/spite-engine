#include <atomic>
#include <filesystem>
#include <memory>
#include <utility>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "base/Logging.hpp"
#include "engine/AnimationCommands.hpp"

namespace spite
{
    namespace
    {
        // A root that does not exist yet (a project with no controller folder) is looked for again every this many
        // calls
        constexpr u32 ARM_RETRY_CALLS = 64;

        // The notification handle and the counter every caller reads; a notification is consumed by whichever caller
        // sees it first, so the counter (not the handle) is what each AnimationWorld compares against. Frame thread
        // only: the calls, the handle and arm() are plain fields, and the function runs under the loader's shared
        // read lock, so that invariant is all that protects them.
        struct ControllerWatch
        {
            std::filesystem::path root;
            HANDLE handle = INVALID_HANDLE_VALUE;
            std::atomic<u64> version{0};
            u32 calls = 0;
            bool warned = false; // the arm failure of this streak is logged; a success starts a new streak

            ~ControllerWatch()
            {
                if (handle != INVALID_HANDLE_VALUE)
                {
                    FindCloseChangeNotification(handle);
                }
            }

            void arm()
            {
                std::error_code error;
                if (!std::filesystem::is_directory(root, error))
                {
                    return;
                }
                handle = FindFirstChangeNotificationW(root.c_str(), TRUE,
                                                      FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
                                                          FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE);
                if (handle != INVALID_HANDLE_VALUE)
                {
                    warned = false;
                }
                else if (!std::exchange(warned, true))
                {
                    SWARNING_LOG("animation: cannot watch '{}': edited controllers are not picked up", root.string());
                }
            }
        };
    } // namespace

    std::function<u64()> animationControllerVersion(const std::filesystem::path& root)
    {
        auto watch = std::make_shared<ControllerWatch>();
        watch->root = root;
        watch->arm();
        return [watch]() -> u64
        {
            if (watch->handle == INVALID_HANDLE_VALUE)
            {
                // a folder made after the engine started is watched from the first look that finds it, and what was
                // written before that counts as a change
                if (++watch->calls % ARM_RETRY_CALLS == 0)
                {
                    watch->arm();
                    if (watch->handle != INVALID_HANDLE_VALUE)
                    {
                        watch->version.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            else if (WaitForSingleObject(watch->handle, 0) == WAIT_OBJECT_0)
            {
                if (!FindNextChangeNotification(watch->handle))
                {
                    // the folder was deleted: the handle never signals again, and it keeps the folder from being made
                    // anew; closing it lets the retry above watch the new one
                    SWARNING_LOG("animation: '{}' is gone; watching again when it returns", watch->root.string());
                    FindCloseChangeNotification(watch->handle);
                    watch->handle = INVALID_HANDLE_VALUE;
                }
                watch->version.fetch_add(1, std::memory_order_relaxed);
            }
            return watch->version.load(std::memory_order_relaxed);
        };
    }
} // namespace spite
