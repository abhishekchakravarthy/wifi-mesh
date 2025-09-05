#!/usr/bin/env python3
"""
Generate Test Tone for ESP32 Audio Testing

Creates a simple 1kHz sine wave test tone that will be clearly audible
when played through the ESP32 audio system.
"""

import math
import struct
import argparse

# ESP32 Audio Configuration
SAMPLE_RATE = 16000  # 16kHz
DURATION = 2.0       # 2 seconds
FREQUENCY = 1000     # 1kHz tone
CHUNK_SIZE = 200     # 200 bytes per chunk (100 samples * 2 bytes)

def generate_sine_wave(frequency, duration, sample_rate):
    """Generate a sine wave as 16-bit PCM data"""
    num_samples = int(sample_rate * duration)
    samples = []
    
    for i in range(num_samples):
        # Generate sine wave
        t = i / sample_rate
        amplitude = 0.5  # 50% amplitude to avoid clipping
        sample = amplitude * math.sin(2 * math.pi * frequency * t)
        
        # Convert to 16-bit signed integer
        sample_int = int(sample * 32767)
        samples.append(sample_int)
    
    return samples

def samples_to_bytes(samples):
    """Convert 16-bit samples to little-endian bytes"""
    bytes_data = bytearray()
    for sample in samples:
        # Convert to little-endian 16-bit
        bytes_data.extend(struct.pack('<h', sample))
    return bytes(bytes_data)

def create_chunks(data, chunk_size):
    """Split data into chunks"""
    chunks = []
    for i in range(0, len(data), chunk_size):
        chunk = data[i:i + chunk_size]
        chunks.append(chunk)
    return chunks

def create_esp32_commands(chunks):
    """Create ESP32 serial commands for the chunks"""
    commands = []
    for i, chunk in enumerate(chunks):
        # Convert chunk to hex string
        hex_data = chunk.hex().upper()
        command = f"send_audio_chunk:{i}:{hex_data}"
        commands.append(command)
    return commands

def main():
    parser = argparse.ArgumentParser(description='Generate test tone for ESP32 audio testing')
    parser.add_argument('-f', '--frequency', type=int, default=FREQUENCY, 
                       help=f'Frequency in Hz (default: {FREQUENCY})')
    parser.add_argument('-d', '--duration', type=float, default=DURATION,
                       help=f'Duration in seconds (default: {DURATION})')
    parser.add_argument('-o', '--output', default='test_tone.bin',
                       help='Output binary file (default: test_tone.bin)')
    parser.add_argument('--commands', default='test_tone_commands.txt',
                       help='Output ESP32 commands file (default: test_tone_commands.txt)')
    
    args = parser.parse_args()
    
    print(f"🎵 Generating {args.frequency}Hz test tone for {args.duration}s")
    print(f"   Sample rate: {SAMPLE_RATE}Hz")
    print(f"   Chunk size: {CHUNK_SIZE} bytes")
    
    # Generate sine wave
    samples = generate_sine_wave(args.frequency, args.duration, SAMPLE_RATE)
    print(f"   Generated {len(samples)} samples")
    
    # Convert to bytes
    audio_data = samples_to_bytes(samples)
    print(f"   Converted to {len(audio_data)} bytes")
    
    # Save binary file
    with open(args.output, 'wb') as f:
        f.write(audio_data)
    print(f"💾 Saved binary data to: {args.output}")
    
    # Create chunks
    chunks = create_chunks(audio_data, CHUNK_SIZE)
    print(f"📦 Split into {len(chunks)} chunks")
    
    # Create ESP32 commands
    commands = create_esp32_commands(chunks)
    
    # Save commands
    with open(args.commands, 'w') as f:
        for command in commands:
            f.write(command + '\n')
    print(f"💾 Saved ESP32 commands to: {args.commands}")
    
    # Show preview
    print(f"\n🔍 Preview of first 3 chunks:")
    for i, chunk in enumerate(chunks[:3]):
        hex_preview = chunk.hex().upper()[:32] + "..." if len(chunk) > 16 else chunk.hex().upper()
        print(f"   Chunk {i+1}: {len(chunk)} bytes - {hex_preview}")
    
    print(f"\n✅ Test tone generation completed!")
    print(f"📊 Summary:")
    print(f"   Frequency: {args.frequency}Hz")
    print(f"   Duration: {args.duration}s")
    print(f"   Samples: {len(samples)}")
    print(f"   Bytes: {len(audio_data)}")
    print(f"   Chunks: {len(chunks)}")
    print(f"\n🚀 To test:")
    print(f"   python3 test_ble_echo.py")

if __name__ == '__main__':
    main()
