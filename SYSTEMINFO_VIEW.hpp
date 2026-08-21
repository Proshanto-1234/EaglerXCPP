#ifndef SYSTEMINFO_VIEW_HPP
#define SYSTEMINFO_VIEW_HPP

#include <windows.h>
#include <process.h>
#include <vector>
#include <utility>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((long)(Status)) >= 0)
#endif

using pfnNtSetInformationThread = long(__stdcall*)(void*, unsigned long, void*, unsigned long);
using pfnNtQuerySystemInformation = long(__stdcall*)(unsigned long, void*, unsigned long, unsigned long*);

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
#ifndef _PROCESSOR_NUMBER_DEFINED
struct PROCESSOR_NUMBER {
    unsigned short Group;
    unsigned char  Number;
    unsigned char  Reserved;
};
#endif
inline bool LoadNtFunctions() {
	HINSTANCE__* hNtDll = GetModuleHandleW(L"ntdll.dll");
	if (!hNtDll) return false;

	NtSetInformationThread = (pfnNtSetInformationThread)GetProcAddress(hNtDll, "NtSetInformationThread");
	NtQuerySystemInformation = (pfnNtQuerySystemInformation)GetProcAddress(hNtDll, "NtQuerySystemInformation");
	return (NtSetInformationThread && NtQuerySystemInformation);
}

inline void NtPinThread(void* hThread, unsigned long coreIndex) {
    if (!NtSetInformationThread) [[unlikely]] return;

    // 1. Hard Affinity (Class 4)
    unsigned long coreInGroup = coreIndex % 64;
    unsigned long long affinityMask = (1ULL << coreInGroup);
    NtSetInformationThread(hThread, 4, &affinityMask, sizeof(affinityMask));

    // 2. Legacy Ideal Processor (Class 13)
    unsigned long idealProcessor = coreInGroup;
    NtSetInformationThread(hThread, 13, &idealProcessor, sizeof(idealProcessor));

    // 3. Modern Ideal Processor Ex (Class 33)
    PROCESSOR_NUMBER procNum = {
        .Group = static_cast<unsigned short>(coreIndex / 64),
        .Number = static_cast<unsigned char>(coreInGroup),
        .Reserved = 0
    };

    NtSetInformationThread(hThread, 33, &procNum, sizeof(procNum));
}

static std::pair<unsigned long, unsigned long> DynamicGetLeastUsedCores() {
	if (!NtQuerySystemInformation) [[unlikely]] return { 0, 0 };

	_SYSTEM_INFO sysInfo;
	GetSystemInfo(&sysInfo);
	unsigned long coreCount = static_cast<unsigned long>(sysInfo.dwNumberOfProcessors);
	unsigned long sanityCores = (coreCount > 256) ? 256 : coreCount;
	unsigned long returnLength = 0;

	size_t bufferSize = sizeof(PROCESSOR_PERFORMANCE_INFORMATION) * sanityCores;
	uint8_t* rawBuffer = reinterpret_cast<uint8_t*>(std::malloc(bufferSize * 2));
	if (!rawBuffer) [[unlikely]] return { 0, (coreCount > 1) ? 1U : 0U };

	PROCESSOR_PERFORMANCE_INFORMATION* sample1 = reinterpret_cast<PROCESSOR_PERFORMANCE_INFORMATION*>(rawBuffer);
	PROCESSOR_PERFORMANCE_INFORMATION* sample2 = reinterpret_cast<PROCESSOR_PERFORMANCE_INFORMATION*>(rawBuffer + bufferSize);

	long status1 = NtQuerySystemInformation(8, sample1, bufferSize, &returnLength);
	Sleep(15);
	long status2 = NtQuerySystemInformation(8, sample2, bufferSize, &returnLength);

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
