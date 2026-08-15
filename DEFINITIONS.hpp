#ifndef DEFINITIONS_HPP
#define DEFINITIONS_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include <immintrin.h>
#include <stdint.h>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>
#include <atomic>

#define OP_HANDSHAKE 1
#define OP_READ 2
#define OP_SOKT_RECYCLE 3

#define WEBSOCKET_NOCRLF 0x40000000
#define WEBSOCKET_BASE64 0x00000001

#define ERROR_UNSUPPORTED_HARDWARE 0xE0000001
#define ERROR_INTERNAL_NTDLL_FAILURE 0xE0000002
#define ERROR_CRITICAL_MEMORY_FAILURE 0xE0000003

// Block Identifiers (Minecraft 1.12.2 IDs)
constexpr uint8_t BLOCK_AIR = 0;
constexpr uint8_t BLOCK_STONE = 1;
constexpr uint8_t BLOCK_GRASS = 2;
constexpr uint8_t BLOCK_DIRT = 3;
constexpr uint8_t BLOCK_BEDROCK = 7;
constexpr uint8_t BLOCK_WATER = 9;
constexpr uint8_t BLOCK_SAND = 12;
constexpr uint8_t BLOCK_ICE = 79;

// Biome Identifiers
constexpr uint8_t BIOME_PLAINS = 1;
constexpr uint8_t BIOME_DESERT = 2;
constexpr uint8_t BIOME_ICE_SPIKES = 140;

struct WindowContext {
    HANDLE hOut = NULL;
    HANDLE hErr = NULL;
    std::wstring message;
    DWORD written = 0;

    bool Initialize() {
        hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        hErr = GetStdHandle(STD_ERROR_HANDLE);
        return (hOut != INVALID_HANDLE_VALUE && hErr != INVALID_HANDLE_VALUE);
    }
};

WindowContext currentWindow;
HANDLE hIOCP = NULL;
SOCKET listenSock = INVALID_SOCKET;
CRITICAL_SECTION SessionLock;
std::atomic<bool> ENGINE{ true };

// Dynamic Player Session Entity State
struct PLAYER_SESSION {
    int32_t entityId = 1;
    double x = 0.0;
    double y = 80.0;
    double z = 0.0;
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool onGround = true;
    uint8_t gameMode = 1; // 1 = Creative, 0 = Survival
    int dimension = 0;    // 0 = Overworld
    uint8_t selectedSlot = 0;
    uint8_t heldItemBlock = BLOCK_STONE;
};

std::unordered_map<SOCKET, PLAYER_SESSION> ActiveSessions;

// Consolidated Unified Connection Context for RIO & IOCP Compatibility
struct CONNECTION_CONTEXT {
    OVERLAPPED overlapped;
    SOCKET socket;
    uint8_t* buffer;
    size_t rxBufferOffset;
    DWORD operation;
    WSABUF wsaBuf;
    RIO_RQ requestQueue;
    RIO_BUF rioBuf;
    DWORD bufferSliceIndex;
};

// Zero-Allocation Thread Arena Allocator (128 KB Ring Buffer)
struct ThreadArena {
    alignas(32) uint8_t pool[131072];
    size_t offset = 0;

    void* Allocate(size_t size) {
        size_t alignedSize = (size + 31) & ~31;
        if (offset + alignedSize > sizeof(pool)) {
            offset = 0;
        }
        void* ptr = &pool[offset];
        offset += alignedSize;
        return ptr;
    }

    void Clear() {
        offset = 0;
    }
};

thread_local ThreadArena WorkerArena;

// AVX2 SIMD Perlin Noise Engine
class AVX2NoiseEngine {
public:
    static __m256d Fade(__m256d t) {
        __m256d t3 = _mm256_mul_pd(_mm256_mul_pd(t, t), t);
        __m256d t4 = _mm256_mul_pd(t3, t);
        __m256d t5 = _mm256_mul_pd(t4, t);

        __m256d c6 = _mm256_set1_pd(6.0);
        __m256d c15 = _mm256_set1_pd(15.0);
        __m256d c10 = _mm256_set1_pd(10.0);

        __m256d term = _mm256_sub_pd(_mm256_mul_pd(t, c6), c15);
        term = _mm256_add_pd(_mm256_mul_pd(t, term), c10);
        return _mm256_mul_pd(t3, term);
    }

