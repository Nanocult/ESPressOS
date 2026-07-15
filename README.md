# ESPressOS

> A Lightweight, Dynamically-Loading Embedded Operating System for ESP32-S3 ecosystem**.

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Project Structure](#project-structure)
- [Component Documentation](#component-documentation)
- [Hardware Requirements](#hardware-requirements)
- [Quick Start](#quick-start)
- [Building the Kernel](#building-the-kernel)
- [Building Apps](#building-apps)
- [Deploying to Device](#deploying-to-device)
- [ABI Versioning](#abi-versioning)
- [Security Model](#security-model)
- [Power Consumption](#power-consumption)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

---

## Overview

ESPressOS is a custom embedded operating system designed for the **ESP32-S3** microcontroller. ESPressOS enables **dynamic application loading/unloading** from an SD card at runtime, similar to a smartphone OS, while operating within the severe memory constraints of an embedded platform (8MB PSRAM, 512KB SRAM). ESPressOS transforms the ESP32-S3 microcontroller into a self-updating, crash-resistant smart assistant platform. 

### ✨ Key Features

| Category | Feature | Details |
|----------|---------|---------|
| **Dynamic Loading** | App Hot-Swap | Load/unload `.espapp` binaries from SD card at runtime |
| | Position-Independent Code | Custom flat binary format with single-pass relocation |
| | App Manifest System | JSON metadata for versioning, icons, permissions |
| **Memory Safety** | Zero-Fragmentation Allocator | 4KB block pool with bitmap tracking in SRAM |
| | Ownership Tagging | Per-app memory tracking with automatic leak reclamation |
| | Crash Isolation | Apps run in sandboxed tasks with WDT monitoring |
| **Hardware Services** | Audio Engine | I2S TDM with DMA ringbuffers, mic multiplexer |
| | Display Manager | LVGL v9 with sandboxed object lifecycle |
| | Storage Manager | SDMMC hot-swap detection, auto-provisioning |
| | Input Manager | Debounced buttons, touch, encoder with gesture system |
| **Resilience** | Crash Recovery | Dual-layer (RAM + NVS) with Safe Mode UI |
| | OTA Updates | Three-stage atomic pipeline with ED25519 verification |
| | Storage Provisioning | Auto-download bootstrap apps on fresh SD card |
| | Cryptographically signed | App downloads with atomic filesystem operations |
| **Power** | VAD-Gated Light Sleep | ~15mA idle with always-on wake word |
| | App Power Locks | Apps declare power requirements to prevent premature sleep |
| | Deep Sleep | <100µA hibernation after extended idle |
| | 5-state power ladder | From Active (130mA) to Deep Sleep (<100µA) |
| **AI** | Wake Word | "Hi ESP" detection via ESP-SR WakeNet |
| | VAD-gated Wake Word | Running in Light Sleep (~15mA) |
| | Voice Assistant | Opus-encoded WebSocket streaming to server |

#### Additional Capabilities

| Feature | Description |
|:--------|:------------|
| **Zero-Fragmentation Memory** | Custom 4KB block pool allocator with ownership tagging and automatic leak reclamation |
| **Persistent Services** | Clock/NTP, Audio Engine, Display Manager run permanently, survive app transitions |
| **Crash Isolation** | Apps run in sandboxed tasks; faults are caught, logged, and recovered without rebooting |
| **Self-Healing** | Dual-layer crash recovery (Soft + Hard), Safe Mode UI, automatic re-provisioning |
| **Hot-Swap Storage** | SD card insertion/ejection detected at hardware level with safe unmount |

### Core Applications (Shipped)

- **Clock/Calendar** — NTP-synced digital clock with calendar widget
- **App Launcher** — Dynamic grid UI discovering installed `.espapp` files
- **Audio Player** — Universal media player (WAV PCM + MP3 via libhelix)
- **Voice Assistant** — Push-to-talk / Wake-word AI assistant (Opus + WebSocket)
- **Settings & OTA** — Wireless app update manager with signature verification

---

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        APPLICATION LAYER                            │
│  ┌─────────┐ ┌──────────┐ ┌────────────┐ ┌─────────┐ ┌───────────┐  │
│  │  Clock  │ │ Launcher │ │Audio Player│ │  Voice  │ │    OTA    │  │
│  │  .espapp│ │ .espapp  │ │  .espapp   │ │ .espapp │ │  .espapp  │  │
│  └────┬────┘ └────┬─────┘ └──────┬─────┘ └────┬────┘ └─────┬─────┘  │
│       │           │              │            │            │        │
├───────┴───────────┴──────────────┴────────────┴────────────┴────────┤
│                  KERNEL API (Function Pointer Table)                │
│              kernel_api.h — ABI v1.6 — Stable Contract              │
├─────────────────────────────────────────────────────────────────────┤
│                          KERNEL CORE                                │
│  ┌───────────── PROTECTED KERNEL (SRAM + 1MB PSRAM) ──────────────┐ │
│  |                   PERSISTENT SERVICES                          | │
│  │   ┌───────────┐ ┌───────────┐ ┌──────────┐ ┌───────────────┐   │ │
│  │   │   Clock   │ │  Audio    │ │ Display  │ │   Storage     │   │ │
│  │   │   / NTP   │ │  Engine   │ │ Manager  │ │   Manager     │   │ │
│  │   │  Service  │ │ (I2S+DMA) │ │  (LVGL)  │ │  (SD Hot-Swap)│   │ │
│  │   └───────────┘ └───────────┘ └──────────┘ └───────────────┘   │ │
│  │   ┌───────────┐ ┌───────────┐ ┌──────────┐ ┌───────────────┐   │ │
│  │   │   Input   │ │   Power   │ │  Wake    │ │   Crash       │   │ │
│  │   │  Manager  │ │  Manager  │ │  Word    │ │   Recovery    │   │ │
│  │   │ (Btn/Touch│ │ (Sleep/   │ │ (ESP-SR) │ │  (NVS+Safe    │   │ │
│  │   │  /Encoder)│ │  VAD Gate)│ │          │ │   Mode)       │   │ │
│  │   └───────────┘ └───────────┘ └──────────┘ └───────────────┘   │ │
│  │   ┌────────────────────────────────────────────────────────┐   │ │
│  │   │  App Lifecycle Manager   │  PSRAM Block Pool Allocator │   │ │
│  │   │  (Load/Run/Unload)       │  (Zero-Frag, Owner-Tagged)  │   │ │
│  │   │  App Sandbox (WDT+Crash) │  Memory Leak Detector       │   │ │
│  │   └────────────────────────────────────────────────────────┘   │ │
│  └────────────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────────────┤
│  ┌──────────────── APP REGION (7MB Octal PSRAM) ─────────────────┐  │
│  │  Active App Code + Data + BSS + App Heap Pool                 │  │
│  └───────────────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────────────┤
│                    HARDWARE ABSTRACTION                             │
│  ┌──────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌──────────────┐  │
│  │ESP32-S3  │ │ Octal   │ │ I2S TDM │ │ SPI/    │ │SDMMC 4-bit   │  │
│  │LX7 Dual  │ │ PSRAM   │ │ (Mic+   │ │ Parallel│ │(SD Card)     │  │
│  │Core 240M │ │ 8MB     │ │ Speaker)│ │ Display │ │              │  │
│  └──────────┘ └─────────┘ └─────────┘ └─────────┘ └──────────────┘  │
│  ┌─────────────────┐  ┌─────────────────────┐                       │
│  | ST7789 240×320  │  | INMP441 + MAX98357A │                       │
│  |   4× Buttons    │  |  Touch Controller   │                       │
│  └─────────────────┘  └─────────────────────┘                       │
└─────────────────────────────────────────────────────────────────────┘
```

### Memory Map

```
Internal SRAM (512KB)
├── Kernel Code + Data .............. ~128KB
├── FreeRTOS Stacks + Queues ........ ~64KB
├── WiFi/BT Stack ................... ~80KB
├── Audio DMA Buffers ............... ~16KB
├── PSRAM Bitmap (224 bytes) ........ <1KB
├── Input/Power State ............... ~4KB
└── Reserved / Stack Overflow ....... ~32KB

Octal PSRAM (8MB)
├── Kernel Reserved (1MB)
│   ├── LVGL Double Buffer .......... ~30KB
│   ├── Audio Ringbuffers ........... ~64KB
│   └── WakeNet AFE State .......... ~200KB
├── App Code Region (2MB max)
│   └── [Currently loaded .espapp]
├── App Heap Pool (5MB)
│   └── [4KB blocks, bitmap-tracked]
└── Reserved / Fragmentation ........ ~1MB

SD Card (SDMMC 4-bit, FAT32)
├── /apps/         ← .espapp binaries + .manifest.json
├── /data/         ← Per-app persistent data
├── /media/        ← Audio files (WAV, MP3)
├── /cache/        ← OTA download staging
└── /crash_reports/← Forensic logs
```

---

## 📁 Project Structure

```
ESPressOS/
├── README.md                          ← You are here
├── LICENSE
├── sdkconfig.defaults                 ← ESP-IDF build configuration
├── partitions.csv                     ← Custom partition table
├── CMakeLists.txt                     ← Top-level kernel build
│
├── kernel/                            ← KERNEL SOURCE (Flashed to ESP32-S3)
│   ├── main.c                         ← Entry point, service init, lifecycle loop
│   ├── kernel_api.c                   ← KernelAPI table population
│   ├── kernel_internal.h              ← Internal kernel-only declarations
│   │
│   ├── loader/
│   │   ├── app_loader.h               ← Load/Run/Unload public API
│   │   ├── app_loader.c               ← ELF→PSRAM, relocation engine
│   │   ├── app_sandbox.h              ← Crash isolation wrapper
│   │   └── app_sandbox.c              ← WDT, panic hook, thread join
│   │
│   ├── memory/
│   │   ├── psram_pool.h               ← Block pool allocator API
│   │   └── psram_pool.c               ← Bitmap, alloc, free, leak detect
│   │
│   ├── services/
│   │   ├── svc_clock.h / .c           ← NTP sync, RTC, tick counter
│   │   ├── svc_audio.h / .c           ← I2S DMA, ringbuffers, mic mux
│   │   ├── svc_display.h / .c         ← LVGL host, object sandboxing
│   │   ├── svc_storage.h / .c         ← SD hot-swap, mount, provisioning
│   │   ├── svc_input.h / .c           ← Buttons, touch, encoder, gestures
│   │   ├── svc_power.h / .c           ← Sleep states, locks, idle timers
│   │   ├── svc_wake.h / .c            ← WakeNet, AFE, VAD-gated sleep
│   │   ├── svc_recovery.h / .c        ← NVS crash tracking, safe mode
│   │   ├── svc_fs.c                   ← Sandboxed filesystem wrappers
│   │   └── svc_net.c                  ← HTTP/HTTPS/WebSocket wrappers
│   │
│   └── lifecycle/
│       ├── lifecycle_manager.c        ← App state machine, launch queue
│       └── kernel_lifecycle_loop.c    ← Main run loop
│
├── include/                           ← SHARED HEADERS (Kernel + Apps)
│   ├── kernel_api.h                   ← THE ABI CONTRACT (v1.6)
│   ├── espapp_format.h                ← Binary format structs (symlink to kernel/)
│   └── opus_wrapper.h                 ← Opus codec abstraction
│
├── tools/                             ← BUILD TOOLCHAIN
│   ├── build_espapp.py                ← ELF → signed .espapp converter
│   ├── espapp.ld                      ← App linker script (PIC, Xtensa LX7)
│   ├── EspAppBuild.cmake              ← CMake module for app projects
│   └── gen_api_table.py               ← Generates flat API index table
│
├── keys/                              ← CRYPTOGRAPHIC KEYS (NEVER COMMIT)
│   ├── app_signing_key.pem            ← ED25519 private key (build server)
│   └── app_public_key.pem             ← ED25519 public key (embedded in kernel)
│
├── apps/                              ← APPLICATION SOURCE CODE
│   ├── clock/
│   │   ├── CMakeLists.txt
│   │   ├── app_clock.c
│   │   └── clock.manifest.json
│   ├── launcher/
│   │   ├── CMakeLists.txt
│   │   ├── app_launcher.c
│   │   └── launcher.manifest.json
│   ├── audio/
│   │   ├── CMakeLists.txt
│   │   ├── app_audio.c
│   │   ├── helix_wrapper.h / .c
│   │   └── audio.manifest.json
│   ├── voice/
│   │   ├── CMakeLists.txt
│   │   ├── app_voice.c
│   │   ├── opus_wrapper.h / .c
│   │   └── voice.manifest.json
│   └── settings/
│       ├── CMakeLists.txt
│       ├── app_settings.c
│       └── settings.manifest.json
│
├── lib/                               ← THIRD-PARTY LIBRARIES
│   ├── helix-mp3/                     ← Fixed-point MP3 decoder
│   ├── libopus/                       ← Opus codec (PIC-compiled)
│   └── lvgl/                          ← LVGL v9 (display framework)
│
├── assets/                            ← EMBEDDED KERNEL ASSETS
│   ├── chime.wav                      ← Wake word confirmation sound
│   ├── media/                         ← Media files
│   └── fonts/                         ← LVGL font binaries
│
├── docs/                              ← DETAILED COMPONENT DOCUMENTATION
│   ├── architecture.md                ← Detailed OS architecture
│   ├── kernel-api.md                  ← Kernel ABI reference
│   ├── app-loader.md                  ← App loader
│   ├── audio-engine.md                ← Audio pipeline documentation
│   ├── display-manager.md             ← LVGL display service
│   ├── storage-manager.md             ← SD card hot-swap & provisioning
│   ├── input-manager.md               ← Button/touch/encoder handling
│   ├── power-management.md            ← Light sleep & VAD gating
│   ├── memory-management.md           ← PSRAM pool & leak detection
│   ├── wake-word.md                   ← ESP-SR wake word integration
│   ├── crash-recovery.md              ← Soft/hard crash protection
│   ├── ota-updates.md                 ← OTA app update pipeline
│   ├── app-development-guide.md       ← Guide for writing .espapp apps
│   ├── espapp-binary-format.md        ← .espapp binary specification
│   └── server-api-contract.md         ← Server API contract
│
└── tests/                             ← INTEGRATION & STRESS TESTS
    ├── test_loader.c
    ├── test_psram_pool.c
    ├── test_crash_recovery.c
    ├── test_storage_hotswap.c
    └── test_power_states.c
```

---

## Component Documentation

Each subsystem has dedicated documentation covering architecture, API reference, configuration, and integration notes.

| Component | Document | Description |
|:----------|:---------|:------------|
| **System Architecture** | [docs/architecture.md](docs/architecture.md) | Full system design, memory map, task priorities, data flow |
| **Kernel API (ABI)** | [docs/kernel-api.md](docs/kernel-api.md) | Complete function pointer table, versioning rules, app contract |
| **App Loader & Relocator** | [docs/app-loader.md](docs/app-loader.md) | `.espapp` loading, Xtensa relocation, signature verification |
| **PSRAM Block Pool** | [docs/psram-pool.md](docs/psram-pool.md) | Allocator design, ownership tagging, leak detection, force-reclaim |
| **Audio Engine** | [docs/audio-engine.md](docs/audio-engine.md) | I2S TDM, DMA ringbuffers, mic multiplexer, format negotiation |
| **Display Manager** | [docs/display-manager.md](docs/display-manager.md) | LVGL integration, object sandboxing, property API, double buffering |
| **Storage Manager** | [docs/storage-manager.md](docs/storage-manager.md) | SDMMC hot-swap, CD pin monitoring, auto-provisioning, FS locking |
| **Input Manager** | [docs/input-manager.md](docs/input-manager.md) | Button debounce, touch, encoder, system gestures, subscriptions |
| **Power Management** | [docs/power-management.md](docs/power-management.md) | 5-state power ladder, app locks, VAD-gated Light Sleep, Deep Sleep |
| **Wake Word Service** | [docs/wake-word.md](docs/wake-word.md) | ESP-SR AFE, WakeNet model, VAD gating, auto-launch pipeline |
| **Crash Recovery** | [docs/crash-recovery.md](docs/crash-recovery.md) | Soft/Hard crash layers, NVS tracking, Safe Mode, blacklisting |
| **OTA Updates** | [docs/ota-updates.md](docs/ota-updates.md) | Server manifest, streaming download, atomic rename, ED25519 verify |
| **App Development Guide** | [docs/app-development-guide.md](docs/app-development-guide.md) | How to create, build, sign, and deploy a new `.espapp` |
| **Binary Format Spec** | [docs/espapp-binary-format.md](docs/espapp-binary-format.md) | Header layout, relocation table, signature block, alignment rules |
| **Server API Contract** | [docs/server-api-contract.md](docs/server-api-contract.md) | OTA endpoints, Voice WebSocket protocol, provisioning manifest |

---

## 🔧 Hardware Requirements

### Primary Configuration (Recommended)

| Component | Model | Interface | GPIO | Purpose |
|-----------|-------|-----------|------|---------|
| **MCU Module** | ESP32-S3-WROOM-1-N16R8 | | | 16MB Flash + 8MB Octal PSRAM |
| Display | ST7789 2.8" 240×320 | SPI | MOSI=11, CLK=12, DC=13, CS=14, RST=15 | Primary UI output |
| Microphone | INMP441 (PDM) | I2S TDM | SD=7, WS=5, SCK=4 | Voice capture + Wake Word |
| Speaker DAC | MAX98357A | I2S TDM | DIN=6, BCLK=4, LRC=5 | Audio playback |
| SD Card | MicroSD breakout | SDMMC 4-bit | CMD=38, CLK=39, D0-D3=40-43 | App storage + user data |
| SD Detect | Mechanical switch | GPIO | CD=13 | |
| Buttons | 4× tactile | GPIO (active low) | 0, 1, 2, 3 | Back, Up, Down, Power |
| Touch | CST816S / FT6236 | I2C | SDA=9, SCL=8 | Touch input (optional) |
| Power | 3.7V LiPo + TP4056 or USB-C 5V | | | Power supply |

### GPIO Assignment (Default)

```
GPIO  0: BTN_BACK        GPIO  4: I2S_BCLK
GPIO  1: BTN_UP          GPIO  5: I2S_WS (LRCLK)
GPIO  2: BTN_DOWN        GPIO  6: I2S_DOUT (→ MAX98357A)
GPIO  3: BTN_POWER       GPIO  7: I2S_DIN  (← INMP441)
GPIO  8: TOUCH_SCL       GPIO 10: ENCODER_A
GPIO  9: TOUCH_SDA       GPIO 11: ENCODER_B
GPIO 13: SD_CD (Detect)  GPIO 14-17: SDMMC D0-D3
GPIO 18: SDMMC_CLK       GPIO 19: SDMMC_CMD
GPIO 20: SPI_CS (Display) GPIO 21: SPI_SCK
GPIO 22: SPI_MOSI        GPIO 38: SPI_DC (Display)
```

> ⚠️ Adjust GPIO assignments in `sdkconfig.defaults` and service headers to match your PCB layout.

## Power Consumption

| State | Display | CPU | Current | Trigger |
|:------|:--------|:----|:--------|:--------|
| Active | ON 100% | Full speed | ~120mA | User interaction |
| Dimmed | ON 20% | Full speed | ~65mA | 30s idle |
| Display Off | OFF | Full speed | ~45mA | 45s idle / Audio playing |
| Light Sleep | OFF | VAD-gated | **~15mA** | 60s idle / Wake Word active |
| Deep Sleep | OFF | Halted | **<100µA** | 5min idle / Manual |

Wake sources from Light Sleep: Timer (VAD cycle), Touch, Button, I2S DMA.
Wake sources from Deep Sleep: GPIO button only (full reboot).

See [docs/power-management.md](docs/power-management.md) for implementation details.

---

## 🚀 Quick Start

### Prerequisites

- **ESP-IDF v5.3+** installed and sourced (`source $IDF_PATH/export.sh`)
- **Python 3.8+** with `cryptography` package (`pip install cryptography`) (for `build_espapp.py`)
- **Xtensa ESP32-S3 toolchain** (included with ESP-IDF)
- **OpenSSL** (for ED25519 key generation)
- **Hardware:** ESP32-S3-N16R8 devkit + peripherals (see above)

### 1. Clone & Configure

```bash
git clone https://github.com/nanocult/ESPressOS.git
cd ESPressOS
cp sdkconfig.defaults sdkconfig
idf.py set-target esp32s3
idf.py menuconfig   # Verify PSRAM=Octal, Flash=16MB
```

### 2. Generate Signing Keys (One-Time)

```bash
mkdir -p keys
openssl genpkey -algorithm ed25519 -out keys/app_signing_key.pem
openssl pkey -in keys/app_signing_key.pem -pubout -out keys/app_public_key.pem
```

### 3. Build & Flash Kernel

```bash
idf.py build flash monitor -p /dev/ttyUSB0
```

### 4. Build Apps

```bash
cd apps/clock && mkdir build && cd build
cmake .. && make build_espapp
# Output: build/clock.espapp (~15KB)
```

### 5. Deploy Apps to SD Card

```bash
# Format SD card as FAT32 (16KB allocation unit)
# Create directory structure:
mkdir -p /mnt/sd/{apps,data,media,cache,crash_reports}

# Copy built apps:
cp apps/clock/build/clock.espapp /mnt/sd/apps/
cp apps/clock/clock.manifest.json /mnt/sd/apps/
cp apps/launcher/build/launcher.espapp /mnt/sd/apps/
cp apps/launcher/launcher.manifest.json /mnt/sd/apps/
# ... repeat for audio, voice, settings
```

### 6. Boot

Insert SD card → Power on → Kernel mounts SD → Loads Launcher → Clock ticks.
The kernel will mount the SD card, discover apps via manifests, and launch the Launcher grid. Tap "Clock" to load the clock app.

---

## Building the Kernel

The kernel is a standard ESP-IDF project:

```bash
cd esp-appos/
idf.py build          # Compile kernel firmware
idf.py flash          # Flash via USB
idf.py monitor        # Serial console (115200 baud)
```

### Critical `sdkconfig` Settings

```ini
CONFIG_ESP32S3_SPIRAM_SUPPORT=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_PM_ENABLE=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_MAX_LFN=64
CONFIG_SDMMC_FREQ_HIGHSPEED=y
CONFIG_LVGL_FONT_MONTSERRAT_14=y
CONFIG_LVGL_FONT_MONTSERRAT_24=y
CONFIG_LVGL_FONT_MONTSERRAT_48=y
```

---

## Building Apps

Each app is a standalone CMake project that produces a signed `.espapp` binary.

```bash
cd apps/<app_name>
mkdir build && cd build
cmake ..
make build_espapp
```

The build pipeline:
1. Compile with `-fPIC -mlongcalls -mtext-section-literals -nostdlib`
2. Link with `espapp.ld` (custom linker script)
3. Run `build_espapp.py` → Extract sections → Generate relocations → Sign ED25519
4. Output: `<app_name>.espapp` (typically 10-150KB)

See [docs/app-development-guide.md](docs/app-development-guide.md) for the full walkthrough.

---

## Deploying to Device

### Manual (SD Card)
Copy `.espapp` + `.manifest.json` to `/apps/` on the SD card.

### OTA (Wireless)
1. Upload new `.espapp` to your OTA server
2. Update `manifest.json` with new `build` number
3. On device: Open Settings app → "Check for Updates"
4. App downloads → Verifies ED25519 → Atomic rename → Ready

### Auto-Provisioning (Fresh Card)
Insert blank SD card → Kernel detects unformatted → Prompts user → Formats → Downloads bootstrap apps from server → Boots into Launcher.

---

## ABI Versioning

The Kernel API follows strict semantic versioning:

| Version | Meaning |
|:--------|:--------|
| **MAJOR** bump | Breaking change (struct reorder, function removal). All apps must rebuild. |
| **MINOR** bump | Backward-compatible addition (new function appended). Old apps still work. |

Current: **v1.6**

**Rules:**
- Never remove or reorder existing fields in `KernelAPI` or sub-API structs
- New functions are appended ONLY at the end of their respective struct
- Apps check `api->abi_version` at startup and refuse to run on incompatible kernels
- The flat function pointer table (`g_api_fn_table[]`) index IS the ABI for relocations

See [docs/kernel-api.md](docs/kernel-api.md) for the complete function index table.

---

## 🔐 Security Model

```
┌─────────────────────────────────────────────────┐
│ Build Server                                    │
│  Signs .espapp with ED25519 Private Key         │
└────────────────────┬────────────────────────────┘
                     │ HTTPS (TLS 1.3)
                     ▼
┌─────────────────────────────────────────────────┐
│ OTA Server                                      │
│  Serves signed binaries + manifest.json         │
└────────────────────┬────────────────────────────┘
                     │ Download to /cache/*.tmp
                     ▼
┌─────────────────────────────────────────────────┐
│ ESP32-S3 Kernel                                 │
│  1. Stream SHA-256 hash                         │
│  2. Verify ED25519 signature (embedded pubkey)  │
│  3. ONLY THEN: atomic rename to /apps/          │
│  4. On load: re-verify before execution         │
└─────────────────────────────────────────────────┘
```
| Layer | Mechanism | Protection |
|-------|-----------|------------|
| **App Integrity** | ED25519 signature on every `.espapp` | Prevents execution of tampered/unauthorized code |
| **OTA Security** | HTTPS + signature verification before install | Prevents MITM attacks on app downloads |
| **Memory Isolation** | Apps restricted to PSRAM pool via Kernel API | Prevents apps from corrupting kernel memory |
| **Filesystem Sandbox** | Path validation in `svc_fs.c` | Prevents apps from reading outside their data directory |
| **Thread Containment** | Kernel tracks all app threads, force-kills on exit | Prevents zombie threads after app crash |

- **Unsigned binaries are NEVER executed**
- **Partial downloads cannot corrupt active apps** (atomic rename)
- **Malicious server payloads are rejected** at the cryptographic layer
- **Apps cannot access kernel memory** (PSRAM pool isolation + opaque handles)
- **Apps cannot access other apps' data** (filesystem sandboxing)
  
---

## 🤝 Contributing

### Adding a New App
1. Create `apps/myapp/app_myapp.c` with `app_main(const KernelAPI*)` entry point
2. Add `CMakeLists.txt` using the `EspAppBuild.cmake` module
3. Create `myapp.manifest.json` with name, version, and build number
4. Build: `cd apps/myapp/build && cmake .. && make`
5. Copy `.espapp` and `.manifest.json` to SD card `/apps/`

### Extending the Kernel API
1. **Append** new function pointers to the appropriate sub-API struct in `kernel_api.h`
2. Bump `KERNEL_ABI_VERSION_MINOR`
3. Implement the function in the corresponding `svc_*.c`
4. Wire it in `kernel_main.c` `g_kernel_api` initialization
5. Update `g_api_fn_table` in `kernel_main.c` with the new function pointer
6. Update `build_espapp.py` relocation index mapping

> ⚠️ **Never remove or reorder existing API functions.** This will break all deployed apps.

---

## 📊 Performance Benchmarks (ESP32-S3-N16R8 @ 240MHz)

| Metric | Value | Notes |
|--------|-------|-------|
| App Load Time | <200ms | 100KB app from SDMMC 4-bit |
| Relocation Time | <5ms | ~50 relocations, single pass |
| PSRAM Bandwidth | ~35MB/s | Octal SPI, concurrent with I2S DMA |
| Audio Latency | ~23ms | 1KB ringbuffer chunk at 44.1kHz |
| LVGL Render | 30-60 FPS | 240×320 RGB565, SPI display |
| Wake Word Detection | <500ms | "Hi ESP" with ESP-SR WakeNet |
| Memory Overhead (Kernel) | ~1.2MB | SRAM + reserved PSRAM |
| App Binary Size (Clock) | ~12KB | No stdlib, PIC |
| App Binary Size (Audio+MP3) | ~150KB | Includes Helix decoder |

---

## 📖 Documentation

Detailed documentation for each subsystem is available in the `docs/` directory:

### Core Architecture
- **[Architecture Overview](docs/architecture.md)** — Kernel design, memory layout, task priorities, boot sequence
- **[Binary Format Specification](docs/binary-format.md)** — `.espapp` header, sections, relocation table, signature block
- **[Kernel API Reference](docs/kernel-api.md)** — Complete ABI v1.6 function signatures, types, error codes

### Persistent Services
- **[Audio Engine](docs/audio-engine.md)** — I2S TDM pipeline, ringbuffer architecture, mic multiplexer, format negotiation
- **[Display Manager](docs/display-manager.md)** — LVGL integration, sandboxed object lifecycle, property API, double buffering
- **[Storage Manager](docs/storage-manager.md)** — SDMMC hot-swap state machine, provisioning protocol, health checks
- **[Clock Service](docs/clock-service.md)** — NTP synchronization, RTC fallback, timezone handling
- **[Input Manager](docs/input-manager.md)** — Button debounce, touch events, encoder, system gesture interception

### System Services
- **[Power Management](docs/power-management.md)** — VAD-gated light sleep, power locks, deep sleep, menuconfig settings
- **[Crash Recovery](docs/crash-recovery.md)** — Soft/hard crash detection, NVS state machine, Safe Mode UI
- **[Memory Management](docs/memory-management.md)** — PSRAM block pool, ownership tagging, leak detection, force reclamation
- **[Wake Word](docs/wake-word.md)** — ESP-SR integration, AFE pipeline, mic multiplexing, power gating

### Application Development
- **[App Development Guide](docs/app-development.md)** — Writing your first `.espapp`, CMake setup, linker script, no-stdlib constraints
- **[OTA Updates](docs/ota-updates.md)** — Server protocol, three-stage download pipeline, signature verification

---

## Roadmap

### Completed (v1.0)
- [x] Custom `.espapp` binary format + linker script
- [x] PSRAM block pool allocator with leak detection
- [x] App loader, relocator, sandbox
- [x] Persistent services (Clock, Audio, Display, Storage, Input, Power)
- [x] Core apps (Clock, Launcher, Audio Player, Voice Assistant, Settings)
- [x] OTA update pipeline with ED25519 verification
- [x] Dual-layer crash recovery + Safe Mode
- [x] SD card hot-swap + auto-provisioning
- [x] Wake Word detection with VAD-gated Light Sleep
- [x] Power management (5-state ladder)

### Planned (v1.1)
- [ ] Offline Voice Commands (ESP-SR MultiNet, no server required)
- [ ] Bluetooth Audio (A2DP sink for phone streaming)
- [ ] Multi-language support (font packs, locale manifests)
- [ ] App Store UI (browse, search, install from server catalog)
- [ ] Dual-MCU Premium Config (dedicated display/audio processor)
- [ ] Telemetry service (crash reports, usage stats → server)
- [ ] Factory Test app (GPIO/Mic/Speaker/SD validation)

### Future (v2.0)
- [ ] Multi-app concurrent execution (picture-in-picture)
- [ ] Hardware-accelerated AI inference (ESP-DL / TFLite Micro)
- [ ] Custom display compositor (layered rendering)
- [ ] Plugin architecture for audio codecs (FLAC, AAC, OGG)

---

## Contributing

1. **Fork** the repository
2. **Branch** from `main`: `git checkout -b feature/my-feature`
3. **Follow the ABI rules**: Never modify existing `kernel_api.h` fields. Append only.
4. **Test**: Run `tests/` suite. Verify zero PSRAM leaks after 100 load/unload cycles.
5. **Sign**: All app binaries must be signed. CI will reject unsigned PRs.
6. **Submit PR** with description of changes and test results.

### Code Style
- C11, no C++ in kernel
- 4-space indent, `snake_case` for functions, `UPPER_CASE` for macros
- All public APIs documented with Doxygen comments
- No dynamic allocation in ISR context
- No ESP-IDF headers in app code (use `kernel_api.h` exclusively)

### Support

- **Issues:** [GitHub Issues](https://github.com/nanocult/ESPressOS/issues)
- **Discussions:** [GitHub Discussions](https://github.com/nanocult/ESPressOS/discussions)
- **ESP32-S3 Reference:** [Espressif Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
---

## License

This project is licensed under the **CC0 1.0 Universal**. See [LICENSE](LICENSE) for details.

Third-party components retain their own licenses:
- **LVGL v9**: MIT License
- **Helix MP3 Decoder** (libhelix-mp3): RPSL (RealNetworks Public Source License) — royalty-free
- **libopus**: BSD 3-Clause License
- **ESP-SR**: Espressif proprietary (free for ESP32 use)
- **mbedTLS** — Apache 2.0 License
  
---

## Acknowledgments

- [Espressif Systems](https://www.espressif.com/) — ESP32-S3, ESP-IDF, ESP-SR
- [LVGL](https://lvgl.io/) — Embedded graphics library
- [Helix MP3](https://www.helixcommunity.org/) — Fixed-point MP3 decoder
- [Opus](https://opus-codec.org/) — Open audio codec

*ESPressOS v1.0 — Built for the ESP32-S3. From microcontroller firmware to smart operating system.*

---

*Built for the embedded world. Runs on a chip the size of a fingernail.*
