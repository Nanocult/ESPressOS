# App Binary Format Decision

> This document defines the EPSressOS Binary Format Specification v1.0. This format is designed specifically for the ESP32-S3 Xtensa LX7 architecture, optimizing for minimal storage footprint, fast sequential loading from SDMMC, and safe dynamic relocation in Octal PSRAM.

## 1. Design Principles

**Custom Flat Binary with Relocation Table (`.espapp`)**
*   **Not ELF:** Too heavy (headers, section tables, dynamic symbols add 2-5KB overhead per app).
*   **Not Raw `.bin`:** No relocation support = must load at fixed address = no concurrent apps.
*   **Our Format:**
    ```
    [Header 64B] [Code Section] [Data Section] [Relocation Table] [Signature]
    ```
    *   Header: Magic, version, API compat, entry point offset, sizes
    *   Compiled with `-fPIC -mlongcalls -nostdlib`
    *   Loader applies relocations in single pass (<10ms for typical app)
    *   Total overhead: ~80 bytes vs ELF's ~2KB+
    *   Load speed: Sequential read from SDMMC → memcpy to PSRAM → relocate → jump

1.  **Zero Internal SRAM Usage:** All app code/data resides in PSRAM. Only the loader stub and kernel API table use SRAM.
2.  **Single-Pass Relocation:** Relocations are applied sequentially during load, avoiding a second pass or symbol lookup.
3.  **Versioned ABI:** Apps declare minimum kernel API version. Loader rejects incompatible binaries before execution.
4.  **Security by Default:** Every binary is ED25519-signed. Unsigned/corrupt binaries never execute.
5.  **Alignment Safe:** All sections are 4-byte aligned to satisfy Xtensa LX7 memory access requirements.

## 2. File Structure Overview

```
Offset      Size        Field
─────────────────────────────────────
0x0000      64 B        Header
0x0040      Variable    Code Section (.text)
Variable    Variable    Read-Only Data (.rodata)
Variable    Variable    Initialized Data (.data)
Variable    Variable    Relocation Table
Variable    64 B        ED25519 Signature
─────────────────────────────────────
            (End of file)
```

> ⚠️ **Critical Alignment Rule:** All section offsets and sizes MUST be multiples of 4 bytes. The build toolchain must pad sections with `0x00` to maintain alignment. Unaligned access on LX7 causes fatal exceptions.

## 3. Header Definition (64 Bytes)

```c
#define ESPAPP_MAGIC        0x45535041  // "ESPA" little-endian
#define ESPAPP_ABI_VERSION  1           // Increment on breaking Kernel API changes
#define ESPAPP_MAX_NAME_LEN 32

typedef struct __attribute__((packed)) {
    uint32_t magic;             // Must be ESPAPP_MAGIC
    uint8_t  abi_version;       // Minimum required kernel API version
    uint8_t  flags;             // Bitfield: [0]=has_rodata, [1]=has_data, [2]=requires_wifi
    uint16_t reserved;          // Future use, must be 0
    
    char     name[ESPAPP_MAX_NAME_LEN]; // Null-terminated app display name
    
    uint32_t code_size;         // Size of .text section (bytes, 4-aligned)
    uint32_t rodata_size;       // Size of .rodata section (bytes, 4-aligned)
    uint32_t data_size;         // Size of .data section (bytes, 4-aligned)
    uint32_t bss_size;          // Size of .bss (zero-init, NOT stored in file)
    
    uint32_t reloc_count;       // Number of relocation entries
    uint32_t entry_offset;      // Entry point offset from start of code section
    
    uint32_t total_size;        // Total file size including signature (for validation)
    uint32_t header_crc32;      // CRC32 of bytes [0..59] (excludes this field + sig)
} espapp_header_t;
```

### Header Flags Bitfield
| Bit | Name | Description |
|-----|------|-------------|
| 0 | `HAS_RODATA` | File contains .rodata section |
| 1 | `HAS_DATA` | File contains .data section |
| 2 | `REQUIRES_WIFI` | App needs WiFi; loader ensures connection before launch |
| 3 | `REQUIRES_AUDIO` | App needs audio engine; loader reserves audio buffer |
| 4-7 | Reserved | Must be 0 |

## 4. Section Layout & Loading Order

Sections are stored contiguously in the file and loaded to **contiguous PSRAM** in this exact order:

| PSRAM Region | Source | Notes |
|--------------|--------|-------|
| Base + 0 | .text | Executable code, RX permissions |
| Base + code_size | .rodata | Constants, strings, R-only |
| Base + code_size + rodata_size | .data | RW initialized data |
| Base + ... + data_size | .bss | Zero-filled at load time, NOT in file |
| After .bss | App Heap Pool | Remaining PSRAM block for runtime allocs |

> 💡 **Loader Optimization:** Since sections are contiguous in both file and PSRAM, the loader performs a **single `sdmmc_read_sectors()` call** for all code+rodata+data, then a single `memset()` for BSS. No seeking required.

## 5. Relocation Table Format

Xtensa LX7 PIC generates `R_XTENSA_SLOT0_OP` and `R_XTENSA_ASM_EXPAND` relocations. We simplify to a custom compact format:

```c
typedef struct __attribute__((packed)) {
    uint32_t offset;    // Byte offset from start of CODE section where patch applies
    uint8_t  type;      // Relocation type (see below)
    uint8_t  target;    // Target identifier (see below)
    uint16_t addend;    // Signed addend (supports ±32KB range)
} espapp_reloc_t;
```

