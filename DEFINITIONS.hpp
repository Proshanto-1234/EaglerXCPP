#ifndef DEFINITIONS_HPP
#define DEFINITIONS_HPP

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h> // Required for RIO types and definitions

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

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

constexpr unsigned long WEBSOCKET_BASE64 = 0x00000001;
constexpr unsigned long WEBSOCKET_NOCRLF = 0x40000000;
constexpr size_t PLAYER_STATUS_NORMAL = 0x00;
constexpr size_t PLAYER_STATUS_OPERATOR = 0x01;

// Registered I/O Memory Allocations
constexpr DWORD RIO_POOL_SIZE  = 64 * 1024 * 1024; // 64 MB Pinned RAM Pool
constexpr DWORD RIO_SLICE_SIZE = 32768;            // 32 KB per socket slice

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

// ============================================================================
// UPDATED FOR REGISTERED I/O (RIO) SUPPORT
// ============================================================================
struct CONNECTION_CONTEXT {
	_OVERLAPPED overlapped;
	unsigned long long socket;
	uint8_t* buffer;
	size_t rxBufferOffset;
	_WSABUF wsaBuf;
	SOCKET_OPERATION operation;

	// Native RIO Queue and Buffer primitives
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
			offset = 0;
		}
		void* ptr = &memoryPool[offset];
		offset += alignedSize;
		return ptr;
	}

	void Clear() { offset = 0; }
};

inline std::atomic<bool> ENGINE = true;
inline unsigned long long listenSock = INVALID_SOCKET;
inline HANDLE hIOCP = NULL;
inline std::unordered_map<unsigned long long, PLAYER_SESSION> ActiveSessions;
inline CRITICAL_SECTION SessionLock;
inline std::vector<GAME_ENTITY> GlobalEntities;
inline thread_local ThreadArena WorkerArena;

struct Console {
	void* hOut = nullptr;
	void* hIn = nullptr;
	void* hErr = nullptr;
	unsigned long written = 0;
	std::basic_string<wchar_t, std::char_traits<wchar_t>, std::allocator<wchar_t>> message;
	bool Initialize() {
		hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		hIn = GetStdHandle(STD_INPUT_HANDLE);
		hErr = GetStdHandle(STD_ERROR_HANDLE);
		return (hOut != INVALID_HANDLE_VALUE) && (hIn != INVALID_HANDLE_VALUE) && (hErr != INVALID_HANDLE_VALUE);
	}
};

inline Console currentWindow;

__forceinline static bool CheckHardwareInstructionSupport() {
	int cpuInfo[4] = { 0 };
	__cpuid(cpuInfo, 1);
	bool supportsRDRAND = (cpuInfo[2] & (1 << 30)) != 0;
	__cpuid(cpuInfo, 7);
	bool supportsAVX2 = (cpuInfo[1] & (1 << 5)) != 0;
	return supportsRDRAND && supportsAVX2;
}

constexpr unsigned long long WriteVarIntToBuffer(char* dest, int value) {
	size_t written = 0;
	unsigned int uValue = static_cast<unsigned int>(value);
	do {
		char byte = static_cast<char>(uValue & 0x7F);
		uValue >>= 7;
		if (uValue != 0) byte |= 0x80;
		dest[written++] = byte;
	} while (uValue != 0);
	return written;
}

static int ReadVarInt(const uint8_t* buf, size_t maxLen, size_t& bytesRead) {
	int value = 0;
	int bitOffset = 0;
	bytesRead = 0;
	while (bytesRead < maxLen) {
		uint8_t b = buf[bytesRead++];
		value |= (b & 0x7F) << bitOffset;
		if ((b & 0x80) == 0) return value;
		bitOffset += 7;
		if (bitOffset >= 35) break;
	}
	return 0;
}

static void SendWebSocketFrame(unsigned long long socket, const char* payload, size_t length) {
	alignas(32) char headerBuffer[10];
	size_t headerLen = 2;
	headerBuffer[0] = static_cast<char>(0x82);
	if (length <= 125) {
		headerBuffer[1] = static_cast<char>(length);
	}
	else if (length <= 65535) {
		headerBuffer[1] = 126;
		headerBuffer[2] = static_cast<char>((length >> 8) & 0xFF);
		headerBuffer[3] = static_cast<char>(length & 0xFF);
		headerLen = 4;
	}
	else {
		headerBuffer[1] = 127;
		for (int i = 0; i < 8; ++i) {
			headerBuffer[2 + i] = static_cast<char>((length >> ((7 - i) * 8)) & 0xFF);
		}
		headerLen = 10;
	}

	(void)send(socket, headerBuffer, static_cast<int>(headerLen), 0);
	(void)send(socket, payload, static_cast<int>(length), 0);
}