    static __m256d Grad(__m256i hash, __m256d x, __m256d y) {
        alignas(32) int32_t h[4];
        alignas(32) double xv[4], yv[4], res[4];
        _mm_storeu_si128(reinterpret_cast<__m128i*>(h), _mm256_castsi256_si128(hash));
        _mm256_store_pd(xv, x);
        _mm256_store_pd(yv, y);

        for (int i = 0; i < 4; ++i) {
            int g = h[i] & 7;
            double u = g < 4 ? xv[i] : yv[i];
            double v = g < 4 ? yv[i] : xv[i];
            res[i] = ((g & 1) ? -u : u) + ((g & 2) ? -2.0 * v : 2.0 * v);
        }
        return _mm256_load_pd(res);
    }

    static __m256d SampleNoise4(__m256d vx, __m256d vy, const int32_t* p) {
        __m256d xi = _mm256_floor_pd(vx);
        __m256d yi = _mm256_floor_pd(vy);

        __m256i X = _mm256_cvtpd_epi32(_mm256_and_pd(xi, _mm256_set1_pd(255.0)));
        __m256i Y = _mm256_cvtpd_epi32(_mm256_and_pd(yi, _mm256_set1_pd(255.0)));

        __m256d xf = _mm256_sub_pd(vx, xi);
        __m256d yf = _mm256_sub_pd(vy, yi);

        __m256d u = Fade(xf);
        __m256d v = Fade(yf);

        __m256i aa = _mm256_i32gather_epi32(p, X, 4);
        aa = _mm256_add_epi32(aa, Y);

        alignas(32) int32_t indices[4];
        _mm_storeu_si128(reinterpret_cast<__m128i*>(indices), _mm256_castsi256_si128(aa));

        int32_t p_aa[4], p_ab[4], p_ba[4], p_bb[4];
        for (int i = 0; i < 4; ++i) {
            p_aa[i] = p[indices[i]];
            p_ab[i] = p[indices[i] + 1];
            p_ba[i] = p[indices[i] + 16];
            p_bb[i] = p[indices[i] + 17];
        }

        __m256i v_aa = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p_aa));
        __m256i v_ab = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p_ab));
        __m256i v_ba = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p_ba));
        __m256i v_bb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p_bb));

        __m256d g1 = Grad(v_aa, xf, yf);
        __m256d g2 = Grad(v_ba, _mm256_sub_pd(xf, _mm256_set1_pd(1.0)), yf);
        __m256d g3 = Grad(v_ab, xf, _mm256_sub_pd(yf, _mm256_set1_pd(1.0)));
        __m256d g4 = Grad(v_bb, _mm256_sub_pd(xf, _mm256_set1_pd(1.0)), _mm256_sub_pd(yf, _mm256_set1_pd(1.0)));

        __m256d x1 = _mm256_add_pd(g1, _mm256_mul_pd(u, _mm256_sub_pd(g2, g1)));
        __m256d x2 = _mm256_add_pd(g3, _mm256_mul_pd(u, _mm256_sub_pd(g4, g3)));

        return _mm256_add_pd(x1, _mm256_mul_pd(v, _mm256_sub_pd(x2, x1)));
    }
};

const int32_t PERM_TABLE[512] = {
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,
    8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,
    35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,
    134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,
    55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,
    18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,
    226,250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,
    17,182,189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,
    167,43,172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,
    246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,
    14,239,107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,
    4,150,254,138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,
    156,180
};

bool CheckHardwareInstructionSupport() {
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    bool avxSupport = (cpuInfo[2] & (1 << 28)) != 0;
    bool rdrandSupport = (cpuInfo[2] & (1 << 30)) != 0;
    __cpuidex(cpuInfo, 7, 0);
    bool avx2Support = (cpuInfo[1] & (1 << 5)) != 0;
    return avxSupport && avx2Support && rdrandSupport;
}

void LoadOrCreateServerProperties(unsigned short& port, std::wstring& mode) {
    port = 25565;
    mode = L"Eaglercraft-1.12.2-Native";
}

BOOL WINAPI ConsoleCtrlHandler(DWORD fdwCtrlType) {
    if (fdwCtrlType == CTRL_C_EVENT || fdwCtrlType == CTRL_CLOSE_EVENT) {
        ENGINE.store(false);
        return TRUE;
    }
    return FALSE;
}

// Protocol VarInt Encoding/Decoding
void WriteVarIntToBuffer(std::vector<uint8_t>& buf, int32_t value) {
    uint32_t uval = static_cast<uint32_t>(value);
    while (true) {
        if ((uval & ~0x7F) == 0) {
            buf.push_back(static_cast<uint8_t>(uval));
            return;
        }
        buf.push_back(static_cast<uint8_t>((uval & 0x7F) | 0x80));
        uval >>= 7;
    }
}

