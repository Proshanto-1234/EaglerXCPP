#ifndef DEFINITIONS_HPP
#define DEFINITIONS_HPP

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include <cstdint>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <string>
#include <string_view>
#include <format>
#include <numeric>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <immintrin.h>

#include "GPU_CHUNK_SERIALIZER.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

constexpr unsigned long WEBSOCKET_BASE64 = 0x00000001;
constexpr unsigned long WEBSOCKET_NOCRLF = 0x40000000;
constexpr size_t PLAYER_STATUS_NORMAL = 0x00;
constexpr size_t PLAYER_STATUS_OPERATOR = 0x01;

constexpr DWORD RIO_POOL_SIZE = 64 * 1024 * 1024;
constexpr DWORD RIO_SLICE_SIZE = 32768;

#if defined(min)
#undef min
#endif

enum SOCKET_OPERATION { OP_HANDSHAKE, OP_READ, OP_SOKT_RECYCLE };
enum ClientState { STATE_HANDSHAKE, STATE_LOGIN, STATE_PLAY };
enum BiomeID { BIOME_PLAINS = 1, BIOME_DESERT = 2, BIOME_RIVER = 7, BIOME_ICE_SPIKES = 140 };
enum EntityType { ENTITY_MOB, ENTITY_PROJECTILE };
enum PLAYER_GAMEMODE { SURVIVAL, SPECTATOR };

enum ERROR_CODES {
	ERROR_INTERNAL = -1,
	ERROR_UNSUPPORTED_HARDWARE = -2,
	ERROR_CRITICAL_MEMORY_FAILURE = -3,
	ERROR_INTERNAL_NTDLL_FAILURE = -4
};

struct GAME_ENTITY {
	unsigned long long entityId;
	EntityType type;
	int posX;
	int posZ;
	unsigned long long lastActiveTime;
};

struct CONNECTION_CONTEXT {
	_OVERLAPPED overlapped;
	unsigned long long socket;
	uint8_t* buffer;
	size_t rxBufferOffset;
	_WSABUF wsaBuf;
	SOCKET_OPERATION operation;

	RIO_RQ requestQueue;
	RIO_BUF rioBuf;
	DWORD bufferSliceIndex;

	CONNECTION_CONTEXT()
		: socket(INVALID_SOCKET),
		  buffer(nullptr),
		  rxBufferOffset(0),
		  operation(OP_HANDSHAKE),
		  requestQueue(RIO_INVALID_RQ),
		  bufferSliceIndex(0)
	{
		std::memset(&overlapped, 0, sizeof(_OVERLAPPED));
		std::memset(&rioBuf, 0, sizeof(RIO_BUF));
		wsaBuf.buf = nullptr;
		wsaBuf.len = 0;
	}
};

struct PLAYER_SESSION {
	ClientState state = STATE_HANDSHAKE;
	double playerX = 8.0;
	double playerY = 65.0;
	double playerZ = 8.0;
	float yaw = 0.0f;
	float pitch = 0.0f;
	char username[16] = { 0 };
	size_t status = PLAYER_STATUS_NORMAL;
	std::string gamemode = "SURVIVAL";
	int entityId = 1;
};

struct ThreadArena {
	alignas(32) uint8_t memoryPool[128 * 1024];
	size_t offset = 0;

	ThreadArena() noexcept : offset(0) {}

	void* Allocate(size_t size) {
		size_t alignedSize = (size + 31) & ~31;
		if (offset + alignedSize > sizeof(memoryPool)) {
			return nullptr;
		}
		void* ptr = &memoryPool[offset];
		offset += alignedSize;
		return ptr;
	}
	void Clear() { offset = 0; }
};

struct Console {
	void* hOut = nullptr;
	void* hIn = nullptr;
	void* hErr = nullptr;
	unsigned long written = 0;
	std::basic_string<wchar_t, std::char_traits<wchar_t>, std::allocator<wchar_t>> message;
	int japansesFontLoaded = 0;  // 0 = not loaded, 1 = loaded, -1 = failed

