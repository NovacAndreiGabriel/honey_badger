#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
    #define _DEFAULT_SOURCE
#endif

#include <stdlib.h>

#include "large_pages.h"

#if defined(_WIN32)
    #if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0601
        #undef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601
    #endif
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>

#elif defined(__linux__) && !defined(__ANDROID__)

    #include <sys/mman.h>

#endif

#if defined(_WIN32)

typedef BOOL (WINAPI *OpenProcessTokenFn)(HANDLE, DWORD, PHANDLE);
typedef BOOL (WINAPI *LookupPrivilegeValueAFn)(LPCSTR, LPCSTR, PLUID);
typedef BOOL (WINAPI *AdjustTokenPrivilegesFn)(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD);

static void *alloc_windows_large_pages(u64 size) {
    SIZE_T large_page_size = GetLargePageMinimum();
    if (!large_page_size)
        return NULL;

    HMODULE advapi32 = GetModuleHandleA("advapi32.dll");
    if (!advapi32)
        advapi32 = LoadLibraryA("advapi32.dll");
    if (!advapi32)
        return NULL;

    OpenProcessTokenFn open_process_token =
        (OpenProcessTokenFn)(void (*)(void))GetProcAddress(advapi32, "OpenProcessToken");
    LookupPrivilegeValueAFn lookup_privilege_value =
        (LookupPrivilegeValueAFn)(void (*)(void))GetProcAddress(advapi32, "LookupPrivilegeValueA");
    AdjustTokenPrivilegesFn adjust_token_privileges =
        (AdjustTokenPrivilegesFn)(void (*)(void))GetProcAddress(advapi32, "AdjustTokenPrivileges");
    if (!open_process_token || !lookup_privilege_value || !adjust_token_privileges)
        return NULL;

    HANDLE token;
    if (!open_process_token(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return NULL;

    void *mem = NULL;
    LUID luid;

    if (lookup_privilege_value(NULL, "SeLockMemoryPrivilege", &luid)) {
        TOKEN_PRIVILEGES tp = { 0 };
        TOKEN_PRIVILEGES prev_tp = { 0 };
        DWORD prev_tp_len = 0;

        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        if (adjust_token_privileges(token, FALSE, &tp, sizeof(tp), &prev_tp, &prev_tp_len)
         && GetLastError() == ERROR_SUCCESS) {
            SIZE_T alloc_size = (SIZE_T)((size + large_page_size - 1) & ~(u64)(large_page_size - 1));
            mem = VirtualAlloc(NULL, alloc_size, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);

            adjust_token_privileges(token, FALSE, &prev_tp, 0, NULL, NULL);
        }
    }

    CloseHandle(token);
    return mem;
}

void *large_pages_alloc(u64 size, bool *large_pages) {
    void *mem = alloc_windows_large_pages(size);
    bool large = mem != NULL;

    if (!mem)
        mem = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    if (large_pages)
        *large_pages = mem && large;
    return mem;
}

void large_pages_free(void *mem) {
    if (mem)
        VirtualFree(mem, 0, MEM_RELEASE);
}


#elif defined(__linux__) && !defined(__ANDROID__)

#define HUGE_PAGE_SIZE ((u64)2 * 1024 * 1024)

void *large_pages_alloc(u64 size, bool *large_pages) {
    u64 alloc_size = (size + HUGE_PAGE_SIZE - 1) / HUGE_PAGE_SIZE * HUGE_PAGE_SIZE;
    void *mem = aligned_alloc(HUGE_PAGE_SIZE, alloc_size);
    bool large = false;

#if defined(MADV_HUGEPAGE)
    if (mem)
        large = madvise(mem, alloc_size, MADV_HUGEPAGE) == 0;
#endif

    if (large_pages)
        *large_pages = large;
    return mem;
}

void large_pages_free(void *mem) {
    free(mem);
}


#else

void *large_pages_alloc(u64 size, bool *large_pages) {
    if (large_pages)
        *large_pages = false;
    return aligned_alloc(64, (size + 63) / 64 * 64);
}

void large_pages_free(void *mem) {
    free(mem);
}

#endif