int32_t ReadVarInt(const uint8_t* buffer, size_t size, size_t& bytesRead) {
    int32_t result = 0;
    int32_t shift = 0;
    bytesRead = 0;

    while (bytesRead < size) {
        uint8_t byte = buffer[bytesRead++];
        result |= static_cast<int32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return result;
        shift += 7;
        if (shift >= 35) return 0; // Malformed VarInt
    }
    return result;
}

// World Generation Helper
void GenerateWorldChunk(int cx, int cz, uint8_t* outChunkData) {
    std::memset(outChunkData, 0, 65536);

    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            double worldX = (cx * 16 + x) * 0.05;
            double worldZ = (cz * 16 + z) * 0.05;

            __m256d vx = _mm256_set1_pd(worldX);
            __m256d vy = _mm256_set1_pd(worldZ);

            __m256d noiseVal = AVX2NoiseEngine::SampleNoise4(vx, vy, PERM_TABLE);
            alignas(32) double nResult[4];
            _mm256_store_pd(nResult, noiseVal);

            int height = 64 + static_cast<int>(nResult[0] * 12.0);
            height = (std::max)(1, (std::min)(255, height));

            for (int y = 0; y <= height; ++y) {
                int blockIdx = (y * 256) + (z * 16) + x;
                if (y == 0) {
                    outChunkData[blockIdx] = BLOCK_BEDROCK;
                }
                else if (y < height - 3) {
                    outChunkData[blockIdx] = BLOCK_STONE;
                }
                else if (y < height) {
                    outChunkData[blockIdx] = BLOCK_DIRT;
                }
                else {
                    outChunkData[blockIdx] = BLOCK_GRASS;
                }
            }
        }
    }
}

// Low-level Direct Packet Sender via WebSockets
void SendWebSocketFrame(SOCKET sock, const uint8_t* payload, size_t length) {
    std::vector<uint8_t> frame;
    frame.push_back(0x82); // Binary frame header

    if (length <= 125) {
        frame.push_back(static_cast<uint8_t>(length));
    }
    else if (length <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(length & 0xFF));
    }
    else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((length >> (i * 8)) & 0xFF));
        }
    }

    frame.insert(frame.end(), payload, payload + length);
    (void)::send(sock, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0);
}

// Encodes and sends 1.12.2 Chunk Data Packet (0x20)
void SendChunkColumn(SOCKET sock, int cx, int cz, const uint8_t* chunkData) {
    std::vector<uint8_t> packet;
    WriteVarIntToBuffer(packet, 0x20); // Chunk Data Packet ID

    // Chunk Coordinates
    uint32_t ncx = _byteswap_ulong(static_cast<uint32_t>(cx));
    uint32_t ncz = _byteswap_ulong(static_cast<uint32_t>(cz));
    packet.insert(packet.end(), reinterpret_cast<uint8_t*>(&ncx), reinterpret_cast<uint8_t*>(&ncx) + 4);
    packet.insert(packet.end(), reinterpret_cast<uint8_t*>(&ncz), reinterpret_cast<uint8_t*>(&ncz) + 4);

    packet.push_back(1); // Full chunk ground-up continuous
    WriteVarIntToBuffer(packet, 0x01); // Primary Bit Mask (First 16-block section)

    // Calculate Direct-Packed 13-bit Long Array Data Size
    std::vector<uint8_t> dataBuf;
    WriteVarIntToBuffer(dataBuf, 8); // Bits per block palette
    WriteVarIntToBuffer(dataBuf, 0); // Palette size = 0 (Direct global palette)
    WriteVarIntToBuffer(dataBuf, 64); // 64 uint64_t longs for 4096 blocks (13-bit packing)

    for (int i = 0; i < 64; ++i) {
        uint64_t dummyLong = 0;
        dataBuf.insert(dataBuf.end(), reinterpret_cast<uint8_t*>(&dummyLong), reinterpret_cast<uint8_t*>(&dummyLong) + 8);
    }

    // Biomes (256 bytes)
    for (int i = 0; i < 256; ++i) dataBuf.push_back(BIOME_PLAINS);

    WriteVarIntToBuffer(packet, static_cast<int32_t>(dataBuf.size()));
    packet.insert(packet.end(), dataBuf.begin(), dataBuf.end());

    WriteVarIntToBuffer(packet, 0); // Number of Block Entities

    SendWebSocketFrame(sock, packet.data(), packet.size());
}

