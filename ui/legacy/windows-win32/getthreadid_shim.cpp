// Link-time shim for the 32-bit (Windows 2000) executable: KERNEL32!GetThreadId.
//
// GetThreadId exists only since Windows Vista / Server 2003 (not on Windows 2000, not on XP x86).
// The loader of an older system refuses an executable whose import table names it. GCC 13's
// libgcc pulls it in: libstdc++.a(eh_alloc.o), the pool used to throw exceptions, needs
// __gthr_win32_mutex_init_function, so the linker takes the whole libgcc.a(gthr-win32.o), and
// that object also holds __gthr_win32_equal(), which calls GetThreadId. This program never calls
// it (no std::thread), but the import would still be in the table.
//
// The two definitions below satisfy the references of gthr-win32.o inside the executable
// (the import-pointer variable that "call *__imp__GetThreadId@4" goes through, and the function
// it points to), so the linker does not take the import from libkernel32.a. The function behaves
// as the real one if it is ever called: it looks GetThreadId up when called (present on newer
// systems) and otherwise asks the kernel for the thread's identifier (NtQueryInformationThread,
// the same information, available since Windows NT 3.1). It uses only GetModuleHandleA and
// GetProcAddress, which the executable imports already, so the import table gains nothing.
// See docs/implementation/adr/0002-win2000-getthreadid-shim.md. Only used by the i686 build (see CMakeLists.txt).

#include <windows.h>

namespace {

typedef DWORD(WINAPI *GetThreadIdFn)(HANDLE);
typedef LONG(NTAPI *QueryThreadFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);

// THREAD_BASIC_INFORMATION of winternl.h / ntddk.h (32-bit layout).
struct ThreadBasicInformation {
    LONG exitStatus;
    PVOID tebBaseAddress;
    PVOID uniqueProcess; // CLIENT_ID
    PVOID uniqueThread;
    ULONG_PTR affinityMask;
    LONG priority;
    LONG basePriority;
};

// FARPROC cannot be cast straight to another function type without a warning
// (-Wcast-function-type); going through void * is accepted by GCC in C++11.
void *Lookup(const char *module, const char *name) {
    const HMODULE handle = GetModuleHandleA(module);
    if (handle == NULL)
        return NULL;
    return reinterpret_cast<void *>(GetProcAddress(handle, name));
}

} // namespace

// Global symbol "_GetThreadId@4": what libkernel32.a would define (the stdcall name of the real
// function). The C++ name is different so that it cannot clash with a declaration in <windows.h>.
extern "C" DWORD WINAPI loomoraThreadId(HANDLE thread) __asm__("_GetThreadId@4");

extern "C" DWORD WINAPI loomoraThreadId(HANDLE thread) {
    void *const native = Lookup("kernel32.dll", "GetThreadId");
    if (native != NULL)
        return reinterpret_cast<GetThreadIdFn>(native)(thread);

    // Windows 2000 / XP: the documented behaviour of GetThreadId, 0 on failure.
    void *const query = Lookup("ntdll.dll", "NtQueryInformationThread");
    if (query == NULL)
        return 0;
    ThreadBasicInformation info = ThreadBasicInformation();
    const LONG status = reinterpret_cast<QueryThreadFn>(query)(
        thread, 0 /* ThreadBasicInformation */, &info, sizeof(info), NULL);
    if (status < 0)
        return 0;
    return static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(info.uniqueThread));
}

// The "import address table entry" gthr-win32.o goes through. A constant initializer: no
// start-up code.
extern "C" {
extern GetThreadIdFn const loomoraImpGetThreadId __asm__("__imp__GetThreadId@4");
GetThreadIdFn const loomoraImpGetThreadId = &loomoraThreadId;
}