	bool Initialize() {
		hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		hIn = GetStdHandle(STD_INPUT_HANDLE);
		hErr = GetStdHandle(STD_ERROR_HANDLE);

		if ((hOut == INVALID_HANDLE_VALUE) || 
			(hIn == INVALID_HANDLE_VALUE) || 
			(hErr == INVALID_HANDLE_VALUE)) {
			return false;
		}

		// Enable UTF-16 console mode and virtual terminal processing
		DWORD dwMode = 0;
		GetConsoleMode((HANDLE)hOut, &dwMode);
		dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		SetConsoleMode((HANDLE)hOut, dwMode);

		// Try to load Japanese font
		LoadJapaneseFont();

		return true;
	}

private:
	bool LoadJapaneseFont() {
		// Get executable directory
		wchar_t exePath[MAX_PATH];
		if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
			japansesFontLoaded = -1;
			return false;
		}

		// Find last backslash to get directory
		wchar_t* lastSlash = wcsrchr(exePath, L'\\');
		if (!lastSlash) {
			japansesFontLoaded = -1;
			return false;
		}
		*lastSlash = L'\0';

		// Build full path to font file
		wchar_t fontPath[MAX_PATH];
		wcscpy_s(fontPath, MAX_PATH, exePath);
		wcscat_s(fontPath, MAX_PATH, L"\\NotoSansMonoCJKjp-Regular.otf");

		// Try to load font into memory (private to this process)
		int fontCount = AddFontResourceExW(fontPath, FR_PRIVATE, NULL);

		if (fontCount == 0) {
			japansesFontLoaded = -1;
			return false;
		}

		// Apply font to console
		CONSOLE_FONT_INFOEX fontInfo = {};
		fontInfo.cbSize = sizeof(CONSOLE_FONT_INFOEX);

		if (!GetCurrentConsoleFontEx((HANDLE)hOut, FALSE, &fontInfo)) {
			RemoveFontResourceExW(fontPath, FR_PRIVATE, NULL);
			japansesFontLoaded = -1;
			return false;
		}

		// Set font name
		wcscpy_s(fontInfo.FaceName, LF_FACESIZE, L"Noto Sans Mono CJK JP");
		fontInfo.dwFontSize.Y = 12;  // 12pt
		fontInfo.dwFontSize.X = 0;   // Auto width

		if (!SetCurrentConsoleFontEx((HANDLE)hOut, FALSE, &fontInfo)) {
			RemoveFontResourceExW(fontPath, FR_PRIVATE, NULL);
			japansesFontLoaded = -1;
			return false;
		}

		japansesFontLoaded = 1;
		return true;
	}

public:
	~Console() {
		// Font resources are automatically released when process exits
		// RemoveFontResourceExW is called automatically by Windows
	}
};

// ============================================================================
// GLOBAL DECLARATIONS
// ============================================================================
extern std::atomic<bool> ENGINE;
extern unsigned long long listenSock;
extern void* hIOCP;
extern std::unordered_map<unsigned long long, PLAYER_SESSION> ActiveSessions;
extern CRITICAL_SECTION SessionLock;
extern std::vector<GAME_ENTITY> GlobalEntities;
extern thread_local ThreadArena WorkerArena;
extern GPUChunkSerializer g_gpuSerializer;
extern Console currentWindow;

// ============================================================================
// UTILITY FUNCTION DECLARATIONS
// ============================================================================
bool CheckHardwareInstructionSupport();
unsigned long long WriteVarIntToBuffer(char* dest, int value);
int ReadVarInt(const uint8_t* buf, size_t maxLen, size_t& bytesRead);
void SendWebSocketFrame(unsigned long long socket, const char* payload, size_t length);
void BroadcastChatMessage(const std::string& sender, const std::string& message);
uint32_t GetHardwareRandom();
void LoadOrCreateServerProperties(unsigned short& port, std::wstring& mode);
void GenerateWorldChunk(int chunkX, int chunkZ, uint8_t* outChunkData);
void GarbageCollectStrayEntities();
void SendChunkColumn(unsigned long long socket, int chunkX, int chunkZ);
void ProcessEaglercraftPacket(CONNECTION_CONTEXT* ctx, uint8_t* payload, size_t len);
int __stdcall ConsoleCtrlHandler(unsigned long dwCtrlType);

