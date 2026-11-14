#ifndef _PLATFORMS_SYSTEM_H_
#define _PLATFORMS_SYSTEM_H_

#ifdef __cplusplus
extern "C" {
#endif

// What the operating system says about this process, for profilers.
typedef struct
{
    unsigned long long resident_bytes; // physical memory in use (working set / resident set)
    unsigned long long private_bytes;  // memory committed by the process alone ; 0 where unknown
    double cpu_seconds;                // CPU time used so far, all threads, user and kernel
} ProcessStats;

int platform_process_stats(ProcessStats* out_stats); // 0 where unavailable

#ifdef __cplusplus
}
#endif

#endif // _PLATFORMS_SYSTEM_H_