static void BroadcastChatMessage(const std::string& sender, const std::string& message) {
	alignas(32) char staticBuffer[2048];
	char* jsonPayloadStart = staticBuffer + 32;
	size_t maxJsonSize = sizeof(staticBuffer) - 32;

	std::format_to_n_result<char*> formatResult = std::format_to_n(jsonPayloadStart, maxJsonSize,
		"{{\"text\":\"[{}] {}\"}}", sender, message);

	size_t jsonLen = formatResult.size;
	if (jsonLen >= maxJsonSize) [[unlikely]] { return; }

	constexpr char mcPacketId = 0x0F;
	char stringVarInt[5];
	size_t stringVarIntLen = WriteVarIntToBuffer(stringVarInt, static_cast<int>(jsonLen));

	int mcInnerPayloadSize = static_cast<int>(1 + stringVarIntLen + jsonLen);
	char mcTotalLengthVarInt[5];
	size_t mcLengthVarIntLen = WriteVarIntToBuffer(mcTotalLengthVarInt, mcInnerPayloadSize);

	size_t totalMcPacketSize = mcLengthVarIntLen + static_cast<size_t>(mcInnerPayloadSize);
	size_t mcHeaderStartOffset = 32 - (mcLengthVarIntLen + 1 + stringVarIntLen);
	char* mcWriteCursor = staticBuffer + mcHeaderStartOffset;

	std::copy_n(mcTotalLengthVarInt, mcLengthVarIntLen, mcWriteCursor);
	mcWriteCursor += mcLengthVarIntLen;
	*mcWriteCursor = mcPacketId;
	mcWriteCursor += 1;
	std::copy_n(stringVarInt, stringVarIntLen, mcWriteCursor);

	char* frameStart = staticBuffer + mcHeaderStartOffset;

	EnterCriticalSection(&SessionLock);
	for (const auto& [socket, session] : ActiveSessions) {
		if (session.state == STATE_PLAY) {
			SendWebSocketFrame(socket, frameStart, totalMcPacketSize);
		}
	}
	LeaveCriticalSection(&SessionLock);
}

__forceinline uint32_t GetHardwareRandom() {
	uint32_t val = 0;
	for (int i = 0; i < 10; ++i) {
		if (_rdrand32_step(&val)) return val;
	}
	return 13372026;
}

static void LoadOrCreateServerProperties(unsigned short& port, std::wstring& mode) {
	std::unordered_map<std::string, std::string> props;
	std::ifstream file("server.properties");

	if (!file.is_open()) {
		std::ofstream outFile("server.properties");
		outFile << "# Server Properties for Eaglercraft 1.12.2 Native Server\n";
		outFile << "server-port=26565\n";
		outFile << "gamemode=survival\n";
		outFile << "view-distance=6\n";
		outFile.close();
		port = 26565;
		return;
	}

	std::string line;
	while (std::getline(file, line)) {
		if (line.empty() || line[0] == '#') continue;
		std::istringstream is_line(line);
		std::string key, value;
		if (std::getline(is_line, key, '=') && std::getline(is_line, value)) {
			props[key] = value;
		}
	}

	if (props.count("server-port")) {
		port = static_cast<unsigned short>(std::stoi(props["server-port"]));
	}
}

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

