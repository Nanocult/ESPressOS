#!/usr/bin/env python3
"""
espav_converter.py - Convert ESPAV files to AVI/MKV
Usage: python espav_converter.py input.espav output.avi

TODO: 
- merge smaller consequitive chunks in one file
- check the SD card free space for saving
"""

import struct
import sys
from pathlib import Path

def read_espav(input_path):
    with open(input_path, 'rb') as f:
        # Read header
        magic = f.read(4)
        if magic != b'ESPAV':
            raise ValueError("Invalid ESPAV file")
        
        version = struct.unpack('B', f.read(1))[0]
        video_fps = struct.unpack('B', f.read(1))[0]
        audio_sample_rate = struct.unpack('<I', f.read(4))[0]
        audio_channels = struct.unpack('B', f.read(1))[0]
        audio_bits = struct.unpack('B', f.read(1))[0]
        f.read(20)  # Skip reserved
        
        print(f"ESPAV v{version}")
        print(f"Video: {video_fps} fps")
        print(f"Audio: {audio_sample_rate}Hz, {audio_channels}ch, {audio_bits}-bit")
        
        # Read chunks
        video_frames = []
        audio_chunks = []
        
        while True:
            chunk_header = f.read(9)
            if len(chunk_header) < 9:
                break
            
            timestamp_ms, chunk_type, size = struct.unpack('<IBI', chunk_header)
            data = f.read(size)
            
            if chunk_type == 0x01:  # Video
                video_frames.append((timestamp_ms, data))
            elif chunk_type == 0x02:  # Audio
                audio_chunks.append((timestamp_ms, data))
        
        print(f"Extracted {len(video_frames)} video frames")
        print(f"Extracted {len(audio_chunks)} audio chunks")
        
        return video_frames, audio_chunks, video_fps, audio_sample_rate, audio_channels

def save_separate_files(video_frames, audio_chunks, output_base):
    # Save video as MJPEG
    with open(f"{output_base}.mjpeg", 'wb') as f:
        for _, frame in video_frames:
            f.write(frame)
    
    # Save audio as WAV
    import wave
    with wave.open(f"{output_base}.wav", 'wb') as wav:
        wav.setnchannels(1)  # Assume mono for now
        wav.setsampwidth(2)  # 16-bit
        wav.setframerate(16000)  # Assume 16kHz for now
        
        for _, chunk in audio_chunks:
            wav.writeframes(chunk)
    
    print(f"Saved: {output_base}.mjpeg and {output_base}.wav")
    print(f"Use ffmpeg to combine: ffmpeg -i {output_base}.mjpeg -i {output_base}.wav -c:v copy -c:a aac {output_base}.mp4")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("Usage: python espav_converter.py input.espav output_base")
        sys.exit(1)
    
    input_path = sys.argv[1]
    output_base = sys.argv[2]
    
    video_frames, audio_chunks, fps, sr, ch = read_espav(input_path)
    save_separate_files(video_frames, audio_chunks, output_base)
