#include "core/platform/platform.h"
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef LoadLibrary
#undef CopyFile
#else
#include <dlfcn.h>
#include <pthread.h>
#endif

namespace engine::platform {

namespace fs = std::filesystem;

LibraryHandle LoadLibrary(const std::string& path, std::string* error)
{
#if defined(_WIN32)
    HMODULE h = ::LoadLibraryA(path.c_str());
    if (!h && error) *error = "LoadLibrary failed: " + std::to_string(::GetLastError());
    return reinterpret_cast<LibraryHandle>(h);
#else
    // RTLD_LOCAL: a module's symbols never leak into the global namespace,
    // so two versions can never cross-resolve.
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h && error) *error = ::dlerror();
    return h;
#endif
}

void UnloadLibrary(LibraryHandle h)
{
    if (!h) return;
#if defined(_WIN32)
    ::FreeLibrary(reinterpret_cast<HMODULE>(h));
#else
    ::dlclose(h);
#endif
}

void* GetSymbol(LibraryHandle h, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
    return ::dlsym(h, name);
#endif
}

int64_t FileModifiedNs(const std::string& path)
{
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

bool CopyFile(const std::string& from, const std::string& to)
{
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

std::string TempDirectory()
{
    std::error_code ec;
    auto p = fs::temp_directory_path(ec);
    return ec ? std::string(".") : p.string();
}

std::string SharedLibraryExtension()
{
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

void SetCurrentThreadName(const char* name)
{
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    (void)name;
#endif
}

}  // namespace engine::platform
