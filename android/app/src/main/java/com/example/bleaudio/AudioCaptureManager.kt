package com.example.bleaudio

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.util.Log
import kotlinx.coroutines.*
import java.util.concurrent.LinkedBlockingQueue
import android.media.AudioAttributes

class AudioCaptureManager(
    private val onAudioData: (ByteArray, Int) -> Unit,
    private val onError: (String) -> Unit
) {
    companion object {
        private const val TAG = "AudioCaptureManager"
        private const val SAMPLE_RATE = 16000
        private const val CHANNEL_CONFIG = AudioFormat.CHANNEL_IN_MONO
        private const val AUDIO_FORMAT = AudioFormat.ENCODING_PCM_16BIT // REVERT to 16-BIT PCM
        // Buffer size needs to be larger to send bigger packets less frequently
        private const val FRAME_SIZE = 200  // Read 200 samples (12.5ms) at a time
        private const val PACKET_SIZE_BYTES = 200 // 200 samples compressed to 200 u-law bytes
    }

    private var audioRecord: AudioRecord? = null
    private var audioTrack: AudioTrack? = null
    private var isRecording = false
    private var isPlaybackActive = false
    private val playbackBuffer = LinkedBlockingQueue<ByteArray>()

    private var recordingJob: Job? = null
    private var playbackJob: Job? = null

    private fun initializeRecording(): Boolean {
        try {
            val minBufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL_CONFIG, AUDIO_FORMAT)
            audioRecord = AudioRecord(
                MediaRecorder.AudioSource.MIC,
                SAMPLE_RATE,
                CHANNEL_CONFIG,
                AUDIO_FORMAT,
                minBufferSize * 2
            )
            if (audioRecord?.state != AudioRecord.STATE_INITIALIZED) {
                Log.e(TAG, "AudioRecord initialization failed.")
                return false
            }
            Log.d(TAG, "AudioRecord initialized, buffer size: ${minBufferSize * 2} bytes")
            return true
        } catch (e: Exception) {
            Log.e(TAG, "Exception during AudioRecord initialization", e)
            return false
        }
    }

    private fun initializePlayback(): Boolean {
        try {
            val minBufferSize = AudioTrack.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_OUT_MONO, AUDIO_FORMAT)
            val attributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build()
            val format = AudioFormat.Builder()
                .setEncoding(AUDIO_FORMAT)
                .setSampleRate(SAMPLE_RATE)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                .build()

            audioTrack = AudioTrack.Builder()
                .setAudioAttributes(attributes)
                .setAudioFormat(format)
                .setBufferSizeInBytes(minBufferSize * 4)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            
            if (audioTrack?.state != AudioTrack.STATE_INITIALIZED) {
                Log.e(TAG, "AudioTrack initialization failed.")
                return false
            }
            Log.d(TAG, "AudioTrack initialized, buffer size: ${minBufferSize * 4} bytes")
            return true
        } catch (e: Exception) {
            Log.e(TAG, "Exception during AudioTrack initialization", e)
            return false
        }
    }

    fun startRecording() {
        if (isRecording) {
            Log.w(TAG, "Recording is already in progress")
            return
        }

        // Reset state
        playbackBuffer.clear()
        isRecording = true

        try {
            // Start processing in background, but run the priming loop first.
            recordingJob = CoroutineScope(Dispatchers.IO).launch {
                if (!initializeRecording() || !initializePlayback()) {
                    Log.e(TAG, "Failed to initialize audio components")
                    onError("Audio init failed")
                    return@launch
                }

                audioRecord?.startRecording()
                audioTrack?.play()
                Log.d(TAG, "Audio recording and playback started")

                val primeBuffer = ShortArray(FRAME_SIZE)
                var hasRealAudio = false
                for (i in 1..10) { // Try up to 10 times
                    val readSize = audioRecord?.read(primeBuffer, 0, primeBuffer.size) ?: 0
                    if (readSize > 0 && primeBuffer.any { it != 0.toShort() }) {
                        Log.d(TAG, "Microphone is live and delivering audio data after $i attempts.")
                        hasRealAudio = true
                        break
                    }
                    delay(20) // Non-blocking delay
                }

                if (!hasRealAudio) {
                    Log.e(TAG, "Microphone failed to deliver audio data after multiple attempts.")
                    onError("Mic failed to start")
                    // Clean up resources before exiting
                    stopAudioComponents()
                    return@launch
                }

                unifiedAudioLoop()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error starting recording", e)
            onError("Failed to start recording: ${e.message}")
        }
    }

    fun stopRecording() {
        Log.d(TAG, "Stop recording signal received by manager.")
        isRecording = false
    }

    private fun stopAudioComponents() {
        Log.d(TAG, "Stopping and releasing audio components...")
        
        recordingJob?.cancel() // Cancel the coroutine
        recordingJob = null

        audioRecord?.apply {
            if (recordingState == AudioRecord.RECORDSTATE_RECORDING) {
                stop()
            }
            release()
        }
        audioRecord = null
        Log.d(TAG, "AudioRecord released.")

        audioTrack?.apply {
            if (playState == AudioTrack.PLAYSTATE_PLAYING) {
                // Flush might be better than stop to clear pending data without abrupt stop
                flush()
                stop()
            }
            release()
        }
        audioTrack = null
        Log.d(TAG, "AudioTrack released.")
        
        playbackBuffer.clear()
        isPlaybackActive = false
    }


    private suspend fun unifiedAudioLoop() {
        Log.d(TAG, "Unified audio loop started.")
        val pcmBuffer = ShortArray(FRAME_SIZE)
        
        // Pre-buffering phase
        Log.d(TAG, "Pre-buffering to ensure smooth start...")
        val preBufferStartTime = System.currentTimeMillis()
        while (System.currentTimeMillis() - preBufferStartTime < 500) {
            val readResult = audioRecord?.read(pcmBuffer, 0, FRAME_SIZE, AudioRecord.READ_NON_BLOCKING) ?: 0
            if (readResult > 0) {
                val encoded = MuLawCodec.encode(pcmBuffer, readResult)
                onAudioData(encoded, encoded.size)
            }
            // Small delay to prevent a tight loop from starving other processes
            delay(10) 
        }
        Log.d(TAG, "Pre-buffering complete.")

        // Main full-duplex loop
        while (isRecording) { // The loop should run as long as we are supposed to be recording
            // 1. Capture audio
            val readResult = audioRecord?.read(pcmBuffer, 0, FRAME_SIZE, AudioRecord.READ_NON_BLOCKING) ?: 0
            if (readResult > 0) {
                Log.d(TAG, "MIC CAPTURE: ${pcmBuffer.take(8).joinToString()}")
                val encoded = MuLawCodec.encode(pcmBuffer, readResult)
                Log.d(TAG, "ENCODED SEND: ${encoded.take(8).joinToString { "0x%02X".format(it) }}")
                onAudioData(encoded, encoded.size)
            } else if (readResult < 0) {
                Log.w(TAG, "AudioRecord read error: $readResult")
            }

            // 2. Playback received audio
            val data = playbackBuffer.poll() // Use non-blocking poll
            if (data != null) {
                Log.d(TAG, "DECODING RECV: ${data.take(8).joinToString { "0x%02X".format(it) }}")
                val decoded = MuLawCodec.decode(data, data.size)
                Log.d(TAG, "PLAYING DECODED: ${decoded.take(8).joinToString()}")
                if (decoded.isNotEmpty()) {
                    val written = audioTrack?.write(decoded, 0, decoded.size) ?: 0
                    if (written < 0) {
                         Log.w(TAG, "AudioTrack write error: $written")
                    }
                }
            }
            
            // If neither reading nor playing, yield to avoid busy-waiting
            if (readResult <= 0 && data == null) {
                delay(5)
            }
        }
        
        Log.d(TAG, "Recording flag is false. Draining playback buffer...")

        // Drain the remaining playback buffer
        while (playbackBuffer.isNotEmpty()) {
            val data = playbackBuffer.poll()
            if (data != null) {
                val decoded = MuLawCodec.decode(data, data.size)
                if (decoded.isNotEmpty()) {
                    audioTrack?.write(decoded, 0, decoded.size)
                }
            }
        }
        
        Log.d(TAG, "Buffer drained. Releasing audio components.")
        stopAudioComponents()
        Log.d(TAG, "Unified audio loop finished.")
    }

    fun playReceivedAudio(data: ByteArray, size: Int) {
        if (size > 0) {
            val packet = data.copyOf(size)
            // Log this outside the hot path of the audio loop for clarity
            // Log.d(TAG, "RECEIVED ECHO: ${packet.take(8).joinToString { "0x%02X".format(it) }}")
            playbackBuffer.offer(packet)
            // If not recording, ensure a playback-only loop is running to drain the buffer
            if (!isRecording) {
                startPlaybackOnlyIfNeeded()
            }
        }
    }

    private fun startPlaybackOnlyIfNeeded() {
        if (isPlaybackActive) return
        try {
            if (audioTrack == null) {
                if (!initializePlayback()) {
                    Log.e(TAG, "Failed to initialize playback for playback-only mode")
                    return
                }
            }
            audioTrack?.play()
            isPlaybackActive = true
            playbackJob = CoroutineScope(Dispatchers.IO).launch {
                Log.d(TAG, "Playback-only loop started")
                while (isPlaybackActive) {
                    val data = playbackBuffer.poll()
                    if (data != null) {
                        val decoded = MuLawCodec.decode(data, data.size)
                        if (decoded.isNotEmpty()) {
                            val written = audioTrack?.write(decoded, 0, decoded.size) ?: 0
                            if (written < 0) {
                                Log.w(TAG, "AudioTrack write error (playback-only): $written")
                            }
                        }
                    } else {
                        // No data; avoid busy spin
                        delay(5)
                    }
                }
                Log.d(TAG, "Playback-only loop stopped")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error starting playback-only loop", e)
        }
    }
    
    private fun handleAudioRecordError(errorCode: Int) {
        when (errorCode) {
            AudioRecord.ERROR_INVALID_OPERATION -> Log.e(TAG, "AudioRecord Error: Invalid operation.")
            AudioRecord.ERROR_BAD_VALUE -> Log.e(TAG, "AudioRecord Error: Bad value. Check parameters.")
            AudioRecord.ERROR_DEAD_OBJECT -> Log.e(TAG, "AudioRecord Error: Dead object. Media server died.")
            AudioRecord.ERROR -> Log.e(TAG, "AudioRecord Error: Generic error.")
            0 -> Log.v(TAG, "Mic read 0 bytes, likely just no speech.") // Not an error, just silence
            else -> Log.e(TAG, "AudioRecord Error: Unknown error code $errorCode")
        }
    }

    // Generate a 32-byte test pattern that will produce audible sound when repeated
    private fun generateTestPattern(counter: Int): ByteArray {
        val pattern = ByteArray(32) // Back to 32-byte pattern for nRF compatibility
        
        // Generate a test pattern that will be audible when repeated
        // This will be 16-bit PCM samples (little-endian)
        
        // Start with a recognizable pattern
        pattern[0] = 0xAA.toByte()  // Start marker (low byte)
        pattern[1] = 0x55.toByte()  // Start marker (high byte)
        pattern[2] = (counter and 0xFF).toByte()  // Counter (low byte)
        pattern[3] = 0x00.toByte()  // Counter (high byte)
        
        // Fill with a sine wave pattern (16-bit samples)
        // Generate a 500Hz tone that will be clearly audible when repeated
        for (i in 4 until 32 step 2) {
            val sampleIndex = (i - 4) / 2
            // Generate 500Hz sine wave: sin(2π * 500Hz * sampleIndex / 16000Hz)
            val sample = (Math.sin(2.0 * Math.PI * 500.0 * sampleIndex / 16000.0) * 15000.0).toInt().toShort()
            pattern[i] = (sample.toInt() and 0xFF).toByte()     // Low byte
            pattern[i + 1] = (sample.toInt() shr 8).toByte()    // High byte
        }
        
        return pattern
    }

    // Raw 16-bit audio - no compression, no processing
    private fun compressSimple(samples: ShortArray, count: Int, compressed: ByteArray): Int {
        var compressedIndex = 0
        
        for (i in 0 until count) {
            if (compressedIndex + 1 < compressed.size) {
                // Send raw 16-bit samples (little-endian)
                val sample = samples[i]
                compressed[compressedIndex++] = (sample.toInt() and 0xFF).toByte() // Low byte
                compressed[compressedIndex++] = (sample.toInt() shr 8).toByte()   // High byte
            }
        }
        
        return compressedIndex
    }



    // Raw 16-bit audio - no decompression, no processing
    private fun decompressSimple(compressed: ByteArray, size: Int, samples: ShortArray): Int {
        var sampleIndex = 0
        
        for (i in 0 until size step 2) {
            if (sampleIndex < samples.size && i + 1 < size) {
                // Reconstruct 16-bit samples from little-endian bytes
                val lowByte = compressed[i].toInt() and 0xFF
                val highByte = compressed[i + 1].toInt() and 0xFF
                val sample = (lowByte or (highByte shl 8)).toShort()
                samples[sampleIndex++] = sample
            }
        }
        
        return sampleIndex
    }
    


    fun playTestTone() {
        if (audioTrack == null) {
            Log.e(TAG, "AudioTrack not initialized, cannot play test tone.")
            return
        }
        if (audioTrack?.playState == AudioTrack.PLAYSTATE_PLAYING) {
            Log.w(TAG, "AudioTrack is already playing.")
            return
        }
        
        // This function is now obsolete as the generator was removed.
        // It can be removed entirely.
    }

    fun release() {
        try {
            stopRecording()
            audioTrack?.stop()
            audioTrack?.release()
            audioTrack = null
            Log.d(TAG, "AudioCaptureManager released")
        } catch (e: Exception) {
            Log.e(TAG, "Error releasing AudioCaptureManager", e)
        }
    }
}
