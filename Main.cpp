/*
* Necessary macros and compiler checks here
*/
#ifndef __cplusplus
#error This program requires a C++ compiler.
#elif (!(defined(_WIN64) || defined(_M_X64)))
#error 64-bit target environment required.
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>    
#include <bcrypt.h>
#include <wincrypt.h>
#include <tbs.h>
#include <processthreadsapi.h>
#include <process.h>

#include <unordered_map>
#include <vector>
#include <format>
#include <span>
#include <array>
#include <cstring>
#include <thread>
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

// ============================================================================
// RIO (REGISTERED I/O) EXTENSION SUBSYSTEM
// ============================================================================
static RIO_EXTENSION_FUNCTION_TABLE g_rio = {};
static RIO_BUFFERID g_rioBufferId = RIO_INVALID_BUFFERID;
static uint8_t* g_rioBufferPool = nullptr;
static RIO_CQ g_rioCQ = RIO_INVALID_CQ;
static bool g_rioEnabled = false;

constexpr DWORD RIO_POOL_SIZE = 64 * 1024 * 1024; // 64 MB Pinned RAM Pool
constexpr DWORD RIO_SLICE_SIZE = 32768;            // 32 KB per socket slice

static bool InitializeRIOSubsystem(SOCKET dummySock) {
	GUID functionTableId = WSAID_MULTIPLE_RIO;
	DWORD dwBytes = 0;

	int result = WSAIoctl(
		dummySock, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,
		&functionTableId, sizeof(GUID),
		&g_rio, sizeof(g_rio),
		&dwBytes, NULL, NULL
	);

	if (result == SOCKET_ERROR) return false;

	// Allocate page-aligned, contiguous RAM pool
	g_rioBufferPool = reinterpret_cast<uint8_t*>(_aligned_malloc(RIO_POOL_SIZE, 4096));
	if (!g_rioBufferPool) return false;

	// Pin RAM memory pages persistently with afd.sys kernel network driver
	g_rioBufferId = g_rio.RIORegisterBuffer(reinterpret_cast<char*>(g_rioBufferPool), RIO_POOL_SIZE);
	if (g_rioBufferId == RIO_INVALID_BUFFERID) return false;

	// Create Ring-3 User-Mode Lock-Free Completion Queue
	g_rioCQ = g_rio.RIOCreateCompletionQueue(10000, NULL);
	if (g_rioCQ == RIO_INVALID_CQ) return false;

	g_rioEnabled = true;
	return true;
}
// ============================================================================

static void* hGenerationThreadSignal = nullptr;
inline unsigned char* GlobalWorldMemory = nullptr;

static std::string ProcessWebSocketHandshake(const std::basic_string_view<char> requestData);
static void avx2_apply_mask(uint8_t* data, size_t len, uint32_t mask);

unsigned long __stdcall WorldGenerationWorkerThread(void* lpParam) {
	NtPinThread(GetCurrentThread(), 2);
	int index = 0;
	for (int cz = 0; cz < 64; ++cz) {
		for (int cx = 0; cx < 64; ++cx) {
			uint8_t* chunkPtr = GlobalWorldMemory + (index * 65536);
			GenerateWorldChunk(cx, cz, chunkPtr);
			index++;
		}
	}
	SetEvent(hGenerationThreadSignal);
	return 0;
}

