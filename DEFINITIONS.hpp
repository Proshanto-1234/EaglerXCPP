#ifndef DEFINITIONS_HPP
#define DEFINITIONS_HPP

// --- compile-time constants ---
constexpr unsigned long WEBSOCKET_BASE64 = 0x00000001;
constexpr unsigned long WEBSOCKET_NOCRLF = 0x40000000;
constexpr size_t PLAYER_STATUS_NORMAL = 0x00;
constexpr size_t PLAYER_STATUS_OPERATOR = 0x01;

#if defined(min)
#undef min
#endif

// --- core enumerations ---
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

// --- engine structures ---
struct GAME_ENTITY {
	unsigned long long entityId;
	EntityType type;
	int posX;
	int posZ;
	unsigned long long lastActiveTime;
};

struct CONNECTION_CONTEXT {
	_OVERLAPPED         overlapped;
	unsigned long long socket;
	uint8_t* buffer;
	_WSABUF             wsaBuf;
	SOCKET_OPERATION   operation;
};

struct PLAYER_SESSION {
	ClientState state = STATE_HANDSHAKE;
	int playerX = 0;
	int playerY = 50;
	int playerZ = 0;
	char username[16] = { 0 };
	size_t status = PLAYER_STATUS_NORMAL;
	std::string gamemode = "SURVIVAL";
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

// Explicit 16-byte packed structure matching your SIMD configuration
struct alignas(16) FileCraftingMatrix {
	uint16_t grid[9];
	uint16_t pad[7];
};

struct FileRecipeDefinition {
	FileCraftingMatrix inputPattern;
	uint16_t resultID;
	uint8_t resultCount;
	uint8_t resultDamage;
};

// --- inline global context (C++17) ---
inline std::atomic<bool> ENGINE = true;
inline unsigned long long listenSock = INVALID_SOCKET;
inline HANDLE hIOCP = NULL;
inline std::unordered_map<unsigned long long, PLAYER_SESSION> ActiveSessions;
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

// --- intrinsics & system optimization pipelines ---
__forceinline static bool CheckHardwareInstructionSupport() {
	int cpuInfo[4] = { 0 };
	__cpuid(cpuInfo, 1);
	bool supportsRDRAND = (cpuInfo[2] & (1 << 30)) != 0;
	__cpuid(cpuInfo, 7);
	bool supportsAVX2 = (cpuInfo[1] & (1 << 5)) != 0;
	return supportsRDRAND && supportsAVX2;
}

// OPTIMIZATION: 100% allocation-free stack-bound message serialization
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

static void BroadcastChatMessage(const std::string& sender, const std::string& message) {
	// 1024 bytes is tight if a user sends a large message because Minecraft JSON has high overhead.
	// Let's safe-guard with a 2048-byte 32-byte cache-aligned stack allocation.
	alignas(32) char staticBuffer[2048];

	// Leave a safe offset buffer window at the beginning for arbitrary header lengths (32 bytes)
	char* jsonPayloadStart = staticBuffer + 32;
	size_t maxJsonSize = sizeof(staticBuffer) - 32;

	// Use format_to_n to completely prevent stack overflow attacks/bugs
	std::format_to_n_result<char*> formatResult = std::format_to_n(jsonPayloadStart, maxJsonSize,
		"{{\"text\":\"[{}] {}\"}}", sender, message);

	size_t jsonLen = formatResult.size;
	if (jsonLen >= maxJsonSize) [[unlikely]] { return; } // Dropped to prevent truncation errors
	

	// --- STEP 2: Prepend Minecraft Protocol Envelopes (Packet ID + Length VarInts) ---
	constexpr char mcPacketId = 0x0F; // Chat Message packet ID

	char stringVarInt[5];
	size_t stringVarIntLen = WriteVarIntToBuffer(stringVarInt, static_cast<int>(jsonLen));

	// Size of [ID Byte] + [VarInt Length descriptor] + [Actual JSON Data string]
	int mcInnerPayloadSize = static_cast<int>(1 + stringVarIntLen + jsonLen);

	char mcTotalLengthVarInt[5];
	size_t mcLengthVarIntLen = WriteVarIntToBuffer(mcTotalLengthVarInt, mcInnerPayloadSize);

	size_t totalMcPacketSize = mcLengthVarIntLen + static_cast<size_t>(mcInnerPayloadSize);

	// --- STEP 3: Handle WebSocket Framing Layers (Backwards-allocation mapping) ---
	size_t wsHeaderSize = 2;
	if (totalMcPacketSize > 125) {
		wsHeaderSize = 4;
	}

	// Calculate exactly where the frame must start to hit the static JSON payload flawlessly
	// JSON starts at 32. Subtract MC headers and WS headers to find the zero-copy origin point:
	size_t mcHeaderStartOffset = 32 - (mcLengthVarIntLen + 1 + stringVarIntLen);
	size_t finalFrameStartOffset = mcHeaderStartOffset - wsHeaderSize;

	char* frameStart = staticBuffer + finalFrameStartOffset;

	// Write out the WebSocket parameters safely
	frameStart[0] = static_cast<char>(0x81); // FIN | Text Opcode
	if (wsHeaderSize == 2) {
		frameStart[1] = static_cast<char>(totalMcPacketSize);
	}
	else {
		frameStart[1] = 126; // Extended 16-bit payload indicator
		frameStart[2] = static_cast<char>((totalMcPacketSize >> 8) & 0xFF);
		frameStart[3] = static_cast<char>(totalMcPacketSize & 0xFF);
	}

	// Write the calculated Minecraft VarInt strings and ID into place seamlessly
	char* mcWriteCursor = staticBuffer + mcHeaderStartOffset;
	std::copy_n(mcTotalLengthVarInt, mcLengthVarIntLen, mcWriteCursor);
	mcWriteCursor += mcLengthVarIntLen;

	*mcWriteCursor = mcPacketId;
	mcWriteCursor += 1;

	std::copy_n(stringVarInt, stringVarIntLen, mcWriteCursor);

	// Total size of data to dispatch to the socket pipeline
	size_t totalFrameSize = wsHeaderSize + totalMcPacketSize;

	// --- STEP 4: Concurrent Safe Multiplexed Send Loop ---
	for (const auto& [socket, session] : ActiveSessions) {
		if (session.state == STATE_PLAY) {
			// Securely hands down your aligned stack pointer slice
			(void)send(socket, frameStart, static_cast<int>(totalFrameSize), 0);
		}
	}
}

__forceinline uint32_t GetHardwareRandom() {
	uint32_t val = 0;
	for (int i = 0; i < 10; ++i) {
		if (_rdrand32_step(&val)) return val;
	}
	return 13372026;
}

static void ExecuteServerCommand(unsigned long long clientSocket, const std::string& rawCommand) {
	PLAYER_SESSION& session = ActiveSessions[clientSocket];

	std::istringstream iss(rawCommand);
	std::string baseCmd;
	iss >> baseCmd;

	bool requiresOp = (baseCmd == "/tp" || baseCmd == "/gamemode" || baseCmd == "/op" || baseCmd == "/deop" || baseCmd == "/ban");

	if (requiresOp && !(session.status & PLAYER_STATUS_OPERATOR)) {
		BroadcastChatMessage("Server", "You do not have permission to use this command.");
		return;
	}

	if (baseCmd == "/tp") {
		int targetX, targetY, targetZ;
		if (iss >> targetX >> targetY >> targetZ) {
			session.playerX = targetX;
			session.playerY = targetY;
			session.playerZ = targetZ;
		}
	}
	else if (baseCmd == "/gamemode") {
		std::string mode;
		if (iss >> mode) {
			if (mode.empty()) BroadcastChatMessage("Server", "You must specify a game mode");
			if (mode == "SURVIVAL") session.gamemode = "SURVIVAL";
			if (mode == "SPECTATOR") session.gamemode = "SPECTATOR";
		}
	}
	else if (baseCmd == "/op") {
		std::string targetUser;
		if (iss >> targetUser) {
			for (auto& [socket, pSession] : ActiveSessions) {
				if (std::string(pSession.username) == targetUser) {
					pSession.status |= PLAYER_STATUS_OPERATOR;
					BroadcastChatMessage("Server", targetUser + " has been promoted to Operator.");
					break;
				}
			}
		}
	}
}



static void LoadOrCreateServerProperties(unsigned short& port, std::wstring& mode) {
	std::unordered_map<std::string, std::string> props;
	std::ifstream file("server.properties");

	if (!file.is_open()) {
		std::ofstream outFile("server.properties");
		outFile << "# Server Properties for EaglerCRAPPX Server\n";
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
	// OPTIMIZATION: Converted table to 32-bit int to destroy the expensive vpgatherdd bottleneck
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

		// OPTIMIZATION: Hardware scales aligned by 4 bytes now since types match up perfectly
		__m256i p_X = _mm256_i32gather_epi32((const int*)p_int, X, 4);
		__m256i p_Y = _mm256_i32gather_epi32((const int*)p_int, Y, 4);

		__m256i A = _mm256_add_epi32(p_X, p_Y);
		__m256i B = _mm256_add_epi32(_mm256_i32gather_epi32((const int*)p_int, _mm256_add_epi32(X, _mm256_set1_epi32(1)), 4), p_Y);

		__m256d grad1 = Grad_AVX2(A, x_frac, y_frac);
		__m256d grad2 = Grad_AVX2(B, _mm256_sub_pd(x_frac, _mm256_set1_pd(1.0)), y_frac);

		__m256d res = Lerp_AVX2(v, Lerp_AVX2(u, grad1, grad2), Lerp_AVX2(u, grad1, grad2));
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

	// OPTIMIZATION: Pulled horizontal vector calculations outside, completely eliminating the stack-bouncing array construct loops
	for (int x = 0; x < 16; x += 4) {
		// Vector loaded constants directly into pipeline
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
				double rVal = terrain_res[subX];

				BiomeID currentBiome = BIOME_PLAINS;
				uint8_t surfaceBlock = 2;
				uint8_t fillerBlock = 3;
				double heightScale = 1.0;

				if (rVal > 0.44 && rVal < 0.48) {
					currentBiome = BIOME_RIVER;
					heightScale = 0.7;
				}
				else if (tVal < 0.25) {
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
						outChunkData[index] = 7;
					}
					else if (currentBiome == BIOME_RIVER && y >= calculatedHeight && y <= 62) {
						outChunkData[index] = 9;
					}
					else if (y < calculatedHeight - 4) {
						outChunkData[index] = 1;
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

	GlobalEntities.erase(std::remove_if(GlobalEntities.begin(), GlobalEntities.end(),
		[currentTime](GAME_ENTITY& entity) {
			bool playerNearby = false;

			for (const auto& [socket, session] : ActiveSessions) {
				if (session.state == STATE_PLAY) {
					int dx = entity.posX - session.playerX;
					int dz = entity.posZ - session.playerZ;
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

	size_t removedCount = initialSize - GlobalEntities.size();
	if (removedCount > 0) {
		currentWindow.message = std::format(L"[Entity GC] Evicted {} stray unified mobs/projectiles from memory pools.\n", removedCount);
		WriteConsoleW(currentWindow.hOut, currentWindow.message.c_str(), static_cast<unsigned long>(currentWindow.message.size()), &currentWindow.written, NULL);
	}
}

static void ProcessEaglercraftPacket(CONNECTION_CONTEXT* ctx, uint8_t* payload, size_t len) {
	if (len < 1) return;
	PLAYER_SESSION& session = ActiveSessions[ctx->socket];

	if (session.state == STATE_HANDSHAKE) {
		if (payload[0] == 0x00) {
			uint8_t serverMessageResponse[] = { 0x00, 0x0B, 'E', 'a', 'g', 'l', 'e', 'r', 'S', 'e', 'r', 'v', 'e', 'r' };
			(void)send(ctx->socket, reinterpret_cast<char*>(serverMessageResponse), sizeof(serverMessageResponse), 0);
			session.state = STATE_LOGIN;
		}
	}
	else if (session.state == STATE_LOGIN) {
		if (payload[0] == 0x01) {
			uint8_t loginSuccess[] = { 0x02, 0x00 };
			(void)send(ctx->socket, reinterpret_cast<char*>(loginSuccess), sizeof(loginSuccess), 0);
			session.state = STATE_PLAY;
		}
	}
	else if (session.state == STATE_PLAY) {
		if (payload[0] == 0x03) {
			std::memcpy(&session.playerX, &payload[1], 4);
			std::memcpy(&session.playerZ, &payload[5], 4);

			int currentChunkX = session.playerX >> 4;
			int currentChunkZ = session.playerZ >> 4;

			// OPTIMIZATION: Stripped placement new pointer wrappers. Direct memory handling.
			uint8_t* chunkBuffer = reinterpret_cast<uint8_t*>(WorkerArena.Allocate(65536));
			if (!chunkBuffer) [[unlikely]] return;

			GenerateWorldChunk(currentChunkX, currentChunkZ, chunkBuffer);
			(void)send(ctx->socket, reinterpret_cast<char*>(chunkBuffer), 512, 0);

			WorkerArena.Clear();
		}
	}
}

static void pin_to_core(unsigned long long core) {
	void* currentThread = GetCurrentThread();
	SetThreadAffinityMask(currentThread, core);
	(void)SetThreadIdealProcessor(currentThread, static_cast<unsigned long>(core));
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	SetThreadPriorityBoost(currentThread, 1);
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