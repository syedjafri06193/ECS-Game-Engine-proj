#pragma once
// Thin OS layer: shared libraries, file times, thread names.
#include <cstdint>
#include <string>

namespace engine::platform {

using LibraryHandle = void*;

LibraryHandle LoadLibrary(const std::string& path, std::string* error = nullptr);
void          UnloadLibrary(LibraryHandle h);
void*         GetSymbol(LibraryHandle h, const char* name);

template <typename Fn>
Fn GetSymbol(LibraryHandle h, const char* name) { return reinterpret_cast<Fn>(GetSymbol(h, name)); }

// Modification time in nanoseconds, 0 if the file does not exist.
int64_t FileModifiedNs(const std::string& path);
bool    CopyFile(const std::string& from, const std::string& to);
std::string TempDirectory();
std::string SharedLibraryExtension();   // ".so", ".dylib", ".dll"
void    SetCurrentThreadName(const char* name);

}  // namespace engine::platform
