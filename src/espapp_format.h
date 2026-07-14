#ifndef ESPAPP_FORMAT_H
#define ESPAPP_FORMAT_H

#include <stdint.h>

#define ESPAPP_MAGIC 0x45535041U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  abi_version;
    uint8_t  flags;
    uint16_t reserved;
    char     name[32];
    uint32_t code_size;
    uint32_t rodata_size;
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t reloc_count;
    uint32_t entry_offset;
    uint32_t total_size;
    uint32_t header_crc32;
} espapp_header_t;

typedef struct __attribute__((packed)) {
    uint32_t offset;
    uint8_t  type;
    uint8_t  target;
    uint16_t addend;
} espapp_reloc_t;

typedef struct __attribute__((packed)) {
    uint8_t signature[64];
} espapp_signature_t;

/* Relocation types */
#define REL_ABS32       0
#define REL_CALL        1
#define REL_L32R        2
#define REL_KERNEL_API  3

#endif
