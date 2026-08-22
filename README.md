# Eaglercraft 1.12.2 Update 3 server based on C++
A low-latency Minecraft 1.12.2 (Protocol Eaglercraft, WebSocket Connection) server backend written in native C++ for x64-86 bit Windows systems. This server uses unusual surprisingly fast optimizations to reduce latency and uses smarter preferences than usual Java server programs. Unfortunately most of this program is written by AI, at least for the repository owner's part, but that does not matter. Additionally this program has zero third-party library dependencies.

## Key Architectural Preferences
* **Registered Input/Output and Network Acceleration**: Direct zero-copy network buffers using Windows RIO and IOCP rings for sub-millisecond packet processing.
* **AVX (256-bit) for Terrain Generation and Packet Processing**: 256-bit vectorized 2D Perlin noise engine (`AVX2NoiseEngine`) sampling 4 double-precision coordinates simultaneously via hardware intrinsics (`_mm256_fmadd_pd`, `_mm256_floor_pd`) and AVX2 instructions for processing 4-byte signed WebSocket packets.
* **Hardware Entropy:** Direct hardware-level random number generation utilizing the x86 `_rdrand32_step` instruction
* **Zero-Allocation Thread Arenas:** Custom 32-byte aligned per-thread memory pools (`ThreadArena`) avoiding standard heap allocation overhead (`malloc`/`free`) on tick loops.
## Build targets and requirements AND Project Settings
* **Platform**: Windows
* **Compiler type**: MSVC latest 64-bit
* **Build Options**: Multi-Threaded (`/MT`), Console Subsystem, Release configuration binary. Optimization level will be 'Favour Speed'.
* **Runtime Target**: Windows 10 and forwards, x64-bit
* **Language standard**: C++ 20 language.
* **MSVC Version**: MSVC v143 or Visual Studio 2026 and forwards.

## How to build the project
There is already a VCXPROJ file in this repository which one can download alongside the main program files. In order to build this project in Visual studio, follow the steps below:
* **Open the downloaded VCXPROJ file by right-clicking**
* **Set configuration to Release at the top ribbon (if not already set)**
* **Ensure the project files are in the same directory and Enhanced Instruction Set is configured to AVX2**
* **Press `F5` and wait**
* **Locate the project executable in the 'bin' directory**
## Note ##
The addition library dependencies have already been configured by in-source `#pragma` directives native to Microsoft Visual C/C++ (MSVC) compiler. Therefore, there is no need to set them again in the project configuration
