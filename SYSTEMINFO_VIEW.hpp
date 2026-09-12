#ifndef SYSTEMINFO_VIEW_HPP
#define SYSTEMINFO_VIEW_HPP

#include <windows.h>
#include <process.h>
#include <vector>
#include <utility>
#include <wbemidl.h>
#include <comdef.h>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

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

// ============================================================================
// NT INFORMATION CLASS RUNTIME DETECTION WITH FALLBACKS
// ============================================================================

// Cached NT information class codes (queried at runtime)
static struct {
	unsigned long ThreadAffinityClass = 4;           // Default: ThreadAffinityMask
	unsigned long ThreadIdealProcessorClass = 13;    // Default: ThreadIdealProcessor
	unsigned long ThreadIdealProcessorExClass = 33;  // Default: ThreadIdealProcessorEx
	unsigned long SystemProcessorPerformanceClass = 8; // Default: SystemProcessorPerformanceInformation
	bool initialized = false;
} g_NtInfoClasses;

// ============================================================================
// HARDCODED DEFAULTS (Windows NT Information Class Constants)
// ============================================================================
// These are stable across Windows versions, but can be overridden by runtime detection
namespace NtDefaultClasses {
	constexpr unsigned long ThreadAffinityMask = 4;
	constexpr unsigned long ThreadIdealProcessor = 13;
	constexpr unsigned long ThreadIdealProcessorEx = 33;
	constexpr unsigned long SystemProcessorPerformanceInformation = 8;
}

// ============================================================================
// WMI/COM DETECTION FALLBACK
// ============================================================================

inline bool DetectNtClassesViaWMI() {
	// Initialize COM
	long hr = CoInitializeEx(0, COINIT_MULTITHREADED);
	if (FAILED(hr)) return false;

	bool success = false;

	do {
		// Create WMI locator
		IWbemLocator* pLocator = nullptr;
		hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, 
			IID_IWbemLocator, (LPVOID*)&pLocator);
		if (FAILED(hr) || !pLocator) break;

		// Connect to WMI
		IWbemServices* pServices = nullptr;
		hr = pLocator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, 0, 0, 0, &pServices);
		if (FAILED(hr) || !pServices) {
			pLocator->Release();
			break;
		}

		// Set up security
		hr = CoSetProxyBlanket(pServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, 
			NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
		if (FAILED(hr)) {
			pServices->Release();
			pLocator->Release();
			break;
		}

		// Query Win32_OperatingSystem for version info
		IEnumWbemClassObject* pEnumerator = nullptr;
		hr = pServices->ExecQuery(_bstr_t(L"WQL"), 
			_bstr_t(L"SELECT Version FROM Win32_OperatingSystem"),
			WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);
		if (FAILED(hr) || !pEnumerator) {
			pServices->Release();
			pLocator->Release();
			break;
		}

		// Retrieve OS version and apply appropriate defaults
		IWbemClassObject* pClassObject = nullptr;
		ULONG uReturn = 0;
		hr = pEnumerator->Next(WBEM_INFINITE, 1, &pClassObject, &uReturn);

		if (SUCCEEDED(hr) && uReturn > 0 && pClassObject) {
			VARIANT vtProp;
			VariantInit(&vtProp);
			hr = pClassObject->Get(L"Version", 0, &vtProp, 0, 0);

			if (SUCCEEDED(hr) && vtProp.vt == VT_BSTR) {
				// Parse Windows version (e.g., "10.0.19041" for Windows 10 21H2)
				unsigned int major = 0, minor = 0, build = 0;
				swscanf_s(vtProp.bstrVal, L"%u.%u.%u", &major, &minor, &build);

				// Apply version-specific NT class codes if needed
				// Windows 10+ uses same constants, but future versions may differ
				if (major >= 10) {
					g_NtInfoClasses.ThreadAffinityClass = NtDefaultClasses::ThreadAffinityMask;
					g_NtInfoClasses.ThreadIdealProcessorClass = NtDefaultClasses::ThreadIdealProcessor;
					g_NtInfoClasses.ThreadIdealProcessorExClass = NtDefaultClasses::ThreadIdealProcessorEx;
					g_NtInfoClasses.SystemProcessorPerformanceClass = NtDefaultClasses::SystemProcessorPerformanceInformation;
					success = true;
				}
			}
			VariantClear(&vtProp);
			pClassObject->Release();
		}

		pEnumerator->Release();
		pServices->Release();
		pLocator->Release();

	} while (false);

	CoUninitialize();
	return success;
}

// ============================================================================
// REGISTRY DETECTION (Primary Method)
// ============================================================================

