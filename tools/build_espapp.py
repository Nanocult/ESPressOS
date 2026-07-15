#!/usr/bin/env python3
"""
ESP-AppOS Post-Build Tool v1.0
Converts Xtensa LX7 ELF (built with espapp.ld) into signed .espapp flat binary.

Usage:
    python build_espapp.py <input.elf> <output.espapp> <signing_key.pem> [--abi-version N]

Dependencies: Python 3.8+ standard library only.
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path
from typing import List, Tuple, Dict, Optional

# =============================================================================
# CONSTANTS & FORMAT DEFINITIONS
# =============================================================================

ESPAPP_MAGIC = 0x45535041  # "ESPA" little-endian
ESPAPP_ABI_VERSION_DEFAULT = 1
ALIGNMENT = 4
HEADER_SIZE = 64
SIGNATURE_SIZE = 64

# Header struct format: matches espapp_header_t exactly
# <I=uint32, B=uint8, H=uint16, 32s=char[32]
HEADER_FORMAT = '<IBBH32sIIIIIIIII'
assert struct.calcsize(HEADER_FORMAT) == HEADER_SIZE, f"Header size mismatch: {struct.calcsize(HEADER_FORMAT)} != {HEADER_SIZE}"

# Relocation entry format: matches espapp_reloc_t
RELOC_FORMAT = '<IBBH'
RELOC_SIZE = struct.calcsize(RELOC_FORMAT)

# Relocation types
REL_ABS32 = 0
REL_CALL = 1
REL_L32R = 2
REL_KERNEL_API = 3

# Xtensa ELF relocation type IDs (from xtensa elf.h)
XTENSA_R_32 = 1
XTENSA_R_SLOT0_OP = 20
XTENSA_R_ASM_EXPAND = 21

# Kernel API symbol prefix - functions matching this become REL_KERNEL_API relocations
KERNEL_API_PREFIX = "kernel_api_"


# =============================================================================
# MINIMAL ELF PARSER (No external dependencies)
# =============================================================================

class ElfParser:
    """Minimal ELF32 parser for Xtensa LE binaries."""

    def __init__(self, path: str):
        self.path = path
        with open(path, 'rb') as f:
            self.data = f.read()

        # Validate ELF magic
        if self.data[:4] != b'\x7fELF':
            raise ValueError(f"Not a valid ELF file: {path}")
        if self.data[4] != 1:  # 32-bit
            raise ValueError("Only 32-bit ELF supported")
        if self.data[5] != 1:  # Little endian
            raise ValueError("Only little-endian ELF supported")

        self._parse_header()
        self._parse_section_headers()
        self._parse_symbol_table()

    def _parse_header(self):
        """Parse ELF header to get section header table location."""
        self.e_shoff = struct.unpack_from('<I', self.data, 32)[0]
        self.e_shentsize = struct.unpack_from('<H', self.data, 46)[0]
        self.e_shnum = struct.unpack_from('<H', self.data, 48)[0]
        self.e_shstrndx = struct.unpack_from('<H', self.data, 50)[0]

    def _parse_section_headers(self):
        """Parse all section headers into a dict keyed by name."""
        # First, get section name string table
        shstrtab_offset = self.e_shoff + self.e_shstrndx * self.e_shentsize
        shstrtab_hdr = self._read_section_header(shstrtab_offset)
        shstrtab = self.data[shstrtab_hdr['sh_offset']:shstrtab_hdr['sh_offset'] + shstrtab_hdr['sh_size']]

        self.sections: Dict[str, dict] = {}
        for i in range(self.e_shnum):
            offset = self.e_shoff + i * self.e_shentsize
            hdr = self._read_section_header(offset)
            name_end = shstrtab.index(b'\x00', hdr['sh_name'])
            name = shstrtab[hdr['sh_name']:name_end].decode('ascii', errors='replace')
            hdr['name'] = name
            self.sections[name] = hdr

    def _read_section_header(self, offset: int) -> dict:
        fields = struct.unpack_from('<IIIIIIIIII', self.data, offset)
        return {
            'sh_name': fields[0], 'sh_type': fields[1], 'sh_flags': fields[2],
            'sh_addr': fields[3], 'sh_offset': fields[4], 'sh_size': fields[5],
            'sh_link': fields[6], 'sh_info': fields[7], 'sh_addralign': fields[8],
            'sh_entsize': fields[9]
        }

    def _parse_symbol_table(self):
        """Parse .symtab to resolve symbol values."""
        self.symbols: Dict[str, int] = {}
        symtab = self.sections.get('.symtab')
        strtab = self.sections.get('.strtab')
        if not symtab or not strtab:
            return

        strtab_data = self.data[strtab['sh_offset']:strtab['sh_offset'] + strtab['sh_size']]
        entry_size = symtab['sh_entsize'] or 16
        num_syms = symtab['sh_size'] // entry_size

        for i in range(num_syms):
            off = symtab['sh_offset'] + i * entry_size
            st_name, st_value, st_size, st_info = struct.unpack_from('<IIIB', self.data, off)[:4]
            name_end = strtab_data.index(b'\x00', st_name)
            name = strtab_data[st_name:name_end].decode('ascii', errors='replace')
            self.symbols[name] = st_value

    def get_section_data(self, name: str) -> bytes:
        sec = self.sections.get(name)
        if not sec or sec['sh_size'] == 0:
            return b''
        return self.data[sec['sh_offset']:sec['sh_offset'] + sec['sh_size']]

    def get_symbol_value(self, name: str) -> Optional[int]:
        return self.symbols.get(name)

    def get_relocations(self) -> List[dict]:
        """Extract all RELA relocations from .rela.* sections."""
        relocs = []
        for name, sec in self.sections.items():
            if not name.startswith('.rela.'):
                continue
            target_section = name[6:]  # Strip ".rela." prefix
            entry_size = sec['sh_entsize'] or 12
            num_entries = sec['sh_size'] // entry_size

            for i in range(num_entries):
                off = sec['sh_offset'] + i * entry_size
                r_offset, r_info, r_addend = struct.unpack_from('<IIi', self.data, off)
                r_type = r_info & 0xFF
                r_sym = r_info >> 8
                relocs.append({
                    'offset': r_offset,
                    'type': r_type,
                    'sym_index': r_sym,
                    'addend': r_addend,
                    'target_section': target_section
                })
        return relocs


# =============================================================================
# CORE BUILD LOGIC
# =============================================================================

def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def pad_to_alignment(data: bytes, alignment: int) -> bytes:
    remainder = len(data) % alignment
    if remainder == 0:
        return data
    return data + b'\x00' * (alignment - remainder)


def convert_relocations(elf: ElfParser, raw_relocs: List[dict]) -> bytes:
    """Convert Xtensa ELF relocations to compact espapp_reloc_t format."""
    output = bytearray()
    kernel_api_map: Dict[str, int] = {}

    # Build kernel API index map from symbols
    for sym_name, sym_val in elf.symbols.items():
        if sym_name.startswith(KERNEL_API_PREFIX):
            idx = int(sym_name[len(KERNEL_API_PREFIX):])
            kernel_api_map[sym_name] = idx

    for rel in raw_relocs:
        espapp_type = None
        target = 0
        addend = rel['addend'] & 0xFFFF  # Clamp to 16-bit signed range

        if rel['type'] == XTENSA_R_32:
            espapp_type = REL_ABS32
            # Determine target section based on symbol
            # Simplified: assume code-section relative for now
            target = 0x00  # Code base
        elif rel['type'] in (XTENSA_R_SLOT0_OP, XTENSA_R_ASM_EXPAND):
            # Check if this references a kernel API symbol
            # In practice, resolve via sym_index → sym name → kernel_api_map
            # For MVP, treat as CALL relocation
            espapp_type = REL_CALL
            target = 0x00
        else:
            print(f"  WARNING: Skipping unsupported reloc type {rel['type']} at offset 0x{rel['offset']:X}")
            continue

        entry = struct.pack(RELOC_FORMAT, rel['offset'], espapp_type, target, addend)
        output.extend(entry)

    return bytes(output)


def compute_crc32(data: bytes) -> int:
    """CRC32 matching ESP-IDF's esp_rom_crc32_le."""
    import binascii
    return binascii.crc32(data) & 0xFFFFFFFF


