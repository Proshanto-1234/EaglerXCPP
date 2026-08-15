# Eaglercraft 1.12.2 Update 3 server based on C++
A low-latency Minecraft 1.12.2 (Protocol Eaglercraft, WebSocket Connection) server backend written in native C++ for x64-86 bit Windows systems. This server uses unusual surprisingly fast optimizations to reduce latency and uses smarter preferences than usual Java server programs

## Key Architectural Preferences
* **Registered Input/Output and Network Acceleration**: Direct zero-copy network buffers using Windows RIO and IOCP rings for sub-millisecond packet processing.
* **AVX (256-bit) for Terrain Generation and Packet Processing**: 256-bit vectorized 2D Perlin noise engine (`AVX2NoiseEngine`) sampling 4 double-precision coordinates simultaneously via hardware intrinsics (`_mm256_fmadd_pd`, `_mm256_floor_pd`) and AVX2 instructions for processing 4-byte signed WebSocket packets.
* **Hardware Entropy:** Direct hardware-level random number generation utilizing the x86 `_rdrand32_step` instruction
* **Zero-Allocation Thread Arenas:** Custom 32-byte aligned per-thread memory pools (`ThreadArena`) avoiding standard heap allocation overhead (`malloc`/`free`) on tick loops.