// ============================================================================
// ENCODING UTILITIES
// ============================================================================
namespace encoding {
	// Convert UTF-8 std::string to UTF-16 std::wstring
	inline std::wstring utf8_to_utf16(const std::string& utf8_str) {
		if (utf8_str.empty()) {
			return std::wstring();
		}

		// Get required buffer size
		int size_needed = MultiByteToWideChar(CP_UTF8, 0,
			utf8_str.c_str(), (int)utf8_str.length(), NULL, 0);

		if (size_needed <= 0) {
			return std::wstring();
		}

		// Convert UTF-8 to UTF-16
		std::wstring result(size_needed, 0);
		MultiByteToWideChar(CP_UTF8, 0,
			utf8_str.c_str(), (int)utf8_str.length(),
			&result[0], size_needed);

		return result;
	}

	// Convert UTF-16 std::wstring to UTF-8 std::string
	inline std::string utf16_to_utf8(const std::wstring& utf16_str) {
		if (utf16_str.empty()) {
			return std::string();
		}

		// Get required buffer size
		int size_needed = WideCharToMultiByte(CP_UTF8, 0,
			utf16_str.c_str(), (int)utf16_str.length(), NULL, 0, NULL, NULL);

		if (size_needed <= 0) {
			return std::string();
		}

		// Convert UTF-16 to UTF-8
		std::string result(size_needed, 0);
		WideCharToMultiByte(CP_UTF8, 0,
			utf16_str.c_str(), (int)utf16_str.length(),
			&result[0], size_needed, NULL, NULL);

		return result;
	}

	// Safe conversion with fallback for invalid characters
	inline std::wstring utf8_to_utf16_safe(const std::string& utf8_str,
		wchar_t fallback_char = L'?') {
		if (utf8_str.empty()) {
			return std::wstring();
		}

		// Try strict UTF-8 conversion first
		int size_needed = MultiByteToWideChar(CP_UTF8,
			MB_ERR_INVALID_CHARS,
			utf8_str.c_str(), (int)utf8_str.length(), NULL, 0);

		if (size_needed <= 0) {
			// Invalid UTF-8 sequence, use lossy conversion
			size_needed = MultiByteToWideChar(CP_UTF8, 0,
				utf8_str.c_str(), (int)utf8_str.length(), NULL, 0);

			if (size_needed <= 0) {
				return std::wstring(1, fallback_char);
			}
		}

		std::wstring result(size_needed, 0);
		MultiByteToWideChar(CP_UTF8, 0,
			utf8_str.c_str(), (int)utf8_str.length(),
			&result[0], size_needed);

		return result;
	}
}

// ============================================================================
// INLINE UTILITY CLASS
// ============================================================================
class AVX2NoiseEngine {
private:
	alignas(32) int32_t p_int[512];

	void InitPermutation(uint32_t seed) {
		uint8_t p[256];
		std::iota(std::begin(p), std::end(p), 0);

		uint32_t state = seed;
		for (int i = 255; i > 0; i--) {
			state = state * 1664525ULL + 1013904223ULL;
			int j = state % (i + 1);
			std::swap(p[i], p[j]);
		}
		for (int i = 0; i < 256; i++) {
			p_int[i] = static_cast<int32_t>(p[i]);
			p_int[256 + i] = static_cast<int32_t>(p[i]);
		}
	}

