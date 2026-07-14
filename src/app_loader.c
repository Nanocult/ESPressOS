#include "app_loader.h"
#include "psram_pool.h"       /* Your PSRAM block allocator */
#include "kernel_internal.h"  /* kernel_get_api(), signature verification */
#include "espapp_format.h"    /* Header/reloc structs from spec */

#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ed25519.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "app_loader";

/* ========================================================================== */
/* INTERNAL CONTEXT STRUCTURE                                                 */
/* ========================================================================== */

struct app_context {
    void*       psram_base;     /* Base address of allocated PSRAM block */
    size_t      total_ram;      /* Total PSRAM consumed */
    uint32_t    entry_offset;   /* Offset from psram_base to app_main */
    char        name[32];       /* Copy of app name from header */
    bool        is_loaded;      /* Guard against double-run/unload */
};

/* ========================================================================== */
/* HEADER VALIDATION                                                          */
/* ========================================================================== */

static load_result_t validate_header(const espapp_header_t* hdr) {
    if (hdr->magic != ESPAPP_MAGIC) {
        ESP_LOGE(TAG, "Bad magic: 0x%08X", hdr->magic);
        return LOAD_ERR_BAD_MAGIC;
    }

    /* Verify header CRC32 (covers bytes 0..59) */
    uint32_t computed_crc = esp_rom_crc32_le(0, (const uint8_t*)hdr, 60);
    if (computed_crc != hdr->header_crc32) {
        ESP_LOGE(TAG, "Header CRC mismatch: expected 0x%08X, got 0x%08X",
                 hdr->header_crc32, computed_crc);
        return LOAD_ERR_BAD_CRC;
    }

    /* ABI compatibility check */
    if (hdr->abi_version > KERNEL_ABI_VERSION) {
        ESP_LOGE(TAG, "App requires ABI v%d, kernel provides v%d",
                 hdr->abi_version, KERNEL_ABI_VERSION);
        return LOAD_ERR_ABI_MISMATCH;
    }

    /* Size sanity checks */
    size_t loaded_size = hdr->code_size + hdr->rodata_size + hdr->data_size;
    size_t total_ram = loaded_size + hdr->bss_size;

    if (total_ram > APP_MAX_LOAD_SIZE) {
        ESP_LOGE(TAG, "App too large: %u bytes (max %lu)", total_ram, APP_MAX_LOAD_SIZE);
        return LOAD_ERR_TOO_LARGE;
    }

    if (hdr->entry_offset >= hdr->code_size) {
        ESP_LOGE(TAG, "Entry offset 0x%X outside code section (size 0x%X)",
                 hdr->entry_offset, hdr->code_size);
        return LOAD_ERR_NO_ENTRY;
    }

    /* Alignment validation */
    if ((hdr->code_size % 4) || (hdr->rodata_size % 4) ||
        (hdr->data_size % 4) || (hdr->bss_size % 4)) {
        ESP_LOGE(TAG, "Section sizes not 4-byte aligned");
        return LOAD_ERR_BAD_CRC; /* Reuse error code for malformed binary */
    }

    return LOAD_OK;
}

/* ========================================================================== */
/* RELOCATION ENGINE                                                          */
/* Single-pass, in-place patching of PSRAM-resident code                      */
/* ========================================================================== */

