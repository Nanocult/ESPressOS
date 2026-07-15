# App Store (OTA App Update Channel)

**OTA App Update Channel** (App Store) to push new apps to the device wirelessly. Build the "Settings" app that checks the server for new `.espapp` versions and downloads them to `/sdcard/apps/`. A robust OTA (Over-The-Air) system must guarantee that a failed download, power loss, or malicious server payload **never bricks the device**. We will achieve this using a **Three-Stage Atomic Pipeline**: Download to temporary cache → Cryptographic Verification → Atomic Filesystem Rename.

### 1. Kernel API Extensions

Update `kernel_api.h` and bump `KERNEL_ABI_VERSION` to `((1 << 8) | 4)` (v1.4).

```c
/* --- ADD TO kernel_api.h --- */

/* Add to k_fs_api_t (append only): */
typedef struct {
    // ... existing fields ...
    k_err_t (*rename)(const char* old_path, const char* new_path);
    k_err_t (*remove)(const char* path);
} k_fs_api_t;

/* Add to k_sys_api_t (append only): */
typedef struct {
    // ... existing fields ...
    /** Verify ED25519 signature of a .espapp file on SD card. 
     *  Returns true only if valid and matches kernel public key. */
    bool (*verify_app_file)(const char* path);
} k_sys_api_t;
```

> ⚠️ **Kernel Implementation Note:** `svc_sys_verify_app_file()` must stream the file from SD, compute SHA-256, and verify against the embedded ED25519 public key. It must **not** load the file into PSRAM.

### 2. Server API Contract

Your backend must expose a simple JSON manifest. Using an integer `build` number makes version comparison trivial without needing complex semantic versioning parsers in a `-nostdlib` environment.

**Endpoint:** `GET https://ota.yourserver.com/manifest.json`
```json
{
  "apps": [
    {
      "name": "clock",
      "build": 15,
      "url": "https://ota.yourserver.com/apps/clock.espapp"
    },
    {
      "name": "launcher",
      "build": 8,
      "url": "https://ota.yourserver.com/apps/launcher.espapp"
    }
  ]
}
```

**Local SD Card Manifest (`/sdcard/apps/clock.manifest.json`):**
```json
{
  "name": "Clock",
  "build": 12,
  "icon": "clock.bin"
}
```

### 3. The Settings & OTA App (`app_settings.c`)

This app handles network requests, JSON parsing, streaming downloads, and UI updates entirely on the main thread. Because `http_stream` yields to the kernel's network task, the UI remains responsive enough for a progress indicator.


### 4. Safety Guarantees & Architecture Validation

This implementation includes several critical safeguards that distinguish a toy project from a production embedded OS:

| Threat Vector | Mitigation Strategy |
| :--- | :--- |
| **Power loss during download** | File is written to `/cache/*.tmp`. The active `/apps/*.espapp` remains untouched and bootable. On next boot, the kernel ignores `.tmp` files. |
| **Malicious Server / MITM** | `sys.verify_app_file()` checks ED25519 signature *before* the rename. If verification fails, the `.tmp` file is deleted. The device refuses to execute unverified code. |
| **SD Card Full** | `fs.write` returns `K_ERR_IO`, aborting the stream. The partial `.tmp` file is deleted. The UI shows "Network Error" (or IO Error), but the OS remains stable. |
| **Corrupt JSON from Server** | The parser uses strict bounds checking (`max_len`) and null-terminates all buffers. Malformed JSON simply results in `updates = 0` or skipped apps. No buffer overflows. |
| **Bricking the Launcher** | If `launcher.espapp` fails verification, the old version is kept. The user is never locked out of the device. |

### 5. Kernel-Side Implementation Requirements

To make this app functional, you must implement the new API wrappers in your kernel:

1.  **`svc_fs_rename` / `svc_fs_remove`**: Wrap POSIX `rename()` and `remove()`. Ensure paths are sandboxed to `/sdcard/`.
2.  **`svc_net_http_stream`**: Use ESP-IDF's `esp_http_client`. Set `buffer_size` to 1024 bytes. In the read loop, call the app's `on_chunk` callback. If the callback returns `!= K_OK`, abort the HTTP client.
3.  **`svc_sys_verify_app_file`**: 
    *   Open file, read in 4KB chunks.
    *   Feed chunks to `mbedtls_sha256_update`.
    *   Read the last 64 bytes as the signature.
    *   Call `mbedtls_ed25519_verify` against the hardcoded kernel public key.
    *   *Crucial:* This function must run on the calling thread (the app's thread) but yield periodically (`vTaskDelay(1)`) to prevent triggering the Task Watchdog during large file reads.
