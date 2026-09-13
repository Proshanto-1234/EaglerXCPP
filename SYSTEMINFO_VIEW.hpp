#ifndef SYSTEMINFO_VIEW_HPP
#define SYSTEMINFO_VIEW_HPP

#include <windows.h>
#include <process.h>
#include <vector>
#include <utility>
#include <cstdlib>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((long)(Status)) >= 0)
#endif

// ============================================================================
// NT API FUNCTION POINTERS & STRUCTURES
// ============================================================================

using pfnNtSetInformationThread = long(__stdcall*)(void*, unsigned long, void*, unsigned long);
using pfnNtQuerySystemInformation = long(__stdcall*)(unsigned long, void*, unsigned long, unsigned long*);
using pfnRtlGetVersion = long(__stdcall*)(PRTL_OSVERSIONINFOW);

struct PROCESSOR_PERFORMANCE_INFORMATION {
    _LARGE_INTEGER IdleTime;
    _LARGE_INTEGER KernelTime;
    _LARGE_INTEGER UserTime;
    _LARGE_INTEGER DpcTime;
    _LARGE_INTEGER InterruptTime;
    unsigned long InterruptCount;
};

static pfnNtSetInformationThread NtSetInformationThread = nullptr;
static pfnNtQuerySystemInformation NtQuerySystemInformation = nullptr;
static pfnRtlGetVersion RtlGetVersionPtr = nullptr;

#ifndef _PROCESSOR_NUMBER_DEFINED
struct PROCESSOR_NUMBER {
    unsigned short Group;
    unsigned char  Number;
    unsigned char  Reserved;
};
#endif

// ============================================================================
// NT INFORMATION CLASSES (Hardcoded ABI Stable Standards)
// ============================================================================
namespace NtInfoClasses {
    static constexpr unsigned long ThreadAffinityClass = 4;           // ThreadAffinityMask
    static constexpr unsigned long ThreadIdealProcessorClass = 13;    // ThreadIdealProcessor
    static constexpr unsigned long ThreadIdealProcessorExClass = 33;  // ThreadIdealProcessorEx
    static constexpr unsigned long SystemProcessorPerformanceClass = 8; // SystemProcessorPerformanceInformation
}

// ============================================================================
// REAL NTDLL INTEGRITY & VERSION VERIFICATION
// ============================================================================

inline bool VerifyNtDllIntegrity(HMODULE hNtDll) {
    if (!hNtDll) return false;

    // Verify DOS and NT headers to ensure ntdll is valid and loaded cleanly
    PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(hNtDll);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return false;

    PIMAGE_NT_HEADERS ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(
        reinterpret_cast<BYTE*>(hNtDll) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return false;

    // Query native kernel OS version via RtlGetVersion from ntdll
    RtlGetVersionPtr = (pfnRtlGetVersion)GetProcAddress(hNtDll, "RtlGetVersion");
    if (RtlGetVersionPtr) {
        RTL_OSVERSIONINFOW osInfo = { 0 };
        osInfo.dwOSVersionInfoSize = sizeof(osInfo);
        if (NT_SUCCESS(RtlGetVersionPtr(&osInfo))) {
            // NT Class ABI constants are guaranteed valid across NT 6.0+ (Win Vista -> Win 11/2026)
            return (osInfo.dwMajorVersion >= 6);
        }
    }

    return true;
}

inline bool LoadNtFunctions() {
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtDll) return false;

    // Verify ntdll via internal PE header inspection and OS version check
    if (!VerifyNtDllIntegrity(hNtDll)) return false;

    // Resolve system call entry points directly from NT API export table
    NtSetInformationThread = (pfnNtSetInformationThread)GetProcAddress(hNtDll, "NtSetInformationThread");
    NtQuerySystemInformation = (pfnNtQuerySystemInformation)GetProcAddress(hNtDll, "NtQuerySystemInformation");

    return (NtSetInformationThread && NtQuerySystemInformation);
}

// ============================================================================
// HIGH-PERFORMANCE THREAD PINNING & CORE SAMPLING
// ============================================================================