### Relocation Types
| Type ID | Name | Description | Patch Operation |
|---------|------|-------------|-----------------|
| 0 | `REL_ABS32` | Absolute 32-bit address | `*(uint32_t*)(code+offset) = base_addr + target_val + addend` |
| 1 | `REL_CALL` | CALLX/J instruction target | Rewrite immediate field with PC-relative offset |
| 2 | `REL_L32R` | L32R literal pool reference | Patch literal pool entry in .rodata |
| 3 | `REL_KERNEL_API` | Kernel function pointer ref | `target` = index into KernelAPI table |

### Target Identifiers
| Target Value | Meaning | Resolution |
|--------------|---------|------------|
| 0x00 | Code section base | `psram_app_base` |
| 0x01 | Rodata section base | `psram_app_base + code_size` |
| 0x02 | Data section base | `psram_app_base + code_size + rodata_size` |
| 0x03 | BSS section base | `psram_app_base + code_size + rodata_size + data_size` |
| 0x10–0xFF | Kernel API function index | `kernel_api_table[target - 0x10]` |

> ⚠️ **Why Custom Relocs Instead of ELF?** Standard Xtensa ELF relocations require symbol string tables, section headers, and complex resolution logic (~5KB loader code). Our flat table resolves in a tight loop: **~200 bytes of loader code, ~2μs per relocation**.

## 6. Signature Block (64 Bytes)

Located at `total_size - 64`. ED25519 signature over SHA-256 hash of `[header || code || rodata || data || reloc_table]`.

```c
typedef struct __attribute__((packed)) {
    uint8_t signature[64];  // ED25519 signature (r || s, 32+32 bytes)
} espapp_signature_t;
```

**Verification Flow:**
1. Read header → validate magic, CRC32, total_size
2. Stream-read body while computing SHA-256 incrementally (no full RAM copy needed)
3. Read signature block
4. Verify ED25519 against embedded public key
5. **Only then** copy body to PSRAM and apply relocations

## 7. Build Toolchain Integration

Each app uses this CMake configuration:

```cmake
# Custom linker script: espapp.ld
MEMORY {
    APP_CODE   : ORIGIN = 0x00000000, LENGTH = 2M
    APP_RODATA : ORIGIN = 0x00200000, LENGTH = 512K
    APP_DATA   : ORIGIN = 0x00280000, LENGTH = 512K
}

SECTIONS {
    .text    : { *(.text*) } > APP_CODE
    .rodata  : { *(.rodata*) } > APP_RODATA
    .data    : { *(.data*) } > APP_DATA
    .bss     : { *(.bss*) *(COMMON) } > APP_DATA
}
```

**Post-build steps (automated):**
1. Extract raw sections via `objcopy -O binary --only-section=.text/.rodata/.data`
2. Generate relocation table via custom Python script parsing `objdump -r` output
3. Assemble header + sections + relocs
4. Compute CRC32 and sign with `esptool.py sign_espapp`
5. Output final `.espapp` file

## 8. Runtime Load Sequence (Pseudocode)

```c
esp_err_t load_app(const char* path, void** entry_point) {
    // 1. Open & read header
    espapp_header_t hdr;
    sdmmc_read(path, 0, &hdr, sizeof(hdr));
    if (hdr.magic != ESPAPP_MAGIC || !verify_crc32(&hdr)) return ESP_FAIL;
    if (hdr.abi_version > KERNEL_ABI_VERSION) return ESP_INCOMPATIBLE;
    
    // 2. Allocate contiguous PSRAM block
    size_t total_ram = hdr.code_size + hdr.rodata_size + hdr.data_size + hdr.bss_size;
    void* app_base = psram_pool_alloc_aligned(total_ram, 4);
    if (!app_base) return ESP_NO_MEM;
    
    // 3. Stream-read body + verify signature incrementally
    sha256_ctx ctx; sha256_init(&ctx);
    sdmmc_stream_read(path, sizeof(hdr), hdr.total_size - 64, 
                      app_base, &ctx);
    if (!verify_signature(&ctx, path, hdr.total_size)) {
        psram_pool_free(app_base); return ESP_SIG_INVALID;
    }
    
    // 4. Zero BSS
    memset(app_base + hdr.code_size + hdr.rodata_size + hdr.data_size, 
           0, hdr.bss_size);
    
    // 5. Apply relocations (single pass)
    espapp_reloc_t* relocs = (espapp_reloc_t*)(app_base + hdr.code_size 
                          + hdr.rodata_size + hdr.data_size); // Stored after data in RAM
    for (uint32_t i = 0; i < hdr.reloc_count; i++) {
        apply_relocation(app_base, &relocs[i], kernel_api_table);
    }
    
    // 6. Return entry point
    *entry_point = app_base + hdr.entry_offset;
    return ESP_OK;
}
```

## 9. Migration & Compatibility Rules

| Change Type | Action | ABI Version Impact |
|-------------|--------|--------------------|
| Add new kernel API function (append only) | Update kernel_api_table size | No bump (backward compat) |
| Remove/reorder kernel API function | N/A – forbidden | Breaking change |
| Modify header struct layout | Bump `ESPAPP_ABI_VERSION` | Breaking change |
| Add new relocation type | Bump `ESPAPP_ABI_VERSION` | Breaking change |
| Add new flag bit (unused bits) | Document, no version bump | Forward compatible |

> 🔒 **Golden Rule:** Never modify existing fields in `espapp_header_t` or existing relocation types. Always append. Apps built against ABI v1 must run on kernel v1, v2, v3... indefinitely.
