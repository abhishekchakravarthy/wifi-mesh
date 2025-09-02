package com.example.bleaudio

import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Opus decoder implementation for Android
 * Uses the Opus library for high-quality audio decompression
 */
class OpusDecoder {
    companion object {
        private const val TAG = "OpusDecoder"
        private const val SAMPLE_RATE = 16000
        private const val CHANNELS = 1
        private const val FRAME_SIZE = 160 // 10ms at 16kHz
    }

    private var decoder: Long = 0 // Native Opus decoder pointer
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
            decoder = nativeCreateDecoder(SAMPLE_RATE, CHANNELS)
            if (decoder != 0L) {
                isInitialized = true
                Log.d(TAG, "Opus decoder initialized: ${SAMPLE_RATE}Hz, ${CHANNELS} channel")
                return true
            } else {
                Log.e(TAG, "Failed to create Opus decoder")
                return false
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error initializing Opus decoder", e)
            return false
        }
    }

    fun decode(encoded: ByteArray, offset: Int, encodedSize: Int, pcm: ShortArray, pcmOffset: Int, frameSize: Int): Int {
        if (!isInitialized || decoder == 0L) {
            Log.e(TAG, "Decoder not initialized")
            return -1
        }

        return try {
            nativeDecode(decoder, encoded, offset, encodedSize, pcm, pcmOffset, frameSize)
        } catch (e: Exception) {
            Log.e(TAG, "Error decoding audio", e)
            -1
        }
    }

    fun release() {
        if (decoder != 0L) {
            try {
                nativeDestroyDecoder(decoder)
                decoder = 0L
                isInitialized = false
                Log.d(TAG, "Opus decoder released")
            } catch (e: Exception) {
                Log.e(TAG, "Error releasing Opus decoder", e)
            }
        }
    }

    fun isInitialized(): Boolean = isInitialized

    // Native methods
    private external fun nativeCreateDecoder(sampleRate: Int, channels: Int): Long
    private external fun nativeDecode(decoder: Long, encoded: ByteArray, offset: Int, encodedSize: Int, pcm: ShortArray, pcmOffset: Int, frameSize: Int): Int
    private external fun nativeDestroyDecoder(decoder: Long)
}