	inline __m256d Fade_AVX2(__m256d t) {
		__m256d six = _mm256_set1_pd(6.0);
		__m256d fifteen = _mm256_set1_pd(15.0);
		__m256d ten = _mm256_set1_pd(10.0);
		__m256d res = _mm256_fmsub_pd(t, six, fifteen);
		res = _mm256_fmadd_pd(t, res, ten);
		__m256d t3 = _mm256_mul_pd(t, _mm256_mul_pd(t, t));
		return _mm256_mul_pd(t3, res);
	}

	inline __m256d Lerp_AVX2(__m256d t, __m256d a, __m256d b) {
		return _mm256_fmadd_pd(t, _mm256_sub_pd(b, a), a);
	}

	inline __m256d Grad_AVX2(__m256i hash, __m256d x, __m256d y) {
		__m256i h = _mm256_and_si256(hash, _mm256_set1_epi32(7));
		__m256d x_sign = _mm256_and_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi32(_mm256_and_si256(h, _mm256_set1_epi32(1)), _mm256_set1_epi32(0))), x);
		__m256d y_sign = _mm256_and_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi32(_mm256_and_si256(h, _mm256_set1_epi32(2)), _mm256_set1_epi32(0))), y);
		return _mm256_add_pd(x_sign, y_sign);
	}

public:
	AVX2NoiseEngine(uint32_t seed) { InitPermutation(seed); }

	void SampleNoise4(__m256d x, __m256d y, double* outArray) {
		__m256d x_floor = _mm256_floor_pd(x);
		__m256d y_floor = _mm256_floor_pd(y);

		__m128i X_128 = _mm256_cvtpd_epi32(x_floor);
		__m128i Y_128 = _mm256_cvtpd_epi32(y_floor);
		__m256i X_raw = _mm256_castsi128_si256(X_128);
		__m256i Y_raw = _mm256_castsi128_si256(Y_128);

		__m256i X = _mm256_and_si256(X_raw, _mm256_set1_epi32(255));
		__m256i Y = _mm256_and_si256(Y_raw, _mm256_set1_epi32(255));

		__m256d x_frac = _mm256_sub_pd(x, x_floor);
		__m256d y_frac = _mm256_sub_pd(y, y_floor);

		__m256d u = Fade_AVX2(x_frac);
		__m256d v = Fade_AVX2(y_frac);

		__m256i p_X = _mm256_i32gather_epi32((const int*)p_int, X, 4);
		__m256i p_Y = _mm256_i32gather_epi32((const int*)p_int, Y, 4);

		__m256i A = _mm256_add_epi32(p_X, p_Y);
		__m256i B = _mm256_add_epi32(_mm256_i32gather_epi32((const int*)p_int, _mm256_add_epi32(X, _mm256_set1_epi32(1)), 4), p_Y);

		__m256d grad1 = Grad_AVX2(A, x_frac, y_frac);
		__m256d grad2 = Grad_AVX2(B, _mm256_sub_pd(x_frac, _mm256_set1_pd(1.0)), y_frac);

		__m256i A_plus1 = _mm256_add_epi32(A, _mm256_set1_epi32(1));
		__m256i B_plus1 = _mm256_add_epi32(B, _mm256_set1_epi32(1));
		__m256d grad3 = Grad_AVX2(A_plus1, x_frac, _mm256_sub_pd(y_frac, _mm256_set1_pd(1.0)));
		__m256d grad4 = Grad_AVX2(B_plus1, _mm256_sub_pd(x_frac, _mm256_set1_pd(1.0)), _mm256_sub_pd(y_frac, _mm256_set1_pd(1.0)));

		__m256d res = Lerp_AVX2(v, Lerp_AVX2(u, grad1, grad2), Lerp_AVX2(u, grad3, grad4));
		res = _mm256_mul_pd(_mm256_add_pd(res, _mm256_set1_pd(1.0)), _mm256_set1_pd(0.5));
		_mm256_storeu_pd(outArray, res);
	}
};

#endif // !DEFINITIONS_HPP
