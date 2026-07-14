#include "kernel_api.h"

/* Forward declarations of kernel implementation functions */
static void* k_malloc_impl(size_t size);
static void  k_free_impl(void* ptr);
/* ... one impl function per API entry ... */

/* Single static instance - lives in Internal SRAM, never freed */
static const KernelAPI g_kernel_api = {
    .abi_version = KERNEL_ABI_VERSION,
    .mem = {
        .malloc         = k_malloc_impl,
        .malloc_aligned = k_malloc_aligned_impl,
        .free           = k_free_impl,
        .free_heap_size = k_free_heap_size_impl,
        .realloc        = k_realloc_impl,
    },
    .display = { /* ... */ },
    .audio   = { /* ... */ },
    .fs      = { /* ... */ },
    .net     = { /* ... */ },
    .sys     = { /* ... */ },
};

/* Passed to app_main() by the loader after relocation */
const KernelAPI* kernel_get_api(void) {
    return &g_kernel_api;
}
