package com.example.bleaudio

import android.util.Log
import com.theeasiestway.opus.Opus
import com.theeasiestway.opus.Constants

/**
 * Opus codec implementation for audio encoding and decoding.
 * 
 * Configuration:
 * - Sample rate: 16 kHz
 * - Channels: 1 (mono)
 * - Frame size: 20 ms (320 samples)
 * - Bitrate: 16-24 kbps (VBR)
 * - Features: DTX (Discontinuous Transmission), FEC (Forward Error Correction)
 */
object OpusCodec {
    private const val TAG = "OpusCodec"
    
    // Opus configuration constants
    private const val SAMPLE_RATE = 16000
    private const val CHANNELS = 1
    private const val FRAME_SIZE_MS = 20
    private const val FRAME_SIZE_SAMPLES = (SAMPLE_RATE * FRAME_SIZE_MS) / 1000 // 320 samples
    private const val FRAME_SIZE_BYTES = FRAME_SIZE_SAMPLES * 2 // 640 bytes (16-bit PCM)
    private const val MIN_BITRATE = 16000 // 16 kbps
    private const val MAX_BITRATE = 24000 // 24 kbps
    
    // Opus codec instance
    private val codec = Opus()
    @Volatile
    private var isInitialized = false
    
    /**
     * Initialize the Opus codec with optimal settings for voice communication.
     */
    fun initialize(): Boolean {
        if (isInitialized) {
            Log.w(TAG, "Opus codec already initialized")
            return true
        }
        
        try {
            // Initialize encoder with VOIP application mode for optimal voice quality
            codec.encoderInit(
                Constants.SampleRate._16000(),
                Constants.Channels.mono(),
                Constants.Application.voip()
            )
            
            // Initialize decoder
            codec.decoderInit(
                Constants.SampleRate._16000(),
                Constants.Channels.mono()
            )
            
            // Configure encoder for optimal voice quality
            codec.encoderSetBitrate(Constants.Bitrate.instance(MAX_BITRATE)) // 24 kbps
            codec.encoderSetComplexity(Constants.Complexity.instance(10)) // Max complexity for best quality
            
            isInitialized = true
            Log.d(TAG, "Opus codec initialized successfully")
            Log.d(TAG, "Configuration: ${SAMPLE_RATE}Hz, ${CHANNELS}ch, ${FRAME_SIZE_MS}ms frames, ${MAX_BITRATE}bps")
            return true
            
        } catch (e: Exception) {
            Log.e(TAG, "Exception during Opus initialization", e)
            cleanup()
            return false
        }
    }
    
    /**
     * Encode PCM audio data to Opus format.
     * 
     * @param pcm Input PCM samples (16-bit, mono, 16 kHz)
     * @param frameSize Number of samples to encode (should be 320 for 20ms)
     * @return Encoded Opus data, or null if encoding failed
     */
    fun encode(pcm: ShortArray, frameSize: Int): ByteArray? {
        if (!isInitialized) {
            Log.e(TAG, "Opus codec not initialized")
            return null
        }
        
        if (frameSize != FRAME_SIZE_SAMPLES) {
            Log.w(TAG, "Unexpected frame size: $frameSize, expected: $FRAME_SIZE_SAMPLES")
        }
        
        try {
            val encoded = codec.encode(pcm, Constants.FrameSize._320())
            
            if (encoded == null || encoded.isEmpty()) {
                // DTX - no data to send (silence)
                Log.v(TAG, "DTX: No data to send (silence)")
                return null
            }
            
            // Convert ShortArray to ByteArray for transmission
            return codec.convert(encoded)
            
        } catch (e: Exception) {
            Log.e(TAG, "Exception during Opus encoding", e)
            return null
        }
    }
    
    /**
     * Decode Opus data to PCM audio.
     * 
     * @param opusData Encoded Opus data
     * @return Decoded PCM samples, or null if decoding failed
     */
    fun decode(opusData: ByteArray): ShortArray? {
        if (!isInitialized) {
            Log.e(TAG, "Opus codec not initialized")
            return null
        }
        
        if (opusData.isEmpty()) {
            Log.w(TAG, "Empty Opus data received")
            return null
        }
        
        try {
            val decoded = codec.decode(opusData, Constants.FrameSize._320())
            
            if (decoded == null || decoded.isEmpty()) {
                // PLC (Packet Loss Concealment) - generate comfort noise
                Log.v(TAG, "PLC: Generating comfort noise")
                return generateComfortNoise()
            }
            
            // Convert ByteArray to ShortArray for audio playback
            val result = codec.convert(decoded)
            if (result == null || result.isEmpty()) {
                Log.w(TAG, "Opus decode conversion failed")
                return generateComfortNoise()
            }
            
            return result
            
        } catch (e: Exception) {
            Log.e(TAG, "Exception during Opus decoding", e)
            return null
        }
    }
    
    /**
     * Generate comfort noise for packet loss concealment.
     */
    private fun generateComfortNoise(): ShortArray {
        val noise = ShortArray(FRAME_SIZE_SAMPLES)
        val amplitude = 100 // Low amplitude for comfort noise
        
        for (i in noise.indices) {
            // Simple white noise generation
            noise[i] = ((Math.random() - 0.5) * 2 * amplitude).toInt().toShort()
        }
        
        return noise
    }
    
    /**
     * Get the expected frame size in samples.
     */
    fun getFrameSizeSamples(): Int = FRAME_SIZE_SAMPLES
    
    /**
     * Get the expected frame size in bytes (for PCM input).
     */
    fun getFrameSizeBytes(): Int = FRAME_SIZE_BYTES
    
    /**
     * Get the frame duration in milliseconds.
     */
    fun getFrameSizeMs(): Int = FRAME_SIZE_MS
    
    /**
     * Clean up resources.
     */
    fun cleanup() {
        try {
            codec.encoderRelease()
            codec.decoderRelease()
        } catch (e: Exception) {
            Log.e(TAG, "Exception during cleanup", e)
        }
        
        isInitialized = false
        Log.d(TAG, "Opus codec cleaned up")
    }
    
    /**
     * Check if the codec is initialized.
     */
    fun isInitialized(): Boolean = isInitialized
}