static load_result_t apply_relocations(
    void* app_base,
    const espapp_header_t* hdr,
    const KernelAPI* api)
{
    /* Relocation table sits immediately after .data in PSRAM */
    size_t reloc_table_offset = hdr->code_size + hdr->rodata_size + hdr->data_size;
    const espapp_reloc_t* relocs = (const espapp_reloc_t*)(
        (uint8_t*)app_base + reloc_table_offset);

    /* Section base addresses for target resolution */
    const uintptr_t code_base   = (uintptr_t)app_base;
    const uintptr_t rodata_base = code_base + hdr->code_size;
    const uintptr_t data_base   = rodata_base + hdr->rodata_size;
    const uintptr_t bss_base    = data_base + hdr->data_size;

    for (uint32_t i = 0; i < hdr->reloc_count; i++) {
        const espapp_reloc_t* r = &relocs[i];
        uintptr_t patch_addr = code_base + r->offset;
        uintptr_t target_val = 0;

        /* Resolve target value based on target identifier */
        switch (r->target) {
            case 0x00: target_val = code_base;   break;
            case 0x01: target_val = rodata_base; break;
            case 0x02: target_val = data_base;   break;
            case 0x03: target_val = bss_base;    break;
            default:
                if (r->target >= 0x10 && r->target <= 0xFF) {
                    /* Kernel API function pointer lookup */
                    uint8_t api_index = r->target - 0x10;
                    /* Cast API struct to array of function pointers */
                    const uintptr_t* api_fns = (const uintptr_t*)api;
                    /* Skip abi_version field (index 0), sub-API structs follow */
                    /* NOTE: This indexing matches KernelAPI struct layout exactly.
                     * If struct changes, this MUST be updated. */
                    if (api_index >= sizeof(KernelAPI) / sizeof(uintptr_t)) {
                        ESP_LOGE(TAG, "Reloc #%u: kernel API index %u out of range", i, api_index);
                        return LOAD_ERR_RELOC;
                    }
                    target_val = api_fns[api_index];
                } else {
                    ESP_LOGE(TAG, "Reloc #%u: unknown target 0x%02X", i, r->target);
                    return LOAD_ERR_RELOC;
                }
                break;
        }

        target_val += (int16_t)r->addend; /* Sign-extended addend */

        /* Apply patch based on relocation type */
        switch (r->type) {
            case REL_ABS32: {
                *(uint32_t*)patch_addr = (uint32_t)target_val;
                break;
            }
            case REL_CALL: {
                /* Xtensa CALLX/J: rewrite immediate field with PC-relative offset */
                int32_t pc_rel = (int32_t)(target_val - (patch_addr + 3));
                uint32_t insn = *(uint32_t*)patch_addr;
                /* CALL instruction format: [op0=0xCC][imm18] */
                insn = (insn & 0xFF000000) | ((pc_rel >> 2) & 0x00FFFFFF);
                *(uint32_t*)patch_addr = insn;
                break;
            }
            case REL_L32R: {
                /* L32R: literal pool entry already in rodata, patch the literal */
                *(uint32_t*)patch_addr = (uint32_t)target_val;
                break;
            }
            case REL_KERNEL_API: {
                /* Direct function pointer store (for GOT-like entries) */
                *(uint32_t*)patch_addr = (uint32_t)target_val;
                break;
            }
            default:
                ESP_LOGE(TAG, "Reloc #%u: unknown type %u", i, r->type);
                return LOAD_ERR_RELOC;
        }
    }

    ESP_LOGI(TAG, "Applied %u relocations successfully", hdr->reloc_count);
    return LOAD_OK;
}

/* ========================================================================== */
/* PUBLIC API IMPLEMENTATION                                                  */
/* ========================================================================== */

load_result_t app_loader_load(const char* path, app_context_t** out_ctx) {
    *out_ctx = NULL;
    FILE* f = NULL;
    void* psram_block = NULL;
    load_result_t result = LOAD_OK;

    ESP_LOGI(TAG, "Loading app: %s", path);

    /* --- Step 1: Open and read header --- */
    f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open: %s", path);
        return LOAD_ERR_FILE_OPEN;
    }

    espapp_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        result = LOAD_ERR_HEADER_READ;
        goto cleanup;
    }

    result = validate_header(&hdr);
    if (result != LOAD_OK) goto cleanup;

    ESP_LOGI(TAG, "App '%.*s' ABI v%d | code:%u rodata:%u data:%u bss:%u relocs:%u",
             32, hdr.name, hdr.abi_version,
             hdr.code_size, hdr.rodata_size, hdr.data_size,
             hdr.bss_size, hdr.reloc_count);

    /* --- Step 2: Allocate contiguous PSRAM --- */
    size_t loaded_size = hdr.code_size + hdr.rodata_size + hdr.data_size;
    size_t total_ram = loaded_size + hdr.bss_size;

    psram_block = psram_pool_alloc_aligned(total_ram, 4);
    if (!psram_block) {
        ESP_LOGE(TAG, "PSRAM allocation failed (%u bytes)", total_ram);
        result = LOAD_ERR_PSALLOC;
        goto cleanup;
    }

    /* --- Step 3: Stream-read body + incremental SHA-256 --- */
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0); /* SHA-256, not SHA-224 */

    uint8_t stream_buf[4096]; /* Read in 4KB chunks for SDMMC efficiency */
    size_t remaining = loaded_size + (hdr.reloc_count * sizeof(espapp_reloc_t));
    size_t psram_offset = 0;

    while (remaining > 0) {
        size_t chunk = (remaining < sizeof(stream_buf)) ? remaining : sizeof(stream_buf);
        size_t nread = fread(stream_buf, 1, chunk, f);
        if (nread != chunk) {
            result = LOAD_ERR_BODY_READ;
            mbedtls_sha256_free(&sha_ctx);
            goto cleanup;
        }

        /* Hash incrementally */
        mbedtls_sha256_update(&sha_ctx, stream_buf, nread);

        /* Copy to PSRAM */
        memcpy((uint8_t*)psram_block + psram_offset, stream_buf, nread);
        psram_offset += nread;
        remaining -= nread;
    }

    /* Finalize hash */
    uint8_t body_hash[32];
    mbedtls_sha256_finish(&sha_ctx, body_hash);
    mbedtls_sha256_free(&sha_ctx);

    /* --- Step 4: Read and verify signature --- */
    espapp_signature_t sig;
    if (fread(&sig, 1, sizeof(sig), f) != sizeof(sig)) {
        result = LOAD_ERR_BODY_READ;
        goto cleanup;
    }

    if (!kernel_verify_signature(body_hash, sig.signature)) {
        ESP_LOGE(TAG, "Signature verification FAILED for %s", path);
        result = LOAD_ERR_SIGNATURE;
        goto cleanup;
    }

    fclose(f);
    f = NULL; /* Prevent double-close in cleanup */

    /* --- Step 5: Zero-fill BSS --- */
    memset((uint8_t*)psram_block + loaded_size, 0, hdr.bss_size);

    /* --- Step 6: Apply relocations --- */
    const KernelAPI* api = kernel_get_api();
    result = apply_relocations(psram_block, &hdr, api);
    if (result != LOAD_OK) goto cleanup;

    /* --- Step 7: Build context --- */
    app_context_t* ctx = (app_context_t*)malloc(sizeof(app_context_t));
    if (!ctx) {
        result = LOAD_ERR_PSALLOC;
        goto cleanup;
    }

    ctx->psram_base    = psram_block;
    ctx->total_ram     = total_ram;
    ctx->entry_offset  = hdr.entry_offset;
    ctx->is_loaded     = true;
    memcpy(ctx->name, hdr.name, sizeof(ctx->name));

    *out_ctx = ctx;
    ESP_LOGI(TAG, "✓ App '%.*s' loaded at %p (%u bytes)", 32, hdr.name, psram_block, total_ram);
    return LOAD_OK;

