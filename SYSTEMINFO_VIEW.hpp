#ifndef SYSTEMINFO_VIEW_HPP
#define SYSTEMINFO_VIEW_HPP

#include <windows.h>
#include <process.h>
#include <vector>
#include <utility>

// Standard success check
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((long)(Status)) >= 0)
#endif

using pfnNtSetInformationThread = long(__stdcall*)(void*, unsigned long, void*, unsigned long);
using pfnNtQuerySystemInformation = long(__stdcall*)(unsigned long, void*, unsigned long, unsigned long*);

// Completely documented, stable Win32/NT kernel structure
struct PROCESSOR_PERFORMANCE_INFORMATION {
	_LARGE_INTEGER IdleTime;
	_LARGE_INTEGER KernelTime;
	_LARGE_INTEGER UserTime;
	_LARGE_INTEGER DpcTime;
	_LARGE_INTEGER InterruptTime;
	unsigned long InterruptCount;
};

// Global function pointers
static pfnNtSetInformationThread  NtSetInformationThread = nullptr;
static pfnNtQuerySystemInformation NtQuerySystemInformation = nullptr;

inline bool LoadNtFunctions() {
	HINSTANCE__* hNtDll = GetModuleHandleW(L"ntdll.dll");
	if (!hNtDll) return false;

	NtSetInformationThread = (pfnNtSetInformationThread)GetProcAddress(hNtDll, "NtSetInformationThread");
	NtQuerySystemInformation = (pfnNtQuerySystemInformation)GetProcAddress(hNtDll, "NtQuerySystemInformation");
	return (NtSetInformationThread && NtQuerySystemInformation);
}

// Low-level NT Pinning function
inline void NtPinThread(void* hThread, unsigned long coreIndex) {
	if (!NtSetInformationThread) [[unlikely]] return;

	unsigned long long affinityMask = (static_cast<unsigned long long>(1) << coreIndex);
	NtSetInformationThread(hThread, 4, &affinityMask, sizeof(unsigned long long)); // 4 = ThreadAffinityMask

	unsigned long idealProcessor = coreIndex;
	NtSetInformationThread(hThread, 13, &idealProcessor, sizeof(unsigned long));  // 13 = ThreadIdealProcessor
}

/// <summary>
/// Self-contained: Dynamically discovers logical cores, queries NT performance metrics, 
/// and extracts the two least-used cores using a transient zero-footprint heap scope.
/// </summary>
static std::pair<unsigned long, unsigned long> DynamicGetLeastUsedCores() {
	if (!NtQuerySystemInformation) [[unlikely]] return { 0, 0 };

	// Retrieve active hardware core limits instantly
	_SYSTEM_INFO sysInfo;
	GetSystemInfo(&sysInfo);
	unsigned long coreCount = static_cast<unsigned long>(sysInfo.dwNumberOfProcessors);

	// Cap core allocation check at 256 to ensure zero pointer overflow risk
	unsigned long sanityCores = (coreCount > 256) ? 256 : coreCount;
	unsigned long returnLength = 0;

	// 2. ALLOCATION: Grab exact chunk sizing from transient heap
	size_t bufferSize = sizeof(PROCESSOR_PERFORMANCE_INFORMATION) * sanityCores;
	uint8_t* rawBuffer = reinterpret_cast<uint8_t*>(std::malloc(bufferSize * 2));
	if (!rawBuffer) [[unlikely]] return { 0, (coreCount > 1) ? 1U : 0U };

	PROCESSOR_PERFORMANCE_INFORMATION* sample1 = reinterpret_cast<PROCESSOR_PERFORMANCE_INFORMATION*>(rawBuffer);
	PROCESSOR_PERFORMANCE_INFORMATION* sample2 = reinterpret_cast<PROCESSOR_PERFORMANCE_INFORMATION*>(rawBuffer + bufferSize);

	// 3. TELEMETRY SAMPLING: Query SystemProcessorPerformanceInformation (Class 8)
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

	// 4. PARSING PIPELINE
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

	// 5. ERASE TRACE: Completely free calculations from memory layout before thread return
	std::free(rawBuffer);

	return { leastUsedCore, secondLeastUsedCore };
}
#endif // !SYSTEMINFO_VIEW_HPP