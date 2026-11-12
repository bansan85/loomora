#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
        QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
} PMC;
typedef BOOL(WINAPI *Fn)(HANDLE, PMC *, DWORD);

static Fn g_fn;
static void show(const char *label) {
    PMC c;
    memset(&c, 0, sizeof(c));
    c.cb = sizeof(c);
    BOOL ok = g_fn(GetCurrentProcess(), &c, c.cb);
    printf("%-28s ok=%d peakWS=%lu KB  WS=%lu KB  pagefile=%lu KB  peakpagefile=%lu KB faults=%lu\n",
           label, ok, (unsigned long)(c.PeakWorkingSetSize / 1024),
           (unsigned long)(c.WorkingSetSize / 1024), (unsigned long)(c.PagefileUsage / 1024),
           (unsigned long)(c.PeakPagefileUsage / 1024), (unsigned long)c.PageFaultCount);
}

int main(void) {
    HMODULE m = LoadLibraryA("psapi.dll");
    g_fn = (Fn)GetProcAddress(m, "GetProcessMemoryInfo");
    if (!g_fn) {
        printf("no GetProcessMemoryInfo\n");
        return 1;
    }
    show("start");
    volatile char *p = (volatile char *)VirtualAlloc(NULL, 8 * 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    show("after VirtualAlloc 8 MB");
    unsigned long sum = 0;
    for (size_t i = 0; i < 8 * 1024 * 1024; i += 4096) { p[i] = 1; sum += p[i]; }
    printf("touched, sum=%lu\n", sum);
    show("after touching 8 MB");
    VirtualFree((void *)p, 0, MEM_RELEASE);
    show("after free");
    LoadLibraryA("user32.dll");
    LoadLibraryA("gdi32.dll");
    show("after user32+gdi32");
    return 0;
}
