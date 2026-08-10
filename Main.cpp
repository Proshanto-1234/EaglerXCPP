/*
* Necessary macros and compiler checks here
*/
#ifndef __cplusplus
#error This program uses features from the C++ language,ensure you are using a C++ compiler (ideally MSVC latest)
#elif (!(defined(_WIN64) || defined(_M_X64)))
#error This program requires 64-bit Windows environment, ensure you are using the 64-bit compiler or runtime environment
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

// Include Win32 API headers first
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>    
#include <bcrypt.h>
#include <wincrypt.h>
#include <tbs.h>
#include <processthreadsapi.h>
#include <process.h>

// Include C++ standard library headers
#include <unordered_map>
#include <vector>
#include <format>
#include <span>
#include <array>
#include <cstring>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <functional>
#include <string>
#include <string_view>
#include <stdint.h>
#include <atomic>
#include <numeric>
#include <memory>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>

// Include CPU-specific headers last
#include <immintrin.h> 

#pragma comment(lib, "tbs.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")

#include "DEFINITIONS.hpp"
#include "SYSTEMINFO_VIEW.hpp"

static void* hGenerationThreadSignal = nullptr;
inline unsigned char* GlobalWorldMemory = nullptr;
constexpr int WORLD_CHUNKS_X = 64;
constexpr int WORLD_CHUNKS_Z = 64;
constexpr unsigned long long CHUNK_SIZE = 65536;
unsigned long __stdcall WorldGenerationWorkerThread(void* lpParam) {
	// Pin this thread to a dedicated core to let AVX2 burn at maximum frequency
    NtPinThread(GetCurrentThread(), 2);

	int index = 0;
	// Iterate through our 64x64 chunk grid
	for (int cz = 0; cz < 64; ++cz) {
		for (int cx = 0; cx < 64; ++cx) {
			uint8_t* chunkPtr = GlobalWorldMemory + (index * 65536);
			GenerateWorldChunk(cx, cz, chunkPtr);
			index++;
		}
	}

	// Signal main() that the map is ready!
	SetEvent(hGenerationThreadSignal);
	return 0;
}