cleanup:
    if (f) fclose(f);
    if (psram_block) psram_pool_free(psram_block);
    ESP_LOGE(TAG, "Load failed: %s", app_loader_error_str(result));
    return result;
}

void app_loader_run(app_context_t* ctx) {
    if (!ctx || !ctx->is_loaded) {
        ESP_LOGE(TAG, "Cannot run: invalid context");
        return;
    }

    typedef void (*app_entry_fn)(const KernelAPI*);
    app_entry_fn entry = (app_entry_fn)((uint8_t*)ctx->psram_base + ctx->entry_offset);

    ESP_LOGI(TAG, "▶ Running app '%.*s' @ %p", 32, ctx->name, (void*)entry);

    /* Transfer execution. Returns when app calls request_exit() or crashes. */
    const KernelAPI* api = kernel_get_api();
    entry(api);

    ESP_LOGI(TAG, "◀ App '%.*s' exited", 32, ctx->name);
}

void app_loader_unload(app_context_t* ctx) {
    if (!ctx) return;

    ESP_LOGI(TAG, "Unloading app '%.*s'", 32, ctx->name);

    /* Attempt graceful deinit if symbol exists */
    if (ctx->is_loaded && ctx->psram_base) {
        /* Search for app_deinit in first 4KB of code section.
         * In production, store deinit offset in header instead. */
        typedef void (*deinit_fn)(void);
        /* For now, we rely on the weak symbol being linked at a known offset.
         * TODO: Add deinit_offset to espapp_header_t in next ABI revision. */
    }

    /* Free PSRAM block */
    if (ctx->psram_base) {
        psram_pool_free(ctx->psram_base);
        ctx->psram_base = NULL;
    }

    ctx->is_loaded = false;
    free(ctx);
}

const char* app_loader_error_str(load_result_t err) {
    static const char* strs[] = {
        [LOAD_OK]              = "OK",
        [LOAD_ERR_FILE_OPEN]   = "File open failed",
        [LOAD_ERR_HEADER_READ] = "Header read incomplete",
        [LOAD_ERR_BAD_MAGIC]   = "Invalid magic number",
        [LOAD_ERR_BAD_CRC]     = "Header CRC mismatch",
        [LOAD_ERR_ABI_MISMATCH]= "ABI version incompatible",
        [LOAD_ERR_TOO_LARGE]   = "App exceeds max size",
        [LOAD_ERR_PSALLOC]     = "PSRAM allocation failed",
        [LOAD_ERR_BODY_READ]   = "Body read incomplete",
        [LOAD_ERR_SIGNATURE]   = "Signature verification failed",
        [LOAD_ERR_RELOC]       = "Relocation error",
        [LOAD_ERR_NO_ENTRY]    = "Invalid entry point",
    };
    if (err < 0 || err >= sizeof(strs)/sizeof(strs[0])) return "Unknown error";
    return strs[err] ? strs[err] : "Unknown error";
}
