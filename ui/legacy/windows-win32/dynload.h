// Run-time loading helpers: everything newer than Windows 2000 (or absent on some editions) is
// looked up with GetProcAddress so that it never appears in the import table of the executable.
#ifndef LOOMORA_WIN32_DYNLOAD_H
#define LOOMORA_WIN32_DYNLOAD_H

#include <windows.h>

#include <string>

namespace dynload {

// Address of an exported function, or NULL. FARPROC cannot be cast straight to another function
// type without a warning; going through void * is accepted by GCC and clang.
template <typename Function> Function Resolve(HMODULE module, const char *name) {
    if (module == NULL)
        return NULL;
    return reinterpret_cast<Function>(reinterpret_cast<void *>(GetProcAddress(module, name)));
}

// LoadLibrary of a DLL from the system directory only (never from the application folder).
// NULL when the DLL is absent. The caller releases it with FreeLibrary.
inline HMODULE LoadSystemLibrary(const wchar_t *name) {
    wchar_t directory[MAX_PATH];
    const UINT n = GetSystemDirectoryW(directory, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return NULL;
    std::wstring path(directory, n);
    path += L'\\';
    path += name;
    return LoadLibraryW(path.c_str());
}

} // namespace dynload

#endif