static void GenerateWorldChunk(int chunkX, int chunkZ, uint8_t* outChunkData) {
	static uint32_t seed = GetHardwareRandom();
	static AVX2NoiseEngine terrainNoise(seed);
	static AVX2NoiseEngine tempNoise(seed + 101);
	static AVX2NoiseEngine moistureNoise(seed + 202);

	std::memset(outChunkData, 0, 65536);

	alignas(32) static thread_local double terrain_res[4];
	alignas(32) static thread_local double temp_res[4];
	alignas(32) static thread_local double moisture_res[4];

	for (int x = 0; x < 16; x += 4) {
		__m256d base_x = _mm256_set_pd(
			static_cast<double>((chunkX * 16) + x + 3),
			static_cast<double>((chunkX * 16) + x + 2),
			static_cast<double>((chunkX * 16) + x + 1),
			static_cast<double>((chunkX * 16) + x)
		);
		__m256d freq = _mm256_set1_pd(0.005);
		base_x = _mm256_mul_pd(base_x, freq);

		for (int z = 0; z < 16; ++z) {
			__m256d vecZ = _mm256_set1_pd(static_cast<double>((chunkZ * 16) + z));
			vecZ = _mm256_mul_pd(vecZ, freq);

			terrainNoise.SampleNoise4(base_x, vecZ, terrain_res);
			tempNoise.SampleNoise4(base_x, vecZ, temp_res);
			moistureNoise.SampleNoise4(base_x, vecZ, moisture_res);

			for (int subX = 0; subX < 4; ++subX) {
				int currentX = x + subX;
				double tVal = temp_res[subX];
				double mVal = moisture_res[subX];

				BiomeID currentBiome = BIOME_PLAINS;
				uint8_t surfaceBlock = 2; // Grass
				uint8_t fillerBlock = 3;  // Dirt
				double heightScale = 1.0;

				if (tVal < 0.25) {
					currentBiome = BIOME_ICE_SPIKES;
					surfaceBlock = 79;
					fillerBlock = 80;
					heightScale = 1.6;
				}
				else if (tVal > 0.70 && mVal < 0.30) {
					currentBiome = BIOME_DESERT;
					surfaceBlock = 12;
					fillerBlock = 12;
					heightScale = 0.8;
				}

				int calculatedHeight = 55 + static_cast<int>(terrain_res[subX] * 30.0 * heightScale);
				calculatedHeight = std::clamp(calculatedHeight, 5, 250);

				for (int y = 0; y < 256; ++y) {
					size_t index = (y * 256) + (z * 16) + currentX;

					if (y == 0) {
						outChunkData[index] = 7; // Bedrock
					}
					else if (y < calculatedHeight - 4) {
						outChunkData[index] = 1; // Stone
					}
					else if (y < calculatedHeight) {
						outChunkData[index] = fillerBlock;
					}
					else if (y == calculatedHeight) {
						outChunkData[index] = surfaceBlock;
					}
				}
			}
		}
	}
	_mm256_zeroupper();
}

static void GarbageCollectStrayEntities() {
	unsigned long long currentTime = static_cast<unsigned long long>(GetTickCount64() / 1000);
	size_t initialSize = GlobalEntities.size();

	EnterCriticalSection(&SessionLock);
	GlobalEntities.erase(std::remove_if(GlobalEntities.begin(), GlobalEntities.end(),
		[currentTime](GAME_ENTITY& entity) {
			bool playerNearby = false;
			for (const auto& [socket, session] : ActiveSessions) {
				if (session.state == STATE_PLAY) {
					int dx = entity.posX - static_cast<int>(session.playerX);
					int dz = entity.posZ - static_cast<int>(session.playerZ);
					if (((dx * dx) + (dz * dz)) <= 16384) {
						playerNearby = true;
						entity.lastActiveTime = currentTime;
						break;
					}
				}
			}
			return !playerNearby && ((currentTime - entity.lastActiveTime) >= 1800);
		}),
		GlobalEntities.end()
	);
	LeaveCriticalSection(&SessionLock);

	size_t removedCount = initialSize - GlobalEntities.size();
	if (removedCount > 0) {
		currentWindow.message = std::format(L"[Entity GC] Evicted {} stray unified mobs/projectiles from memory pools.\n", removedCount);
		WriteConsoleW(currentWindow.hOut, currentWindow.message.c_str(), static_cast<unsigned long>(currentWindow.message.size()), &currentWindow.written, NULL);
	}
}