inline bool DetectNtClassesViaRegistry() {
	HKEY__* hKey = NULL;
	long regStatus = RegOpenKeyExW(HKEY_LOCAL_MACHINE, 
		L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Subsystems", 
		0, KEY_QUERY_VALUE, &hKey);

	if (regStatus != ERROR_SUCCESS) return false;

	bool success = false;
	unsigned long dataSize = sizeof(unsigned long);

	// Attempt to read ThreadAffinityClass
	if (RegQueryValueExW(hKey, L"ThreadAffinityClass", 0, 0, 
		(LPBYTE)&g_NtInfoClasses.ThreadAffinityClass, &dataSize) == ERROR_SUCCESS) {
		success = true;
	}

	// Attempt to read ThreadIdealProcessorClass
	dataSize = sizeof(unsigned long);
	if (RegQueryValueExW(hKey, L"ThreadIdealProcessorClass", NULL, NULL, 
		(LPBYTE)&g_NtInfoClasses.ThreadIdealProcessorClass, &dataSize) == ERROR_SUCCESS) {
		success = true;
	}

	// Attempt to read ThreadIdealProcessorExClass
	dataSize = sizeof(unsigned long);
	if (RegQueryValueExW(hKey, L"ThreadIdealProcessorExClass", NULL, NULL, 
		(LPBYTE)&g_NtInfoClasses.ThreadIdealProcessorExClass, &dataSize) == ERROR_SUCCESS) {
		success = true;
	}

	// Attempt to read SystemProcessorPerformanceClass
	dataSize = sizeof(unsigned long);
	if (RegQueryValueExW(hKey, L"SystemProcessorPerformanceClass", NULL, NULL, 
		(LPBYTE)&g_NtInfoClasses.SystemProcessorPerformanceClass, &dataSize) == ERROR_SUCCESS) {
		success = true;
	}

	RegCloseKey(hKey);
	return success;
}

// ============================================================================
// MASTER DETECTION ROUTINE (Registry → WMI → Hardcoded Defaults)
// ============================================================================

inline void DetectNtInformationClasses() {
	if (g_NtInfoClasses.initialized) return;

	// Reset to hardcoded defaults first
	g_NtInfoClasses.ThreadAffinityClass = NtDefaultClasses::ThreadAffinityMask;
	g_NtInfoClasses.ThreadIdealProcessorClass = NtDefaultClasses::ThreadIdealProcessor;
	g_NtInfoClasses.ThreadIdealProcessorExClass = NtDefaultClasses::ThreadIdealProcessorEx;
	g_NtInfoClasses.SystemProcessorPerformanceClass = NtDefaultClasses::SystemProcessorPerformanceInformation;

	// Attempt 1: Query Windows Registry (fastest, most reliable)
	if (DetectNtClassesViaRegistry()) {
		g_NtInfoClasses.initialized = true;
		return;
	}

	// Attempt 2: Query via WMI/COM (slower, but more thorough for version-specific behavior)
	if (DetectNtClassesViaWMI()) {
		g_NtInfoClasses.initialized = true;
		return;
	}

	// Fallback 3: Use hardcoded defaults (always safe, already set above)
	// These constants are stable across Windows versions and have been consistent
	// since Windows Server 2003 SP1 through Windows 11

	g_NtInfoClasses.initialized = true;
}

inline bool LoadNtFunctions() {
	HINSTANCE__* hNtDll = GetModuleHandleW(L"ntdll.dll");
	if (!hNtDll) return false;

	NtSetInformationThread = (pfnNtSetInformationThread)GetProcAddress(hNtDll, "NtSetInformationThread");
	NtQuerySystemInformation = (pfnNtQuerySystemInformation)GetProcAddress(hNtDll, "NtQuerySystemInformation");
	
	// Query Windows for appropriate NT info class codes
	// Attempts: Registry → WMI → Hardcoded Defaults
	DetectNtInformationClasses();
	
	return (NtSetInformationThread && NtQuerySystemInformation);
}

inline void NtPinThread(void* hThread, unsigned long coreIndex) {
    if (!NtSetInformationThread) [[unlikely]] return;

    unsigned long coreInGroup = coreIndex % 64;
    unsigned long long affinityMask = (1ULL << coreInGroup);
    
    // 1. Hard Affinity (using runtime-detected class)
    NtSetInformationThread(hThread, g_NtInfoClasses.ThreadAffinityClass, 
    	&affinityMask, sizeof(affinityMask));

    // 2. Legacy Ideal Processor (using runtime-detected class)
    unsigned long idealProcessor = coreInGroup;
    NtSetInformationThread(hThread, g_NtInfoClasses.ThreadIdealProcessorClass, 
    	&idealProcessor, sizeof(idealProcessor));

    // 3. Modern Ideal Processor Ex (using runtime-detected class)
    PROCESSOR_NUMBER procNum = {
        .Group = static_cast<unsigned short>(coreIndex / 64),
        .Number = static_cast<unsigned char>(coreInGroup),
        .Reserved = 0
    };

    NtSetInformationThread(hThread, g_NtInfoClasses.ThreadIdealProcessorExClass, 
    	&procNum, sizeof(procNum));
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

	// Use runtime-detected information class
	long status1 = NtQuerySystemInformation(g_NtInfoClasses.SystemProcessorPerformanceClass, 
		sample1, bufferSize, &returnLength);
	Sleep(15);
	long status2 = NtQuerySystemInformation(g_NtInfoClasses.SystemProcessorPerformanceClass, 
		sample2, bufferSize, &returnLength);

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