// Dynamic Block Mutation Helper
void SetWorldBlock(int32_t x, int32_t y, int32_t z, uint8_t blockId) {
    if (y < 0 || y >= 256) return;
    int cx = x >> 4;
    int cz = z >> 4;
    int bx = x & 15;
    int bz = z & 15;

    // Direct memory update into active world memory if in 64x64 region
    if (cx >= 0 && cx < 64 && cz >= 0 && cz < 64) {
        extern unsigned char* GlobalWorldMemory;
        if (GlobalWorldMemory) {
            int chunkIndex = (cz * 64) + cx;
            uint8_t* chunkPtr = GlobalWorldMemory + (chunkIndex * 65536);
            int blockIdx = (y * 256) + (bz * 16) + bx;
            chunkPtr[blockIdx] = blockId;
        }
    }
}

// Packet Processor with Full Gameplay Decoding
void ProcessEaglercraftPacket(CONNECTION_CONTEXT* ctx, uint8_t* payload, size_t length) {
    if (length == 0) return;

    size_t bytesRead = 0;
    int32_t packetId = ReadVarInt(payload, length, bytesRead);
    uint8_t* body = payload + bytesRead;
    size_t bodyLen = length - bytesRead;

    // Packet 0x00: Login Start / Handshake Init
    if (packetId == 0x00 && ctx->operation == OP_READ) {
        std::vector<uint8_t> loginSuccess;
        WriteVarIntToBuffer(loginSuccess, 0x02); // Login Success Packet
        
        std::string uuid = "00000000-0000-0000-0000-000000000001";
        WriteVarIntToBuffer(loginSuccess, static_cast<int32_t>(uuid.length()));
        loginSuccess.insert(loginSuccess.end(), uuid.begin(), uuid.end());

        std::string username = "Player";
        WriteVarIntToBuffer(loginSuccess, static_cast<int32_t>(username.length()));
        loginSuccess.insert(loginSuccess.end(), username.begin(), username.end());

        SendWebSocketFrame(ctx->socket, loginSuccess.data(), loginSuccess.size());

        // Join Game (0x23)
        std::vector<uint8_t> joinGame;
        WriteVarIntToBuffer(joinGame, 0x23);
        
        int32_t eid = 100;
        uint32_t neid = _byteswap_ulong(static_cast<uint32_t>(eid));
        joinGame.insert(joinGame.end(), reinterpret_cast<uint8_t*>(&neid), reinterpret_cast<uint8_t*>(&neid) + 4);

        joinGame.push_back(1); // Creative Mode
        
        int32_t dim = 0;
        uint32_t ndim = _byteswap_ulong(static_cast<uint32_t>(dim));
        joinGame.insert(joinGame.end(), reinterpret_cast<uint8_t*>(&ndim), reinterpret_cast<uint8_t*>(&ndim) + 4);

        joinGame.push_back(0); // Difficulty
        joinGame.push_back(100); // Max Players
        
        std::string levelType = "default";
        WriteVarIntToBuffer(joinGame, static_cast<int32_t>(levelType.length()));
        joinGame.insert(joinGame.end(), levelType.begin(), levelType.end());
        joinGame.push_back(0); // Reduced Debug Info

        SendWebSocketFrame(ctx->socket, joinGame.data(), joinGame.size());

        // Player Position & Look Sync (0x2F)
        std::vector<uint8_t> posPacket;
        WriteVarIntToBuffer(posPacket, 0x2F);

        double px = 0.0, py = 80.0, pz = 0.0;
        float yaw = 0.0f, pitch = 0.0f;
        uint64_t nx, ny, nz;
        std::memcpy(&nx, &px, 8); nx = _byteswap_uint64(nx);
        std::memcpy(&ny, &py, 8); ny = _byteswap_uint64(ny);
        std::memcpy(&nz, &pz, 8); nz = _byteswap_uint64(nz);

        posPacket.insert(posPacket.end(), reinterpret_cast<uint8_t*>(&nx), reinterpret_cast<uint8_t*>(&nx) + 8);
        posPacket.insert(posPacket.end(), reinterpret_cast<uint8_t*>(&ny), reinterpret_cast<uint8_t*>(&ny) + 8);
        posPacket.insert(posPacket.end(), reinterpret_cast<uint8_t*>(&nz), reinterpret_cast<uint8_t*>(&nz) + 8);

        uint32_t nyaw, npitch;
        std::memcpy(&nyaw, &yaw, 4); nyaw = _byteswap_ulong(nyaw);
        std::memcpy(&npitch, &pitch, 4); npitch = _byteswap_ulong(npitch);
        posPacket.insert(posPacket.end(), reinterpret_cast<uint8_t*>(&nyaw), reinterpret_cast<uint8_t*>(&nyaw) + 4);
        posPacket.insert(posPacket.end(), reinterpret_cast<uint8_t*>(&npitch), reinterpret_cast<uint8_t*>(&npitch) + 4);

        posPacket.push_back(0x00); // Teleport Flags
        WriteVarIntToBuffer(posPacket, 1); // Teleport ID

        SendWebSocketFrame(ctx->socket, posPacket.data(), posPacket.size());

        // Stream initial 3x3 Chunk Grid
        extern unsigned char* GlobalWorldMemory;
        if (GlobalWorldMemory) {
            for (int cz = -1; cz <= 1; ++cz) {
                for (int cx = -1; cx <= 1; ++cx) {
                    SendChunkColumn(ctx->socket, cx, cz, GlobalWorldMemory);
                }
            }
        }
    }
    // Packet 0x0D / 0x0E / 0x0F: Player Position & Rotation Tracking
    else if ((packetId == 0x0D || packetId == 0x0E || packetId == 0x0F) && bodyLen >= 24) {
        double px, py, pz;
        uint64_t rawX, rawY, rawZ;
        std::memcpy(&rawX, body, 8); rawX = _byteswap_uint64(rawX);
        std::memcpy(&rawY, body + 8, 8); rawY = _byteswap_uint64(rawY);
        std::memcpy(&rawZ, body + 16, 8); rawZ = _byteswap_uint64(rawZ);
        std::memcpy(&px, &rawX, 8);
        std::memcpy(&py, &rawY, 8);
        std::memcpy(&pz, &rawZ, 8);

        EnterCriticalSection(&SessionLock);
        auto it = ActiveSessions.find(ctx->socket);
        if (it != ActiveSessions.end()) {
            it->second.x = px;
            it->second.y = py;
            it->second.z = pz;
        }
        LeaveCriticalSection(&SessionLock);
    }
    // Packet 0x14: Player Digging (Block Break Mechanics)
    else if (packetId == 0x14 && bodyLen >= 11) {
        uint8_t status = body[0];
        uint64_t val;
        std::memcpy(&val, body + 1, 8);
        val = _byteswap_uint64(val);

        int32_t x = static_cast<int32_t>(val >> 38);
        int32_t y = static_cast<int32_t>((val >> 26) & 0xFFF);
        int32_t z = static_cast<int32_t>(val << 38 >> 38);

        if (status == 0 || status == 2) { // Started or Finished digging -> Destroy Block
            SetWorldBlock(x, y, z, BLOCK_AIR);
        }
    }
    // Packet 0x1F: Player Block Placement
    else if (packetId == 0x1F && bodyLen >= 12) {
        uint64_t val;
        std::memcpy(&val, body, 8);
        val = _byteswap_uint64(val);

        int32_t x = static_cast<int32_t>(val >> 38);
        int32_t y = static_cast<int32_t>((val >> 26) & 0xFFF);
        int32_t z = static_cast<int32_t>(val << 38 >> 38);
        int32_t face = body[8];

        // Offset target coordinates based on clicked block face
        if (face == 0) y--;
        else if (face == 1) y++;
        else if (face == 2) z--;
        else if (face == 3) z++;
        else if (face == 4) x--;
        else if (face == 5) x++;

        uint8_t blockToPlace = BLOCK_STONE;
        EnterCriticalSection(&SessionLock);
        auto it = ActiveSessions.find(ctx->socket);
        if (it != ActiveSessions.end()) {
            blockToPlace = it->second.heldItemBlock;
        }
        LeaveCriticalSection(&SessionLock);

        SetWorldBlock(x, y, z, blockToPlace);
    }
}

// Entity & Session Maintenance Cleanup
void GarbageCollectStrayEntities() {
    EnterCriticalSection(&SessionLock);
    // Prune stale or unreferenced player session handles
    for (auto it = ActiveSessions.begin(); it != ActiveSessions.end();) {
        if (it->first == INVALID_SOCKET) {
            it = ActiveSessions.erase(it);
        }
        else {
            ++it;
        }
    }
    LeaveCriticalSection(&SessionLock);
}
#endif // DEIFINITIONS.hpp