int main(int argc, char* argv[]) {
	if (!currentWindow.Initialize())
	{
        if (GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE)
        {
            if (WriteConsoleW(GetStdHandle(STD_ERROR_HANDLE), L"CRITICAL ERROR: Failed to initialize console handles. Please check whether your Windows version or Console is compatible\n", 124, &currentWindow.written, NULL ) == 0 ) [[unlikely]] { // These errors are rare anyways
                __debugbreak(); // If even writing to the console fails, break into the debugger for maximum visibility of the issue
            }
        }
        return ERROR_INTERNAL;
	}

    if (!CheckHardwareInstructionSupport()) {
        currentWindow.message = L"CRITICAL ERROR: Missing hardware extensions (AVX2 or RDRAND).\n";
        WriteConsoleW(currentWindow.hErr, currentWindow.message.c_str(), static_cast<unsigned long>(currentWindow.message.size()), &currentWindow.written, NULL);
        return ERROR_UNSUPPORTED_HARDWARE;
    }

    unsigned long dwBytes = 0;
    std::wstring mode = L"Eaglercraft-1.12-u3";
    unsigned short targetPort = 0;

    LoadOrCreateServerProperties(targetPort, mode);

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            std::string narrowMode = argv[++i];
            mode = std::wstring(narrowMode.begin(), narrowMode.end());
        }
        else if (arg == "--port" && i + 1 < argc) {
            targetPort = static_cast<unsigned short>(std::atoi(argv[++i]));
        }
    }
    if (!LoadNtFunctions()) {
        currentWindow.message = L"CRITICAL ERROR: Could not load neccessary NT DLL functions.";
		WriteConsoleW(currentWindow.hErr, currentWindow.message.c_str(), static_cast<unsigned long>(currentWindow.message.size()), &currentWindow.written, NULL);
        return ERROR_INTERNAL_NTDLL_FAILURE;
    }
	std::pair<unsigned long, unsigned long> corePairs = DynamicGetLeastUsedCores(); // Retrieve the two least-used cores for running the program smoothly without other programs interrupting
	NtPinThread(GetCurrentThread(), corePairs.first); // Pin the main thread to the least-used core

    SetConsoleCtrlHandler(ConsoleCtrlHandler, 1);

    WSAData wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

	hIOCP = CreateIoCompletionPort (( (void*)(long long)-1), NULL, 0, 0 );
    listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    LPFN_ACCEPTEX lpfnAcceptEx = NULL;
    _GUID GuidAcceptEx = WSAID_ACCEPTEX;
    unsigned long dwBytes_ioctl = 0;

	(void)WSAIoctl(listenSock, ((0x80000000 | 0x40000000) | (0x08000000) | (6)), &GuidAcceptEx, sizeof(GuidAcceptEx), &lpfnAcceptEx, sizeof(lpfnAcceptEx), &dwBytes_ioctl, NULL, NULL);

    sockaddr_in service = {};
    service.sin_family = AF_INET;
    service.sin_addr.s_addr = INADDR_ANY;
    service.sin_port = htons(targetPort);

    if (bind(listenSock, (sockaddr*)&service, sizeof(service)) != 0) return ERROR_INTERNAL;
    if (listen(listenSock, SOMAXCONN) != 0) return ERROR_INTERNAL;

    CreateIoCompletionPort(reinterpret_cast<void*>(listenSock), hIOCP, static_cast<unsigned long long>(listenSock), 0);

    std::vector<CONNECTION_CONTEXT*> pool;
    pool.reserve(32);
    for (int i = 0; i < 32; ++i) {
        CONNECTION_CONTEXT* ctx = new CONNECTION_CONTEXT();
        ctx->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ctx->socket == INVALID_SOCKET) return ERROR_CRITICAL_MEMORY_FAILURE;
        // Aligned allocation to match AVX2 instructions perfectly
        ctx->buffer = reinterpret_cast<uint8_t*>(_aligned_malloc(16384, 32));
        if (!ctx->buffer) return ERROR_CRITICAL_MEMORY_FAILURE;
        ctx->operation = OP_HANDSHAKE;
        ctx->wsaBuf.buf = (char*)ctx->buffer;
        ctx->wsaBuf.len = 16384;

        CreateIoCompletionPort((void*)ctx->socket, hIOCP, (unsigned long long)ctx, 0);
        memset(&ctx->overlapped, 0, sizeof(_OVERLAPPED));

        (void)lpfnAcceptEx(listenSock, ctx->socket, ctx->buffer, 0,
            sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &dwBytes, &ctx->overlapped);
        pool.push_back(ctx);
    }

    currentWindow.message = std::format(L"WebSockets relay Eaglercraft protocol active on Port {} [Mode: {}]...\n", targetPort, mode);
    WriteConsoleW(currentWindow.hOut, currentWindow.message.c_str(), static_cast<unsigned long>(currentWindow.message.size()), &currentWindow.written, NULL);

    _OVERLAPPED_ENTRY entries[16] = {};
    unsigned long removed = 0;
    unsigned long long lastCleanupTime = GetTickCount64() / 1000;

    while (ENGINE) {
        removed = 0;
		if (!GetQueuedCompletionStatusEx(hIOCP, entries, 16, &removed, 1000, 0)) {
            if (!ENGINE) break;
        }

        // --- PERIODIC CLEANUP CHECK ---
        unsigned long long now = GetTickCount64() / 1000;
        if (now - lastCleanupTime >= 60) {
            GarbageCollectStrayEntities();
            lastCleanupTime = now;
        }

        for (unsigned long i = 0; i < removed; ++i) {
            unsigned long bytes = entries[i].dwNumberOfBytesTransferred;
            CONNECTION_CONTEXT* ctx = (CONNECTION_CONTEXT*)entries[i].lpOverlapped;

            if (bytes == 0) {
                std::memset (&ctx->overlapped, 0, sizeof(ctx->overlapped)); ctx->wsaBuf.buf = reinterpret_cast<char*>(ctx->buffer);
				ctx->wsaBuf.len = 16384; // Reset buffer state for next connection
                ctx->operation = OP_SOKT_RECYCLE;
                std::memset(&ctx->overlapped, 0, sizeof(_OVERLAPPED));
                (void)TransmitFile(ctx->socket, NULL, 0, 0, &ctx->overlapped, NULL, TF_DISCONNECT | TF_REUSE_SOCKET);
                continue;
            }

            if (ctx->operation == OP_SOKT_RECYCLE) {
                ctx->operation = OP_HANDSHAKE;
                std::memset(&ctx->overlapped, 0, sizeof(_OVERLAPPED));
                ctx->wsaBuf.buf = reinterpret_cast<char*>(ctx->buffer);
                ctx->wsaBuf.len = 16384; // Reset buffer state for next connection
                (void)lpfnAcceptEx(listenSock, ctx->socket, ctx->buffer, 0,
                    sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &dwBytes, &ctx->overlapped);
                continue;
            }

            if (ctx->operation == OP_HANDSHAKE) {
                (void)setsockopt(ctx->socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&listenSock, sizeof(listenSock));

                // OPTIMIZATION: Use string_view to eliminate heap allocations during parsing
                std::basic_string_view<char> requestStr(reinterpret_cast<char*>(ctx->buffer), bytes);
                if (requestStr.find("GET ") != std::string_view::npos) {
                    std::string handshakeResponse = ProcessWebSocketHandshake(requestStr);
                    (void)send(ctx->socket, handshakeResponse.c_str(), static_cast<int>(handshakeResponse.size()), 0);
                    ctx->operation = OP_READ;
                }
                else {
                    ctx->operation = OP_SOKT_RECYCLE;
                    (void)TransmitFile(ctx->socket, NULL, 0, 0, &ctx->overlapped, NULL, TF_DISCONNECT | TF_REUSE_SOCKET);
                    continue;
                }
            }
            else if (ctx->operation == OP_READ) {
                uint8_t opcode = ctx->buffer[0] & 0x0F;

                if (opcode == 0x08) { // Connection Close Opcode
                    ctx->operation = OP_SOKT_RECYCLE;
                    (void)TransmitFile(ctx->socket, NULL, 0, 0, &ctx->overlapped, NULL, TF_DISCONNECT | TF_REUSE_SOCKET);
                    continue;
                }

                uint8_t lenByte = ctx->buffer[1] & 0x7F;
                size_t headerSize = 2;
                size_t payloadLen = 0;

                if (lenByte <= 125) {
                    payloadLen = lenByte;
                }
                else if (lenByte == 126) {
                    payloadLen = (static_cast<size_t>(ctx->buffer[2]) << 8) | ctx->buffer[3];
                    headerSize = 4;
                }

                if ((ctx->buffer[1] & 0x80) != 0) { // Check if masked
                    uint32_t maskKey;
                    std::memcpy(&maskKey, &ctx->buffer[headerSize], 4);
                    headerSize += 4;
                    avx2_apply_mask(&ctx->buffer[headerSize], payloadLen, maskKey);
                }

                ProcessEaglercraftPacket(ctx, &ctx->buffer[headerSize], payloadLen);
            }

            unsigned long flags = 0;
            memset(&ctx->overlapped, 0, sizeof(OVERLAPPED));
            (void)WSARecv(ctx->socket, &ctx->wsaBuf, 1, 0, &flags, &ctx->overlapped, 0);
        }
    }

    for (auto ctx : pool) {
        if (ctx->socket != INVALID_SOCKET) closesocket(ctx->socket);
        if (ctx->buffer) _aligned_free(ctx->buffer);
        delete ctx;
    }
    CloseHandle(hIOCP);
    WSACleanup();
    return 0;
}

