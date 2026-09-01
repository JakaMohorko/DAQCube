#include <daqcube_module/resource_extractor.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <filesystem>

namespace daq::modules::daqcube_module
{
    namespace fs = std::filesystem;

    namespace
    {
        // HMODULE of this module dll (not the hosting process exe)
        HMODULE thisModule()
        {
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&thisModule),
                               &module);
            return module;
        }

        bool extractOne(HMODULE module, int resourceId, const fs::path& target, std::string& errorOut)
        {
            const HRSRC resource = FindResourceA(module, MAKEINTRESOURCEA(resourceId), MAKEINTRESOURCEA(10) /*RT_RCDATA*/);
            if (!resource)
            {
                errorOut = "embedded resource " + std::to_string(resourceId) + " not found in the module dll";
                return false;
            }
            const DWORD size = SizeofResource(module, resource);
            const HGLOBAL handle = LoadResource(module, resource);
            const void* data = handle ? LockResource(handle) : nullptr;
            if (!data || size == 0)
            {
                errorOut = "embedded resource " + std::to_string(resourceId) + " could not be loaded";
                return false;
            }

            std::error_code ec;
            if (fs::exists(target, ec) && fs::file_size(target, ec) == size)
                return true;  // already extracted (version dir makes staleness impossible)

            const fs::path temp = target.string() + ".tmp";
            if (FILE* file = _wfopen(temp.c_str(), L"wb"))
            {
                const bool written = std::fwrite(data, 1, size, file) == size;
                std::fclose(file);
                if (written)
                {
                    fs::rename(temp, target, ec);
                    if (!ec || (fs::exists(target, ec) && fs::file_size(target, ec) == size))
                        return true;  // a concurrent extraction winning the rename is fine
                }
                fs::remove(temp, ec);
            }
            errorOut = "failed to write " + target.string();
            return false;
        }
    }

    bool extractEmbeddedResource(int resourceId, const char* fileName, std::string& pathOut, std::string& errorOut)
    {
        const HMODULE module = thisModule();
        if (!module)
        {
            errorOut = "failed to resolve the module dll handle";
            return false;
        }

        wchar_t localAppData[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            errorOut = "LOCALAPPDATA is not set";
            return false;
        }

        const auto version = std::to_string(GAME_MODULE_MAJOR_VERSION) + "." + std::to_string(GAME_MODULE_MINOR_VERSION) +
                             "." + std::to_string(GAME_MODULE_PATCH_VERSION);
        const fs::path dir = fs::path(localAppData) / "DAQCube" / version;

        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec)
        {
            errorOut = "failed to create " + dir.string() + ": " + ec.message();
            return false;
        }

        const fs::path target = dir / fileName;
        if (!extractOne(module, resourceId, target, errorOut))
            return false;

        pathOut = target.string();
        return true;
    }
}
