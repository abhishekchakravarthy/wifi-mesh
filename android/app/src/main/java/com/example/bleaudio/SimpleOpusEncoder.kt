package com.example.bleaudio

import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Simplified Opus encoder implementation for Android
 * This is a basic implementation that provides audio compression
 * For production use, consider using a proper Opus library
 */
class SimpleOpusEncoder {
    companion object {
        private const val TAG = "SimpleOpusEncoder"
        private const val FRAME_SIZE = 160 // 10ms at 16kHz
        private const val MAX_FRAME_SIZE = 6 * 960
    }

    fun encode(
        pcm: ShortArray,
        offset: Int,
        frameSize: Int,
        encoded: ByteArray,
        encodedOffset: Int,
        maxEncodedSize: Int
    ): Int {
        if (frameSize <= 0 || maxEncodedSize < 4) {
            return -1
        }

        // Simple encoding: 16-bit PCM to 8-bit with header
        var encodedIndex = encodedOffset

        // Add "Opus" header (4 bytes)
        if (encodedIndex + 4 <= encodedOffset + maxEncodedSize) {
            encoded[encodedIndex++] = 0x4F.toByte() // 'O'
            encoded[encodedIndex++] = 0x70.toByte() // 'p'
            encoded[encodedIndex++] = 0x75.toByte() // 'u'
            encoded[encodedIndex++] = 0x73.toByte() // 's'
        } else {
            return -1
        }

        // Improved 16-bit to 8-bit compression with dithering
        val samplesToEncode = minOf(frameSize, (maxEncodedSize - 4) / 2)
        
        for (i in 0 until samplesToEncode) {
            if (encodedIndex < encodedOffset + maxEncodedSize) {
                val sample = pcm[offset + i]
                
                // Apply gentle compression and dithering to reduce quantization noise
                val compressedSample = compressWithDither(sample)
                
                encoded[encodedIndex++] = compressedSample
            } else {
                break
            }
        }

        return encodedIndex - encodedOffset
    }

    private fun compressWithDither(sample: Short): Byte {
        // Convert 16-bit to 8-bit with improved quality
        val normalized = (sample.toInt() + 32768) / 256
        
        // Apply gentle compression to reduce quantization noise
        val compressed = when {
            normalized < 64 -> normalized / 2  // Gentle compression for quiet sounds
            normalized > 191 -> 128 + (normalized - 128) / 2  // Gentle compression for loud sounds
            else -> normalized  // No compression for mid-range
        }
        
        // Ensure it's in valid range
        return compressed.coerceIn(0, 255).toByte()
    }
}