inline void NtPinThread(void* hThread, unsigned long coreIndex) {
    if (!NtSetInformationThread) [[unlikely]] return;

    unsigned long coreInGroup = coreIndex % 64;
    unsigned long long affinityMask = (1ULL << coreInGroup);

    // 1. Hard Affinity
    NtSetInformationThread(hThread, NtInfoClasses::ThreadAffinityClass,
        &affinityMask, sizeof(affinityMask));

    // 2. Legacy Ideal Processor
    unsigned long idealProcessor = coreInGroup;
    NtSetInformationThread(hThread, NtInfoClasses::ThreadIdealProcessorClass,
        &idealProcessor, sizeof(idealProcessor));

    // 3. Modern Ideal Processor Ex (Multi-Group / >64 Cores Support)
    PROCESSOR_NUMBER procNum = {
        .Group = static_cast<unsigned short>(coreIndex / 64),
        .Number = static_cast<unsigned char>(coreInGroup),
        .Reserved = 0
    };

    NtSetInformationThread(hThread, NtInfoClasses::ThreadIdealProcessorExClass,
        &procNum, sizeof(procNum));
}

static std::pair<unsigned long, unsigned long> DynamicGetLeastUsedCores() {
    if (!NtQuerySystemInformation) [[unlikely]] return { 0, 0 };

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    unsigned long coreCount = static_cast<unsigned long>(sysInfo.dwNumberOfProcessors);
    unsigned long sanityCores = (coreCount > 256) ? 256 : coreCount;
    unsigned long returnLength = 0;

    size_t bufferSize = sizeof(PROCESSOR_PERFORMANCE_INFORMATION) * sanityCores;
    uint8_t* rawBuffer = reinterpret_cast<uint8_t*>(std::malloc(bufferSize * 2));
    if (!rawBuffer) [[unlikely]] return { 0, (coreCount > 1) ? 1U : 0U };

    PROCESSOR_PERFORMANCE_INFORMATION* sample1 = reinterpret_cast<PROCESSOR_PERFORMANCE_INFORMATION*>(rawBuffer);
    PROCESSOR_PERFORMANCE_INFORMATION* sample2 = reinterpret_cast<PROCESSOR_PERFORMANCE_INFORMATION*>(rawBuffer + bufferSize);

    // Sample processor performance stats via native ntdll syscall
    long status1 = NtQuerySystemInformation(NtInfoClasses::SystemProcessorPerformanceClass,
        sample1, static_cast<unsigned long>(bufferSize), &returnLength);
    Sleep(15);
    long status2 = NtQuerySystemInformation(NtInfoClasses::SystemProcessorPerformanceClass,
        sample2, static_cast<unsigned long>(bufferSize), &returnLength);

    if (!NT_SUCCESS(status1) || !NT_SUCCESS(status2)) [[unlikely]] {
        std::free(rawBuffer);
        return { 0, (coreCount > 1) ? 1U : 0U };
    }

    unsigned long leastUsedCore = 0;
    unsigned long secondLeastUsedCore = (sanityCores > 1) ? 1 : 0;
    double maxIdle1 = -1.0, maxIdle2 = -1.0;

    for (unsigned long i = 0; i < sanityCores; ++i) {
        unsigned long long deltaIdle = sample2[i].IdleTime.QuadPart - sample1[i].IdleTime.QuadPart;
        unsigned long long deltaKernel = sample2[i].KernelTime.QuadPart - sample1[i].KernelTime.QuadPart;
        unsigned long long deltaUser = sample2[i].UserTime.QuadPart - sample1[i].UserTime.QuadPart;
        unsigned long long totalTime = deltaKernel + deltaUser;

        double idleRatio = (totalTime > 0) ? static_cast<double>(deltaIdle) / totalTime : 1.0;

        if (idleRatio > maxIdle1) {
            maxIdle2 = maxIdle1;
            secondLeastUsedCore = leastUsedCore;
            maxIdle1 = idleRatio;
            leastUsedCore = i;
        }
        else if (idleRatio > maxIdle2) {
            maxIdle2 = idleRatio;
            secondLeastUsedCore = i;
        }
    }

    if (leastUsedCore == secondLeastUsedCore && sanityCores > 1) {
        secondLeastUsedCore = (leastUsedCore == 0) ? 1 : 0;
    }

    std::free(rawBuffer);
    return { leastUsedCore, secondLeastUsedCore };
}

#endif // !SYSTEMINFO_VIEW_HPP