static std::string ProcessWebSocketHandshake(const std::basic_string_view<char> requestData) {
    std::basic_string_view<char> searchKey = "Sec-WebSocket-Key: ";
    unsigned long long pos = requestData.find(searchKey);
    if (pos == std::string_view::npos) return "HTTP/1.1 400 Bad Request\r\n\r\n";

    size_t start = pos + searchKey.length();
    size_t end = requestData.find("\r\n", start);
    std::string_view clientKey = requestData.substr(start, end - start);

    std::string combined = std::string(clientKey) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    void* hAlg = 0;
    void* hHash = 0;
    unsigned long cbHash = 20, cbHashObject = 0, cbData = 0;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, L"SHA1", NULL, 0))) return "HTTP/1.1 500 Internal Error\r\n\r\n";
    if (!BCRYPT_SUCCESS(BCryptGetProperty(hAlg, L"ObjectLength", reinterpret_cast<unsigned char*>(&cbHashObject), sizeof(unsigned long), &cbData, 0))) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "HTTP/1.1 500 Internal Error\r\n\r\n";
    }

    // OPTIMIZATION: Avoid vectors/heap allocations completely on connection loop. Use stack arrays.
    std::vector<unsigned char> hashObject(cbHashObject); // Dynamic hash object state container
    std::array<unsigned char, 20> hash;                 // Fixed size array for SHA1 (20 bytes)

    if (!BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, hashObject.data(), cbHashObject, NULL, 0, 0)) ||
        !BCRYPT_SUCCESS(BCryptHashData(hHash, (unsigned char*)combined.c_str(), static_cast<unsigned long>(combined.length()), 0)) ||
        !BCRYPT_SUCCESS(BCryptFinishHash(hHash, hash.data(), cbHash, 0))) {
        if (hHash) BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "HTTP/1.1 500 Internal Error\r\n\r\n";
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    unsigned long outLen = 0;
    CryptBinaryToStringA(hash.data(), (unsigned long)hash.size(), 1UL | WEBSOCKET_NOCRLF, NULL, &outLen);
    std::string base64Key(outLen, '\0');
    CryptBinaryToStringA(hash.data(), (unsigned long)hash.size(), WEBSOCKET_BASE64 | WEBSOCKET_NOCRLF, &base64Key[0], &outLen);

    while (!base64Key.empty() && (base64Key.back() == '\0' || base64Key.back() == '\n' || base64Key.back() == '\r')) {
        base64Key.pop_back();
    }
    currentWindow.message = L"Handling transaction (HTTP request to WSS)\n";
    WriteConsoleW(currentWindow.hOut, currentWindow.message.c_str(), (unsigned long)currentWindow.message.size(), &currentWindow.written, NULL);

    return std::format(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: {}\r\n"
        "Sec-WebSocket-Protocol: eaglercraft\r\n\r\n",
        base64Key
    );
}