static void SendChunkColumn(unsigned long long socket, int chunkX, int chunkZ) {
	// Allocate buffers out of WorkerArena instead of the stack
	char* packet = reinterpret_cast<char*>(WorkerArena.Allocate(16384));
	char* framed = reinterpret_cast<char*>(WorkerArena.Allocate(16400));
	size_t p = 0;

	packet[p++] = 0x20; // Chunk Data Packet ID

	int32_t netX = _byteswap_ulong(chunkX);
	int32_t netZ = _byteswap_ulong(chunkZ);
	std::memcpy(&packet[p], &netX, 4); p += 4;
	std::memcpy(&packet[p], &netZ, 4); p += 4;

	packet[p++] = 0x01; // Full chunk flag

	p += WriteVarIntToBuffer(&packet[p], 0x01); // Primary Bitmask

	uint8_t* blockData = reinterpret_cast<uint8_t*>(WorkerArena.Allocate(65536));
	GenerateWorldChunk(chunkX, chunkZ, blockData);

	// Calculate section size (Paletted layout)
	size_t dataLen = 1 + 1 + 2 + (4096 * 13 / 64) * 8 + 2048 + 2048;
	p += WriteVarIntToBuffer(&packet[p], static_cast<int>(dataLen));

	packet[p++] = 13; // Bits per block palette
	p += WriteVarIntToBuffer(&packet[p], 0); // Palette Length 0 (Direct ID mapping)

	// 13-bit Direct-Packed Long Array Data Structure
	size_t longCount = (4096 * 13) / 64;
	p += WriteVarIntToBuffer(&packet[p], static_cast<int>(longCount));

	uint64_t currentLong = 0;
	int bitOffset = 0;

	for (int y = 0; y < 16; ++y) {
		for (int z = 0; z < 16; ++z) {
			for (int x = 0; x < 16; ++x) {
				size_t flatIdx = (y * 256) + (z * 16) + x;
				uint64_t val = static_cast<uint64_t>(blockData[flatIdx]) << 4;

				currentLong |= (val << bitOffset);
				bitOffset += 13;

				if (bitOffset >= 64) {
					uint64_t netLong = _byteswap_uint64(currentLong);
					std::memcpy(&packet[p], &netLong, 8); p += 8;
					bitOffset -= 64;
					currentLong = (bitOffset > 0) ? (val >> (13 - bitOffset)) : 0;
				}
			}
		}
	}
	if (bitOffset > 0) {
		uint64_t netLong = _byteswap_uint64(currentLong);
		std::memcpy(&packet[p], &netLong, 8); p += 8;
	}

	std::memset(&packet[p], 0xFF, 2048); p += 2048; // Block light
	std::memset(&packet[p], 0xFF, 2048); p += 2048; // Sky light
	std::memset(&packet[p], 1, 256); p += 256;      // Biomes

	p += WriteVarIntToBuffer(&packet[p], 0); // 0 NBT Block Entities

	// Framing
	char vLen[5];
	size_t vLenSize = WriteVarIntToBuffer(vLen, static_cast<int>(p));

	std::copy_n(vLen, vLenSize, framed);
	std::copy_n(packet, p, framed + vLenSize);

	SendWebSocketFrame(socket, framed, vLenSize + p);

	// Reset arena offset at the very end of processing
	WorkerArena.Clear();
}

