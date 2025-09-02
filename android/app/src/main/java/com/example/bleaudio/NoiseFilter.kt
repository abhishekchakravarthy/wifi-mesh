package com.example.bleaudio

import android.util.Log

/**
 * Audio noise filter implementation
 * Provides high-pass filtering and basic noise reduction
 */
class NoiseFilter {
    companion object {
        private const val TAG = "NoiseFilter"
        private const val NOISE_GATE_THRESHOLD = 500
        private const val COMPRESSION_RATIO = 0.7f
        private const val HIGH_PASS_FREQ = 80.0f // Hz
    }

    private var lastSample = 0.0f
    private var highPassAlpha = 0.0f

    init {
        // Calculate high-pass filter coefficient
        val rc = 1.0f / (2.0f * Math.PI.toFloat() * HIGH_PASS_FREQ)
        highPassAlpha = rc / (rc + 1.0f / 16000.0f) // 16kHz sample rate
    }

    fun applyFilter(input: ShortArray): ShortArray {
        val output = ShortArray(input.size)
        
        for (i in input.indices) {
            var sample = input[i].toFloat()
            
            // Apply high-pass filter
            sample = applyHighPassFilter(sample)
            
            // Apply noise gate
            sample = applyNoiseGate(sample)
            
            // Apply compression
            sample = applyCompression(sample)
            
            output[i] = sample.toInt().toShort()
        }
        
        return output
    }

    private fun applyHighPassFilter(sample: Float): Float {
        val filtered = highPassAlpha * (lastSample + sample - lastSample)
        lastSample = sample
        return filtered
    }

    private fun applyNoiseGate(sample: Float): Float {
        return if (kotlin.math.abs(sample) < NOISE_GATE_THRESHOLD) {
            0.0f
        } else {
            sample
        }
    }

    private fun applyCompression(sample: Float): Float {
        val absSample = kotlin.math.abs(sample)
        if (absSample > 1000) {
            val compression = 1000 + (absSample - 1000) * COMPRESSION_RATIO
            return if (sample > 0) compression else -compression
        }
        return sample
    }
}
