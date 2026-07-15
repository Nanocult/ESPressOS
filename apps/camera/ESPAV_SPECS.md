# A/V Container Format Design

A simple custom container format called **ESPAV** (ESP Audio Video)

```
┌─────────────────────────────────────────┐
│ Header (32 bytes)                       │
│ - Magic: "ESPAV" (4 bytes)              │
│ - Version: 1 (1 byte)                   │
│ - Video FPS: uint8 (1 byte)             │
│ - Audio Sample Rate: uint32 (4 bytes)   │
│ - Audio Channels: uint8 (1 byte)        │
│ - Audio Bits: uint8 (1 byte)            │
│ - Reserved: 20 bytes                    │
└─────────────────────────────────────────┘
┌─────────────────────────────────────────┐
│ Chunk 1: Video Frame                    │
│ - Timestamp: uint32 ms (4 bytes)        │
│ - Type: 0x01 (1 byte)                   │
│ - Size: uint32 (4 bytes)                │
│ - Data: JPEG frame (size bytes)         │
└─────────────────────────────────────────┘
┌─────────────────────────────────────────┐
│ Chunk 2: Audio Chunk                    │
│ - Timestamp: uint32 ms (4 bytes)        │
│ - Type: 0x02 (1 byte)                   │
│ - Size: uint32 (4 bytes)                │
│ - Data: PCM samples (size bytes)        │
└─────────────────────────────────────────┘
... (interleaved chunks) ...
```

## ESPAV Conversion tool

A Python tool to convert `.espav` files to standard formats
[/tools/espav_converter.py](/tools/espav_converter.py)