int main(int argc, char* argv[]) {
	InitializeCriticalSection(&SessionLock);

	if (!currentWindow.Initialize()) return ERROR_INTERNAL;

	if (!CheckHardwareInstructionSupport()) {
		currentWindow.message = L"CRITICAL ERROR: AVX2 / RDRAND unsupported.\n";
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

	if (!LoadNtFunctions()) return ERROR_INTERNAL_NTDLL_FAILURE;

	GlobalWorldMemory = reinterpret_cast<unsigned char*>(_aligned_malloc(64 * 64 * 65536, 32));
	hGenerationThreadSignal = CreateEvent(NULL, TRUE, FALSE, NULL);
	uint32_t threadId = 0;
	HANDLE hGenThread = reinterpret_cast<HANDLE>(_beginthreadex(NULL, 0, (_beginthreadex_proc_type)WorldGenerationWorkerThread, NULL, 0, &threadId));
	if (hGenThread) CloseHandle(hGenThread);

	std::pair<unsigned long, unsigned long> corePairs = DynamicGetLeastUsedCores();
	NtPinThread(GetCurrentThread(), corePairs.first);

	SetConsoleCtrlHandler(ConsoleCtrlHandler, 1);

	WSAData wsaData = {};
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

	hIOCP = CreateIoCompletionPort(((void*)(long long)-1), NULL, 0, 0);
	listenSock = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_REGISTERED_IO);

	// Load RIO Extension pointers dynamically via dummy socket query
	if (InitializeRIOSubsystem(listenSock)) {
		currentWindow.message = L"Registered I/O (RIO) Subsystem Initialized Successfully.\n";
		WriteConsoleW(currentWindow.hOut, currentWindow.message.c_str(), static_cast<unsigned long>(currentWindow.message.size()), &currentWindow.written, NULL);
	}

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
		ctx->socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_REGISTERED_IO);
		if (ctx->socket == INVALID_SOCKET) return ERROR_CRITICAL_MEMORY_FAILURE;

		// Assign pre-pinned buffer slice from the RIO buffer pool
		if (g_rioEnabled && (i * RIO_SLICE_SIZE < RIO_POOL_SIZE)) {
			ctx->buffer = g_rioBufferPool + (i * RIO_SLICE_SIZE);
		} else {
			ctx->buffer = reinterpret_cast<uint8_t*>(_aligned_malloc(32768, 32));
		}

		if (!ctx->buffer) return ERROR_CRITICAL_MEMORY_FAILURE;
		ctx->rxBufferOffset = 0;
		ctx->operation = OP_HANDSHAKE;
		ctx->wsaBuf.buf = (char*)ctx->buffer;
		ctx->wsaBuf.len = 32768;

		CreateIoCompletionPort((void*)ctx->socket, hIOCP, (unsigned long long)ctx, 0);
		memset(&ctx->overlapped, 0, sizeof(_OVERLAPPED));

		(void)lpfnAcceptEx(listenSock, ctx->socket, ctx->buffer, 0,
			sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &dwBytes, &ctx->overlapped);
		pool.push_back(ctx);
	}

	currentWindow.message = std::format(L"Playable C++ Native Engine running on Port {}...\n", targetPort);
	WriteConsoleW(currentWindow.hOut, currentWindow.message.c_str(), static_cast<unsigned long>(currentWindow.message.size()), &currentWindow.written, NULL);

	_OVERLAPPED_ENTRY entries[16] = {};
	RIORESULT rioResults[16] = {};
	unsigned long removed = 0;
	unsigned long long lastCleanupTime = GetTickCount64() / 1000;

	while (ENGINE) {
		// DRAIN RIO LOCK-FREE USER-MODE COMPLETION QUEUE IN RING 3 FIRST
		if (g_rioEnabled) {
			ULONG rioDequeued = g_rio.RIODequeueCompletion(g_rioCQ, rioResults, 16);
			for (ULONG r = 0; r < rioDequeued; ++r) {
				CONNECTION_CONTEXT* rioCtx = reinterpret_cast<CONNECTION_CONTEXT*>(rioResults[r].RequestContext);
				if (rioCtx && rioResults[r].BytesTransferred > 0) {
					rioCtx->rxBufferOffset += rioResults[r].BytesTransferred;
				}
			}
		}

		removed = 0;
		if (!GetQueuedCompletionStatusEx(hIOCP, entries, 16, &removed, 1000, 0)) {
			if (!ENGINE) break;
		}

		unsigned long long now = GetTickCount64() / 1000;
		if (now - lastCleanupTime >= 60) {
			GarbageCollectStrayEntities();
			lastCleanupTime = now;
		}

		for (unsigned long i = 0; i < removed; ++i) {
			unsigned long bytes = entries[i].dwNumberOfBytesTransferred;
			CONNECTION_CONTEXT* ctx = (CONNECTION_CONTEXT*)entries[i].lpOverlapped;

			if (bytes == 0) {
				if (ctx->operation != OP_SOKT_RECYCLE) {
					EnterCriticalSection(&SessionLock);
					ActiveSessions.erase(ctx->socket);
					LeaveCriticalSection(&SessionLock);

					ctx->operation = OP_SOKT_RECYCLE;
					ctx->rxBufferOffset = 0;
					std::memset(&ctx->overlapped, 0, sizeof(_OVERLAPPED));
					(void)TransmitFile(ctx->socket, NULL, 0, 0, &ctx->overlapped, NULL, TF_DISCONNECT | TF_REUSE_SOCKET);
				}
				continue;
			}

			if (ctx->operation == OP_SOKT_RECYCLE) {
				ctx->operation = OP_HANDSHAKE;
				ctx->rxBufferOffset = 0;
				std::memset(&ctx->overlapped, 0, sizeof(_OVERLAPPED));
				ctx->wsaBuf.buf = reinterpret_cast<char*>(ctx->buffer);
				ctx->wsaBuf.len = 32768;
				(void)lpfnAcceptEx(listenSock, ctx->socket, ctx->buffer, 0,
					sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &dwBytes, &ctx->overlapped);
				continue;
			}

			ctx->rxBufferOffset += bytes;

			if (ctx->operation == OP_HANDSHAKE) {
				(void)setsockopt(ctx->socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&listenSock, sizeof(listenSock));

				std::basic_string_view<char> requestStr(reinterpret_cast<char*>(ctx->buffer), ctx->rxBufferOffset);
				if (requestStr.find("GET ") != std::string_view::npos && requestStr.find("\r\n\r\n") != std::string_view::npos) {
					std::string handshakeResponse = ProcessWebSocketHandshake(requestStr);
					(void)send(ctx->socket, handshakeResponse.c_str(), static_cast<int>(handshakeResponse.size()), 0);

					EnterCriticalSection(&SessionLock);
					ActiveSessions[ctx->socket] = PLAYER_SESSION();
					LeaveCriticalSection(&SessionLock);

					ctx->operation = OP_READ;
					ctx->rxBufferOffset = 0;
				}
			}
			else if (ctx->operation == OP_READ) {
				size_t readPos = 0;
				while (readPos < ctx->rxBufferOffset) {
					if (ctx->rxBufferOffset - readPos < 2) break;

					uint8_t* ptr = &ctx->buffer[readPos];
					uint8_t opcode = ptr[0] & 0x0F;

					if (opcode == 0x08) { // WS Close
						EnterCriticalSection(&SessionLock);
						ActiveSessions.erase(ctx->socket);
						LeaveCriticalSection(&SessionLock);

						ctx->operation = OP_SOKT_RECYCLE;
						ctx->rxBufferOffset = 0;
						(void)TransmitFile(ctx->socket, NULL, 0, 0, &ctx->overlapped, NULL, TF_DISCONNECT | TF_REUSE_SOCKET);
						break;
					}

					uint8_t lenByte = ptr[1] & 0x7F;
					size_t headerSize = 2;
					size_t payloadLen = 0;

					if (lenByte <= 125) {
						payloadLen = lenByte;
					}
					else if (lenByte == 126) {
						if (ctx->rxBufferOffset - readPos < 4) break;
						payloadLen = (static_cast<size_t>(ptr[2]) << 8) | ptr[3];
						headerSize = 4;
					}

					bool isMasked = (ptr[1] & 0x80) != 0;
					if (isMasked) headerSize += 4;

					if (ctx->rxBufferOffset - readPos < headerSize + payloadLen) break;

					if (isMasked) {
						uint32_t maskKey;
						std::memcpy(&maskKey, &ptr[headerSize - 4], 4);
						avx2_apply_mask(&ptr[headerSize], payloadLen, maskKey);
					}

					ProcessEaglercraftPacket(ctx, &ptr[headerSize], payloadLen);
					readPos += headerSize + payloadLen;
				}

				if (readPos > 0) {
					size_t remaining = ctx->rxBufferOffset - readPos;
					if (remaining > 0) {
						std::memmove(ctx->buffer, &ctx->buffer[readPos], remaining);
					}
					ctx->rxBufferOffset = remaining;
				}
			}

			unsigned long flags = 0;
			memset(&ctx->overlapped, 0, sizeof(OVERLAPPED));
			ctx->wsaBuf.buf = reinterpret_cast<char*>(ctx->buffer + ctx->rxBufferOffset);
			ctx->wsaBuf.len = static_cast<unsigned long>(32768 - ctx->rxBufferOffset);
			(void)WSARecv(ctx->socket, &ctx->wsaBuf, 1, 0, &flags, &ctx->overlapped, 0);
		}
	}

	for (auto ctx : pool) {
		if (ctx->socket != INVALID_SOCKET) closesocket(ctx->socket);
		// If buffer was allocated outside g_rioBufferPool, free it safely
		if (ctx->buffer && (!g_rioEnabled || (ctx->buffer < g_rioBufferPool || ctx->buffer >= g_rioBufferPool + RIO_POOL_SIZE))) {
			_aligned_free(ctx->buffer);
		}
		delete ctx;
	}

	if (g_rioEnabled && g_rioBufferPool) {
		g_rio.RIODeregisterBuffer(g_rioBufferId);
		_aligned_free(g_rioBufferPool);
	}

	if (GlobalWorldMemory) _aligned_free(GlobalWorldMemory);
	if (hGenerationThreadSignal) CloseHandle(hGenerationThreadSignal);

	DeleteCriticalSection(&SessionLock);
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

	std::vector<unsigned char> hashObject(cbHashObject);
	std::array<unsigned char, 20> hash;

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
	__m256i v_mask = _mm256_set1_epi32(static_cast<int>(mask));

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

	_mm256_zeroupper();

	uint8_t* m = reinterpret_cast<uint8_t*>(&mask);
	for (; i < len; ++i) {
		data[i] ^= m[i % 4];
	}
}
