// Process statistics straight from each operating system's API : SDL has no equivalent. Windows' calls
// are also available to UWP apps, so this file is shared by every platform layer.

#include "platforms/platforms_system.h"

#include <string.h>

#if defined(_WIN32)

#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2 // GetProcessMemoryInfo from kernel32, no psapi.lib
#endif

#include <windows.h>
#include <psapi.h>

int platform_process_stats(ProcessStats* out_stats)
{
    memset(out_stats, 0, sizeof(*out_stats));
    HANDLE process = GetCurrentProcess();

    PROCESS_MEMORY_COUNTERS_EX memory;
    memset(&memory, 0, sizeof(memory));
    memory.cb = sizeof(memory);

    if (!GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS*)&memory, sizeof(memory)))
    {
        return 0;
    }

    out_stats->resident_bytes = memory.WorkingSetSize;
    out_stats->private_bytes = memory.PrivateUsage;

    FILETIME creation, exit, kernel, user;

    if (GetProcessTimes(process, &creation, &exit, &kernel, &user))
    {
        // FILETIMEs count 100 ns intervals.
        ULONGLONG k = ((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
        ULONGLONG u = ((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime;
        out_stats->cpu_seconds = (double)(k + u) * 1e-7;
    }

    return 1;
}

#elif defined(__linux__) || defined(__APPLE__)

#include <sys/resource.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <stdio.h>
#endif

int platform_process_stats(ProcessStats* out_stats)
{
    memset(out_stats, 0, sizeof(*out_stats));

#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
    {
        out_stats->resident_bytes = info.resident_size;
    }
#else
    // /proc/self/statm : total and resident sizes, in pages.
    FILE* file = fopen("/proc/self/statm", "r");

    if (file)
    {
        unsigned long long total_pages = 0;
        unsigned long long resident_pages = 0;

        if (fscanf(file, "%llu %llu", &total_pages, &resident_pages) == 2)
        {
            out_stats->resident_bytes = resident_pages * (unsigned long long)sysconf(_SC_PAGESIZE);
        }

        fclose(file);
    }
#endif

    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
        out_stats->cpu_seconds = (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec * 1e-6 +
            (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec * 1e-6;
    }

    return 1;
}

#else

int platform_process_stats(ProcessStats* out_stats)
{
    memset(out_stats, 0, sizeof(*out_stats));
    return 0;
}

#endif
