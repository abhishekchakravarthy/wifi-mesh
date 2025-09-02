package com.example.bleaudio

import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Opus encoder implementation for Android
 * Uses the Opus library for high-quality audio compression
 */
class OpusEncoder {
    companion object {
        private const val TAG = "OpusEncoder"
        private const val SAMPLE_RATE = 16000
        private const val CHANNELS = 1
        private const val FRAME_SIZE = 160 // 10ms at 16kHz
        private const val BITRATE = 16000 // 16kbps for voice quality
        private const val MAX_FRAME_SIZE = 1275 // Maximum Opus frame size
    }

    private var encoder: Long = 0 // Native Opus encoder pointer
    private var isInitialized = false

    init {
        System.loadLibrary("opus")
        initialize()
    }

    fun initialize(): Boolean {
        if (isInitialized) {
            return true
        }

        try {
            encoder = nativeCreateEncoder(SAMPLE_RATE, CHANNELS, BITRATE)
            if (encoder != 0L) {
                isInitialized = true
                Log.d(TAG, "Opus encoder initialized: ${SAMPLE_RATE}Hz, ${CHANNELS} channel, ${BITRATE}bps")
                return true
            } else {
                Log.e(TAG, "Failed to create Opus encoder")
                return false
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error initializing Opus encoder", e)
            return false
        }
    }

    fun encode(pcm: ShortArray, offset: Int, frameSize: Int, encoded: ByteArray, encodedOffset: Int, maxEncodedSize: Int): Int {
        if (!isInitialized || encoder == 0L) {
            Log.e(TAG, "Encoder not initialized")
            return -1
        }

        return try {
            nativeEncode(encoder, pcm, offset, frameSize, encoded, encodedOffset, maxEncodedSize)
        } catch (e: Exception) {
            Log.e(TAG, "Error encoding audio", e)
            -1
        }
    }

    fun release() {
        if (encoder != 0L) {
            try {
                nativeDestroyEncoder(encoder)
                encoder = 0L
                isInitialized = false
                Log.d(TAG, "Opus encoder released")
            } catch (e: Exception) {
                Log.e(TAG, "Error releasing Opus encoder", e)
            }
        }
    }

    fun isInitialized(): Boolean = isInitialized

    // Native methods
    private external fun nativeCreateEncoder(sampleRate: Int, channels: Int, bitrate: Int): Long
    private external fun nativeEncode(encoder: Long, pcm: ShortArray, offset: Int, frameSize: Int, encoded: ByteArray, encodedOffset: Int, maxEncodedSize: Int): Int
    private external fun nativeDestroyEncoder(encoder: Long)
}
