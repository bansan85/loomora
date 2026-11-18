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

#ifdef _WIN64
// 64-bit (XP x64 floor): GetThreadId exists there, only the condition variables below are shimmed, and
// the names of the import entries are not decorated.
#define LOOMORA_IMP(name, bytes) "__imp_" name
#else
#define LOOMORA_IMP(name, bytes) "__imp__" name "@" #bytes
#endif

#ifndef _WIN64
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
#endif // !_WIN64

// FARPROC cannot be cast straight to another function type without a warning
// (-Wcast-function-type); going through void * is accepted by GCC in C++11.
void *Lookup(const char *module, const char *name) {
    const HMODULE handle = GetModuleHandleA(module);
    if (handle == NULL)
        return NULL;
    return reinterpret_cast<void *>(GetProcAddress(handle, name));
}

} // namespace

#ifndef _WIN64
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
#endif // !_WIN64

// ---- condition variables (Windows Vista and later) -------------------------------------------------
//
// GCC 13's libstdc++.a(guard.o) (the guard of a function-local static, __cxa_guard_acquire) needs
// __gthr_win32_cond_*, so the linker takes libgcc.a(gthr-win32-cond.o), which imports
// KERNEL32!InitializeConditionVariable, SleepConditionVariableCS, WakeConditionVariable and
// WakeAllConditionVariable: none of them exists before Windows Vista (found by T059, spec 002, once
// tools/check_imports.py read the import table of the objdump of binutils 2.39 and later). They are
// only called when two threads meet on the guard of the same static, which a single-threaded program
// never does. The same technique as above: define the import-address-table entries inside the
// executable. Each function calls the real one when kernel32 has it (Vista and later); otherwise a
// condition variable that is waited on from a loop (as guard.o does) is served by releasing the
// critical section, sleeping 1 ms and taking it again, a spurious wake-up that callers must accept,
// and the wake-up functions do nothing. Only used by the i686 build (Windows 2000 floor).

namespace {

typedef VOID(WINAPI *CondInitFn)(PVOID);
typedef BOOL(WINAPI *CondSleepFn)(PVOID, LPCRITICAL_SECTION, DWORD);
typedef VOID(WINAPI *CondWakeFn)(PVOID);

} // namespace

extern "C" VOID WINAPI loomoraCondInit(PVOID cv) {
    void *const native = Lookup("kernel32.dll", "InitializeConditionVariable");
    if (native != NULL)
        reinterpret_cast<CondInitFn>(native)(cv);
    else
        *static_cast<PVOID *>(cv) = NULL; // CONDITION_VARIABLE_INIT: one zeroed pointer
}

extern "C" BOOL WINAPI loomoraCondSleep(PVOID cv, LPCRITICAL_SECTION section, DWORD milliseconds) {
    void *const native = Lookup("kernel32.dll", "SleepConditionVariableCS");
    if (native != NULL)
        return reinterpret_cast<CondSleepFn>(native)(cv, section, milliseconds);
    LeaveCriticalSection(section);
    Sleep(1);
    EnterCriticalSection(section);
    return TRUE;
}

extern "C" VOID WINAPI loomoraCondWake(PVOID cv) {
    void *const native = Lookup("kernel32.dll", "WakeConditionVariable");
    if (native != NULL)
        reinterpret_cast<CondWakeFn>(native)(cv);
}

extern "C" VOID WINAPI loomoraCondWakeAll(PVOID cv) {
    void *const native = Lookup("kernel32.dll", "WakeAllConditionVariable");
    if (native != NULL)
        reinterpret_cast<CondWakeFn>(native)(cv);
}

extern "C" {
extern CondInitFn const loomoraImpCondInit __asm__(LOOMORA_IMP("InitializeConditionVariable", 4));
CondInitFn const loomoraImpCondInit = &loomoraCondInit;
extern CondSleepFn const loomoraImpCondSleep __asm__(LOOMORA_IMP("SleepConditionVariableCS", 12));
CondSleepFn const loomoraImpCondSleep = &loomoraCondSleep;
extern CondWakeFn const loomoraImpCondWake __asm__(LOOMORA_IMP("WakeConditionVariable", 4));
CondWakeFn const loomoraImpCondWake = &loomoraCondWake;
extern CondWakeFn const loomoraImpCondWakeAll __asm__(LOOMORA_IMP("WakeAllConditionVariable", 4));
CondWakeFn const loomoraImpCondWakeAll = &loomoraCondWakeAll;
}