static void ProcessEaglercraftPacket(CONNECTION_CONTEXT* ctx, uint8_t* payload, size_t len) {
	if (len < 1) return;

	EnterCriticalSection(&SessionLock);
	PLAYER_SESSION& session = ActiveSessions[ctx->socket];
	LeaveCriticalSection(&SessionLock);

	size_t cursor = 0;
	size_t packetLenRead = 0;
	int packetLength = ReadVarInt(payload, len, packetLenRead);
	cursor += packetLenRead;

	if (cursor >= len) return;

	size_t idLenRead = 0;
	int packetId = ReadVarInt(payload + cursor, len - cursor, idLenRead);
	cursor += idLenRead;

	if (session.state == STATE_HANDSHAKE) {
		if (packetId == 0x00) { session.state = STATE_LOGIN; }
	}
	else if (session.state == STATE_LOGIN) {
		if (packetId == 0x00) { // Login Start
			size_t strLenRead = 0;
			int strLen = ReadVarInt(payload + cursor, len - cursor, strLenRead);
			cursor += strLenRead;

			if (strLen > 0 && strLen <= 16 && cursor + strLen <= len) {
				std::memcpy(session.username, payload + cursor, strLen);
				session.username[strLen] = '\0';
			}

			// Respond with Login Success (0x02)
			alignas(32) char resp[128];
			size_t rp = 0;
			resp[rp++] = 0x02;

			std::string uuidStr = "c06180a0-6f91-424a-9e19-33152ef61d16";
			rp += WriteVarIntToBuffer(&resp[rp], static_cast<int>(uuidStr.length()));
			std::copy_n(uuidStr.c_str(), uuidStr.length(), &resp[rp]);
			rp += uuidStr.length();

			std::string userStr(session.username);
			rp += WriteVarIntToBuffer(&resp[rp], static_cast<int>(userStr.length()));
			std::copy_n(userStr.c_str(), userStr.length(), &resp[rp]);
			rp += userStr.length();

			char vLen[5];
			size_t vLenSize = WriteVarIntToBuffer(vLen, static_cast<int>(rp));
			alignas(32) char framed[256];
			std::copy_n(vLen, vLenSize, framed);
			std::copy_n(resp, rp, framed + vLenSize);

			SendWebSocketFrame(ctx->socket, framed, vLenSize + rp);
			session.state = STATE_PLAY;

			// Dispatch Play Join Game (0x23)
			alignas(32) char joinPacket[128];
			size_t jp = 0;
			joinPacket[jp++] = 0x23;
			int32_t eId = _byteswap_ulong(session.entityId);
			std::memcpy(&joinPacket[jp], &eId, 4); jp += 4;
			joinPacket[jp++] = 1;
			int32_t dim = 0;
			std::memcpy(&joinPacket[jp], &dim, 4); jp += 4;
			joinPacket[jp++] = 0;
			joinPacket[jp++] = 10;
			std::string levelType = "default";
			jp += WriteVarIntToBuffer(&joinPacket[jp], static_cast<int>(levelType.length()));
			std::copy_n(levelType.c_str(), levelType.length(), &joinPacket[jp]); jp += levelType.length();
			joinPacket[jp++] = 0;

			size_t jvSize = WriteVarIntToBuffer(vLen, static_cast<int>(jp));
			std::copy_n(vLen, jvSize, framed);
			std::copy_n(joinPacket, jp, framed + jvSize);
			SendWebSocketFrame(ctx->socket, framed, jvSize + jp);

			// Dispatch Position and Look (0x2F)
			alignas(32) char posPacket[128];
			size_t pp = 0;
			posPacket[pp++] = 0x2F;
			double px = 8.0, py = 65.0, pz = 8.0;
			uint64_t nx, ny, nz;
			std::memcpy(&nx, &px, 8); nx = _byteswap_uint64(nx); std::memcpy(&posPacket[pp], &nx, 8); pp += 8;
			std::memcpy(&ny, &py, 8); ny = _byteswap_uint64(ny); std::memcpy(&posPacket[pp], &ny, 8); pp += 8;
			std::memcpy(&nz, &pz, 8); nz = _byteswap_uint64(nz); std::memcpy(&posPacket[pp], &nz, 8); pp += 8;
			float yaw = 0.0f, pitch = 0.0f;
			uint32_t nyaw, npitch;
			std::memcpy(&nyaw, &yaw, 4); nyaw = _byteswap_ulong(nyaw); std::memcpy(&posPacket[pp], &nyaw, 4); pp += 4;
			std::memcpy(&npitch, &pitch, 4); npitch = _byteswap_ulong(npitch); std::memcpy(&posPacket[pp], &npitch, 4); pp += 4;
			posPacket[pp++] = 0x00;
			pp += WriteVarIntToBuffer(&posPacket[pp], 1);

			size_t pvSize = WriteVarIntToBuffer(vLen, static_cast<int>(pp));
			std::copy_n(vLen, pvSize, framed);
			std::copy_n(posPacket, pp, framed + pvSize);
			SendWebSocketFrame(ctx->socket, framed, pvSize + pp);

			// Render radius 2 chunks
			for (int cx = -2; cx <= 2; ++cx) {
				for (int cz = -2; cz <= 2; ++cz) {
					SendChunkColumn(ctx->socket, cx, cz);
				}
			}
		}
	}
	else if (session.state == STATE_PLAY) {
		if (packetId == 0x0D || packetId == 0x0E) {
			if (cursor + 24 <= len) {
				uint64_t rawX, rawY, rawZ;
				std::memcpy(&rawX, payload + cursor, 8); rawX = _byteswap_uint64(rawX); std::memcpy(&session.playerX, &rawX, 8);
				std::memcpy(&rawY, payload + cursor + 8, 8); rawY = _byteswap_uint64(rawY); std::memcpy(&session.playerY, &rawY, 8);
				std::memcpy(&rawZ, payload + cursor + 16, 8); rawZ = _byteswap_uint64(rawZ); std::memcpy(&session.playerZ, &rawZ, 8);
			}
		}
	} 
}

static int __stdcall ConsoleCtrlHandler(unsigned long dwCtrlType) {
	if (dwCtrlType == 0 || dwCtrlType == 5) {
		ENGINE = false;
		if (listenSock != INVALID_SOCKET) {
			closesocket(listenSock);
			listenSock = INVALID_SOCKET;
		}
		PostQueuedCompletionStatus(hIOCP, 0, 0, NULL);
		return 1;
	}
	return 0;
}

#endif // !DEFINITIONS_HPP
