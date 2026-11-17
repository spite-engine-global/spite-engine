#pragma once
#include <system_error>
#include <string>
#include <utility>

#include "base/Platform.hpp"
#include "BaseExport.hpp"

namespace spite
{
    enum class ErrorCode : u32
    {
        None = 0,

        FileNotFound = 100,
        FileReadFailed = 101,
        FileWriteFailed = 102,
        InvalidPath = 103,

        InvalidMagicNumber = 200,
        UnsupportedVersion = 201,
        CorruptedData = 202,
        UnexpectedEndOfFile = 203,
        ComponentNotRegistered = 204,
        JsonParseFailed = 205,
        JsonConversionFailed = 206,

        AssetNotFound = 300,
        AssetAlreadyLoading = 301,
        AssetLoadFailed = 302,
        AssetSaveFailed = 303,
        InvalidAssetType = 304,

        SceneParsingFailed = 400,
        EntityDeserializationFailed = 401,
        SceneCollectFailed = 402,

        // Internal errors (9xx)
        InternalError = 900,
        NotImplemented = 901,
        Cancelled = 902, // work dropped on purpose (e.g. at teardown): not a failure
    };

    // No SPITE_BASE_API: every member is header-inline and the type carries no
    // out-of-line definitions, so consumers compile it locally and the DLL
    // exports nothing for it (an export annotation here would demand import
    // thunks for implicitly-defined special members the DLL never emits).
    struct Error
    {
        ErrorCode code = ErrorCode::None;
        std::error_code systemError;
        std::string message;

        Error() = default;

        explicit Error(std::string msg) : code(ErrorCode::InternalError), message(std::move(msg)) {}

        Error(ErrorCode errorCode, std::string msg, std::error_code error = {}) :
            code(errorCode), systemError(error), message(std::move(msg))
        {
        }

        bool isFileError() const { return static_cast<u32>(code) >= 100 && static_cast<u32>(code) < 200; }
        bool isSerializationError() const { return static_cast<u32>(code) >= 200 && static_cast<u32>(code) < 300; }
        bool isAssetError() const { return static_cast<u32>(code) >= 300 && static_cast<u32>(code) < 400; }
        bool isSceneError() const { return static_cast<u32>(code) >= 400 && static_cast<u32>(code) < 500; }
    };

    inline Error makeError(ErrorCode code, std::string message, std::error_code systemError = {})
    {
        return Error{code, std::move(message), systemError};
    }
} // namespace spite
