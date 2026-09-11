// Additional generic definition file
#include "DEFINITIONS.hpp"

std::atomic<bool> ENGINE = true; // Global engine variable 
unsigned long long listenSock = INVALID_SOCKET;
void* hIOCP = NULL;
inline std::unordered_map<unsigned long long, PLAYER_SESSION> ActiveSessions;
CRITICAL_SECTION SessionLock;
std::vector<GAME_ENTITY> GlobalEntities;
thread_local ThreadArena WorkerArena;
GPUChunkSerializer g_gpuSerializer;
Console currentWindow;