static void avx2_apply_mask(uint8_t* data, size_t len, uint32_t mask) {
    size_t i = 0;

    // Create vector mask matching the continuous sequence pattern: [B0, B1, B2, B3, B0, B1, B2, B3...]
    __m256i v_mask = _mm256_set1_epi32(static_cast<int>(mask));

    // OPTIMIZATION: Changed streaming stores (_mm256_stream_si256) to standard storeu.
    // This allows data to sit directly inside the CPU's L1/L2 cache since it is immediately read by ProcessEaglercraftPacket.
    for (; i + 64 <= len; i += 64) {
        _mm_prefetch(reinterpret_cast<const char*>(&data[i + 128]), _MM_HINT_T0);

        __m256i v_data1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&data[i]));
        __m256i v_data2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&data[i + 32]));

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(&data[i]), _mm256_xor_si256(v_data1, v_mask));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(&data[i + 32]), _mm256_xor_si256(v_data2, v_mask));
    }

    if (i + 32 <= len) {
        __m256i v_data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&data[i]));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(&data[i]), _mm256_xor_si256(v_data, v_mask));
        i += 32;
    }

    _mm256_zeroupper(); // Avoid AVX-SSE transition penalties

    // Scalor cleanup loop handles remainders perfectly 
    uint8_t* m = reinterpret_cast<uint8_t*>(&mask);
    for (; i < len; ++i) {
        data[i] ^= m[i % 4];
    }
}