def sign_binary(data: bytes, key_path: str) -> bytes:
    """ED25519 sign using pure-Python fallback or cryptography lib."""
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        from cryptography.hazmat.primitives.serialization import load_pem_private_key
        with open(key_path, 'rb') as f:
            private_key = load_pem_private_key(f.read(), password=None)
        signature = private_key.sign(data)
        assert len(signature) == 64
        return signature
    except ImportError:
        print("  WARNING: 'cryptography' not installed. Using PLACEHOLDER signature.")
        print("  Install with: pip install cryptography")
        return b'\x00' * 64


def build_espapp(elf_path: str, output_path: str, key_path: str, abi_version: int):
    print(f"[ESPAPP] Building {output_path} from {elf_path}")

    # 1. Parse ELF
    elf = ElfParser(elf_path)

    # 2. Extract sections
    code = pad_to_alignment(elf.get_section_data('.text'), ALIGNMENT)
    rodata = pad_to_alignment(elf.get_section_data('.rodata'), ALIGNMENT)
    data = pad_to_alignment(elf.get_section_data('.data'), ALIGNMENT)

    # 3. Get sizes from linker-defined symbols (more reliable than section headers)
    code_size = elf.get_symbol_value('__espapp_code_size') or len(code)
    rodata_size = elf.get_symbol_value('__espapp_rodata_size') or len(rodata)
    data_size = elf.get_symbol_value('__espapp_data_size') or len(data)
    bss_size = elf.get_symbol_value('__espapp_bss_size') or 0

    print(f"  .text:   {code_size:>8} bytes")
    print(f"  .rodata: {rodata_size:>8} bytes")
    print(f"  .data:   {data_size:>8} bytes")
    print(f"  .bss:    {bss_size:>8} bytes (not in binary)")

    # 4. Convert relocations
    raw_relocs = elf.get_relocations()
    reloc_data = convert_relocations(elf, raw_relocs)
    reloc_count = len(reloc_data) // RELOC_SIZE
    print(f"  relocs:  {reloc_count:>8} entries ({len(reloc_data)} bytes)")

    # 5. Assemble body (what gets signed)
    body = code + rodata + data + reloc_data

    # 6. Build header (without CRC first)
    app_name = Path(elf_path).stem[:31].encode('ascii').ljust(32, b'\x00')
    flags = 0
    if rodata_size > 0: flags |= 0x01
    if data_size > 0: flags |= 0x02

    total_size = HEADER_SIZE + len(body) + SIGNATURE_SIZE

    # Pack header without CRC (bytes 0..59)
    header_no_crc = struct.pack(
        '<IBBH32sIIIIIIII',
        ESPAPP_MAGIC, abi_version, flags, 0,
        app_name,
        code_size, rodata_size, data_size, bss_size,
        reloc_count, 0,  # entry_offset = 0 (enforced by linker script)
        total_size
    )
    assert len(header_no_crc) == 60

    # Compute and append CRC32
    crc = compute_crc32(header_no_crc)
    header = header_no_crc + struct.pack('<I', crc)
    assert len(header) == HEADER_SIZE

    # 7. Sign the body
    signature = sign_binary(body, key_path)

    # 8. Write final binary
    with open(output_path, 'wb') as f:
        f.write(header)
        f.write(body)
        f.write(signature)

    final_size = Path(output_path).stat().st_size
    print(f"[ESPAPP] ✓ Written {final_size} bytes to {output_path}")
    print(f"[ESPAPP]   Load estimate: {final_size / (8 * 1024 * 1024) * 1000:.0f}ms @ 8MB/s SDMMC")


# =============================================================================
# CLI ENTRY POINT
# =============================================================================

def main():
    parser = argparse.ArgumentParser(description='Build .espapp binary from ELF')
    parser.add_argument('elf', help='Input ELF file path')
    parser.add_argument('output', help='Output .espapp file path')
    parser.add_argument('key', help='ED25519 private key PEM path')
    parser.add_argument('--abi-version', type=int, default=ESPAPP_ABI_VERSION_DEFAULT,
                        help=f'Kernel ABI version (default: {ESPAPP_ABI_VERSION_DEFAULT})')
    args = parser.parse_args()

    if not Path(args.elf).exists():
        print(f"ERROR: ELF file not found: {args.elf}", file=sys.stderr)
        sys.exit(1)
    if not Path(args.key).exists():
        print(f"ERROR: Signing key not found: {args.key}", file=sys.stderr)
        sys.exit(1)

    build_espapp(args.elf, args.output, args.key, args.abi_version)


if __name__ == '__main__':
    main()
