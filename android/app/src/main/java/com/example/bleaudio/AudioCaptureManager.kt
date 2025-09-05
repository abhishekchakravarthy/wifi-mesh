package com.example.bleaudio

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.util.Log
import kotlinx.coroutines.*

class AudioCaptureManager(
    private val onAudioData: (ByteArray, Int) -> Unit,
    private val onError: (String) -> Unit
) {
    companion object {
        private const val TAG = "AudioCaptureManager"
        private const val SAMPLE_RATE = 16000
        private const val CHANNEL_CONFIG = AudioFormat.CHANNEL_IN_MONO
        private const val AUDIO_FORMAT = AudioFormat.ENCODING_PCM_16BIT
        private const val FRAME_SIZE = 100  // 6.25ms at 16kHz (matches ESP32 chunk size)
        private const val COMPRESSED_FRAME_SIZE = 200  // 100 samples * 2 bytes = 200 bytes
        private const val PREBUFFER_BYTES = 1280 // 40ms of 16-bit mono
        
            // Direct playback - no buffering for minimal latency
    }

    private var audioRecord: AudioRecord? = null
    private var audioTrack: AudioTrack? = null
    private var isRecording = false
    private var recordingJob: Job? = null
    
    // Audio buffers - increased for better performance
    private val audioBuffer = ShortArray(FRAME_SIZE * 2)  // Larger buffer
    private val compressedBuffer = ByteArray(COMPRESSED_FRAME_SIZE * 2)
    private val decodedBuffer = ShortArray(FRAME_SIZE * 2)
    
    // Stream synchronization for smooth audio
    private val streamBuffer = ByteArray(2048)  // Back to 2KB buffer
    private var streamBufferSize = 0
    private var streamBufferIndex = 0
    private val streamLock = Object()
    private var isStreamStarted = false
    private var lastPacketTime = 0L
    private var lastReceivedSequence = 0  // Track last received sequence for loss detection
    private var consecutivePacketLoss = 0  // Count consecutive lost packets
    private var bufferPrimed = false  // Track if buffer is ready for optimal processing
    
    // Audio playback buffer for received chunks - much larger buffer
    private val playbackBuffer = ByteArray(4096)  // 4KB buffer for received audio
    private var playbackBufferSize = 0
    private var playbackBufferIndex = 0
    private val playbackLock = Object()
    
    // Chunked audio buffer for reassembling audio data
    private val chunkedAudioBuffer = ByteArray(1024)  // Buffer for reassembling chunks
    private var chunkedAudioBufferSize = 0
    private var lastChunkTime = 0L
    private val chunkTimeout = 50L // 50ms timeout for chunks

    fun startRecording() {
        if (isRecording) {
            Log.w(TAG, "Recording already in progress")
            return
        }

        try {
            // Calculate minimum buffer size
            val minBufferSize = AudioRecord.getMinBufferSize(
                SAMPLE_RATE, 
                CHANNEL_CONFIG, 
                AUDIO_FORMAT
            )

            if (minBufferSize == AudioRecord.ERROR_BAD_VALUE || minBufferSize == AudioRecord.ERROR) {
                onError("Invalid audio configuration")
                return
            }

            // Create AudioRecord with larger buffer for better performance
            audioRecord = AudioRecord(
                MediaRecorder.AudioSource.MIC,
                SAMPLE_RATE,
                CHANNEL_CONFIG,
                AUDIO_FORMAT,
                minBufferSize * 2
            )

            if (audioRecord?.state != AudioRecord.STATE_INITIALIZED) {
                onError("Failed to initialize AudioRecord")
                return
            }

            // No state initialization needed for simple compression

            // Start recording
            audioRecord?.startRecording()
            isRecording = true

            Log.d(TAG, "Audio recording started - ADPCM compression mode")
            Log.d(TAG, "Sample rate: $SAMPLE_RATE Hz")
            Log.d(TAG, "Frame size: $FRAME_SIZE samples (6.25ms)")
            Log.d(TAG, "Compressed frame size: $COMPRESSED_FRAME_SIZE bytes (raw PCM)")

            // Prime the audio buffer to avoid initial cut-off
            Log.d(TAG, "Priming audio buffer...")
            val primeBuffer = ShortArray(FRAME_SIZE)
            var primeAttempts = 0
            while (primeAttempts < 10) { // Try up to 10 times
                val readSize = audioRecord?.read(primeBuffer, 0, primeBuffer.size) ?: 0
                if (readSize > 0) {
                    Log.d(TAG, "Audio buffer primed successfully after $primeAttempts attempts")
                    break
                }
                primeAttempts++
                Thread.sleep(10) // Wait 10ms between attempts
            }

            // Start processing in background
            recordingJob = CoroutineScope(Dispatchers.IO).launch {
                processAudioCapture()
            }

        } catch (e: Exception) {
            Log.e(TAG, "Error starting recording", e)
            onError("Failed to start recording: ${e.message}")
        }
    }

    fun stopRecording() {
        if (!isRecording) {
            Log.w(TAG, "Recording not in progress")
            return
        }

        try {
            isRecording = false
            recordingJob?.cancel()
            recordingJob = null

            audioRecord?.stop()
            audioRecord?.release()
            audioRecord = null
            
            Log.d(TAG, "Audio recording stopped - microphone disabled")
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping audio recording", e)
        }
    }

    private suspend fun processAudioCapture() {
        Log.d(TAG, "=== REAL AUDIO CAPTURE MODE STARTED ===")
        Log.d(TAG, "Capturing real microphone audio")
        
        while (isRecording) {
            try {
                // Read real audio from microphone
                val readSize = audioRecord?.read(audioBuffer, 0, FRAME_SIZE) ?: 0
                
                if (readSize > 0) {
                    // Compress the audio data
                    val compressedSize = compressSimple(audioBuffer, readSize, compressedBuffer)
                    
                    if (compressedSize > 0) {
                        // Send real audio data in 32-byte chunks for nRF compatibility
                        val chunkSize = 32
                        var offset = 0
                        
                        while (offset < compressedSize) {
                            val currentChunkSize = minOf(chunkSize, compressedSize - offset)
                            val chunk = compressedBuffer.sliceArray(offset until offset + currentChunkSize)
                            
                            try {
                                Log.d(TAG, "=== REAL AUDIO CHUNK SENT ===")
                                Log.d(TAG, "Chunk size: $currentChunkSize bytes (offset: $offset)")
                                onAudioData(chunk, currentChunkSize)
                            } catch (e: Exception) {
                                Log.e(TAG, "Error sending audio chunk", e)
                            }
                            
                            offset += chunkSize
                            
                            // Small delay between chunks to prevent overwhelming
                            kotlinx.coroutines.delay(5)
                        }
                    }
                }
                
                // Small delay to prevent overwhelming the system
                kotlinx.coroutines.delay(10)
                
            } catch (e: Exception) {
                Log.e(TAG, "Error in audio capture loop", e)
                break
            }
        }
        
        Log.d(TAG, "Real audio capture stopped")
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



    // Play back received audio data with sequence number handling
    fun playAudioData(data: ByteArray, size: Int) {
        if (audioTrack == null) {
            Log.w(TAG, "AudioTrack not initialized, initializing now")
            if (!initializePlayback()) {
                Log.e(TAG, "Failed to initialize playback for received audio")
                return
            }
        }

        try {
            // Validate data size (expecting 2-byte sequence number + audio data)
            if (size <= 2 || size > data.size) {
                Log.w(TAG, "Invalid audio data size: $size (expected > 2 bytes)")
                return
            }
            
            // Extract sequence number from packet header
            val sequenceNumber = (data[1].toInt() shl 8) or (data[0].toInt() and 0xFF)
            val audioDataSize = size - 2
            val audioData = data.sliceArray(2 until size)
            
            // Check for packet loss and buffer priming
            if (lastReceivedSequence > 0) {
                val expectedSequence = (lastReceivedSequence + 1) and 0xFFFF
                if (sequenceNumber != expectedSequence) {
                    consecutivePacketLoss++
                    if (consecutivePacketLoss >= 5) {
                        Log.w(TAG, "Packet loss detected: Expected $expectedSequence, Got $sequenceNumber (Loss: $consecutivePacketLoss)")
                    }
                } else {
                    consecutivePacketLoss = 0
                }
            }
            lastReceivedSequence = sequenceNumber
            
            // Buffer priming: Wait for initial packets to fill buffer
            if (!bufferPrimed && streamBufferSize < 2048) {
                Log.d(TAG, "Buffer priming: ${streamBufferSize} bytes collected")
            } else if (!bufferPrimed && streamBufferSize >= 2048) {
                bufferPrimed = true
                Log.d(TAG, "Buffer primed - optimal processing enabled")
            }
            
            synchronized(streamLock) {
                // Add audio data to stream buffer (skip sequence number)
                for (i in 0 until audioDataSize) {
                    streamBuffer[streamBufferIndex] = audioData[i]
                    streamBufferIndex = (streamBufferIndex + 1) % streamBuffer.size
                    streamBufferSize = minOf(streamBufferSize + 1, streamBuffer.size)
                }
                
                // Start stream playback if we have enough data and haven't started yet
                if (!isStreamStarted && streamBufferSize >= PREBUFFER_BYTES) {
                    isStreamStarted = true
                    lastPacketTime = System.currentTimeMillis()
                    Log.d(TAG, "Starting synchronized stream playback with ${streamBufferSize} bytes, sequence: $sequenceNumber")
                    
                    // Start background stream processing
                    CoroutineScope(Dispatchers.IO).launch {
                        processSynchronizedStream()
                    }
                }
            }
            
        } catch (e: Exception) {
            Log.e(TAG, "Error buffering audio data", e)
        }
    }
    
    // Background thread for synchronized stream processing (inspired by ESP32 I2S WiFi Radio)
    private suspend fun processSynchronizedStream() {
        val playbackData = ByteArray(200) // 200-byte chunks (100 samples)
        var lastPlayTime = 0L
        val targetInterval = 6L // 6.25ms intervals
        var consecutiveEmptyReads = 0
        val maxEmptyReads = 80 // Stop if no data for 500ms
        
        while (isStreamStarted) {
            try {
                val currentTime = System.currentTimeMillis()
                var dataProcessed = false
                
                synchronized(streamLock) {
                    if (streamBufferSize >= 200) {
                        // Get data from stream buffer with proper circular buffer handling
                        val startIndex = (streamBufferIndex - streamBufferSize + streamBuffer.size) % streamBuffer.size
                        for (i in 0 until 200) {
                            val bufferIndex = (startIndex + i) % streamBuffer.size
                            playbackData[i] = streamBuffer[bufferIndex]
                        }
                        streamBufferSize -= 200
                        
                        // Process the audio data - it's already raw 16-bit PCM, no decompression needed
                        // ESP32 B sends raw 16-bit PCM data directly
                        val pcmBytes = ByteArray(200) // 200 bytes = 100 samples * 2 bytes
                        System.arraycopy(playbackData, 0, pcmBytes, 0, 200)
                        
                        // Play the audio with blocking write for better synchronization
                        val written = audioTrack?.write(pcmBytes, 0, pcmBytes.size, AudioTrack.WRITE_BLOCKING) ?: 0
                        if (written > 0) {
                            Log.v(TAG, "Synchronized stream: $written bytes, buffer: ${streamBufferSize} bytes")
                            dataProcessed = true
                            consecutiveEmptyReads = 0
                        }
                        
                        lastPlayTime = currentTime
                    } else {
                        consecutiveEmptyReads++
                    }
                }
                
                // Stop if no data for too long
                if (consecutiveEmptyReads > maxEmptyReads) {
                    Log.w(TAG, "No audio data for ${maxEmptyReads * 10}ms, stopping stream")
                    break
                }
                
                // Maintain consistent timing with adaptive delays
                val elapsed = currentTime - lastPlayTime
                val delayTime = if (dataProcessed) {
                    maxOf(0L, targetInterval - elapsed)
                } else {
                    // Shorter delay when no data to reduce latency
                    maxOf(1L, targetInterval / 2 - elapsed)
                }
                
                if (delayTime > 0) {
                    kotlinx.coroutines.delay(delayTime)
                }
                
            } catch (e: Exception) {
                Log.e(TAG, "Error in synchronized stream", e)
                break
            }
        }
        
        isStreamStarted = false
        Log.d(TAG, "Synchronized stream stopped")
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
    


    private fun initializePlayback(): Boolean {
        try {
            val minBufferSize = AudioTrack.getMinBufferSize(
                SAMPLE_RATE,
                AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT
            )

            if (minBufferSize == AudioRecord.ERROR_BAD_VALUE || minBufferSize == AudioRecord.ERROR) {
                Log.e(TAG, "Invalid audio configuration for playback")
                return false
            }

            // Release existing AudioTrack if any
            audioTrack?.stop()
            audioTrack?.release()
            audioTrack = null

            // Try different AudioTrack configurations
            audioTrack = AudioTrack.Builder()
                .setAudioAttributes(android.media.AudioAttributes.Builder()
                    .setUsage(android.media.AudioAttributes.USAGE_MEDIA)
                    .setContentType(android.media.AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build())
                .setAudioFormat(android.media.AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build())
                .setBufferSizeInBytes(minBufferSize * 10) // Large buffer for stability
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()

            if (audioTrack?.state != AudioTrack.STATE_INITIALIZED) {
                Log.e(TAG, "Failed to initialize AudioTrack")
                return false
            }

            // Start playback immediately
            audioTrack?.play()
            Log.d(TAG, "AudioTrack initialized and started for playback")
            Log.d(TAG, "AudioTrack state: ${audioTrack?.state}, play state: ${audioTrack?.playState}")
            Log.d(TAG, "Buffer size: ${audioTrack?.bufferSizeInFrames} frames")
            
            // Delay a bit to ensure AudioTrack is ready
            Thread.sleep(50)
            
            return true

        } catch (e: Exception) {
            Log.e(TAG, "Error initializing AudioTrack", e)
            return false
        }
    }

    fun playReceivedAudio(data: ByteArray, size: Int) {
        // This is the final, simplified playback logic.
        // It relies on a large AudioTrack hardware buffer to handle jitter,
        // which is the standard and most robust method for audio streaming.
        try {
            // Ensure AudioTrack is initialized and ready.
            if (audioTrack == null || audioTrack?.state != AudioTrack.STATE_INITIALIZED) {
                if (!initializePlayback()) {
                    onError("Failed to initialize AudioTrack for playback")
                    return
                }
            }

            // Ensure the track is playing.
            if (audioTrack?.playState != AudioTrack.PLAYSTATE_PLAYING) {
                audioTrack?.play()
            }

            // Write the incoming audio data directly to the AudioTrack's buffer.
            // The audio hardware will pull from this buffer at the correct, stable rate.
            if (size > 0) {
                val written = audioTrack?.write(data, 0, size) ?: 0
                if (written < size) {
                    Log.w(TAG, "AudioTrack buffer might be full. Wrote $written of $size bytes.")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error playing received audio", e)
        }
    }
    
    // Play accumulated audio from buffer
    private fun playAccumulatedAudio() {
        try {
            if (chunkedAudioBufferSize > 0) {
                Log.d(TAG, "Playing accumulated audio: $chunkedAudioBufferSize bytes")
                
                // Ensure AudioTrack is initialized
                if (audioTrack == null) {
                    if (!initializePlayback()) {
                        Log.e(TAG, "Failed to initialize AudioTrack for accumulated audio")
                        return
                    }
                }
                
                // Ensure AudioTrack is playing
                if (audioTrack?.playState != AudioTrack.PLAYSTATE_PLAYING) {
                    audioTrack?.play()
                }
                
                // Instead of direct write, push into stream buffer
                val even = if (chunkedAudioBufferSize % 2 == 0) chunkedAudioBufferSize else chunkedAudioBufferSize - 1
                synchronized(streamLock) {
                    for (i in 0 until even) {
                        streamBuffer[streamBufferIndex] = chunkedAudioBuffer[i]
                        streamBufferIndex = (streamBufferIndex + 1) % streamBuffer.size
                        streamBufferSize = minOf(streamBufferSize + 1, streamBuffer.size)
                    }
                    if (!isStreamStarted && streamBufferSize >= PREBUFFER_BYTES) {
                        isStreamStarted = true
                        lastPacketTime = System.currentTimeMillis()
                        kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.Dispatchers.IO).launch {
                            processSynchronizedStream()
                        }
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error playing accumulated audio", e)
        }
    }
    
    // Validate received test pattern (audio format)
    private fun validateTestPattern(data: ByteArray, size: Int): Boolean {
        if (size != 32) {
            Log.e(TAG, "Invalid pattern size: $size (expected 32)")
            return false
        }
        
        // Check start markers (16-bit value: 0x55AA in little-endian)
        if (data[0] != 0xAA.toByte() || data[1] != 0x55.toByte()) {
            Log.e(TAG, "Invalid start markers: 0x${data[0].toString(16).uppercase()}, 0x${data[1].toString(16).uppercase()} (expected 0xAA, 0x55)")
            return false
        }
        
        // Check counter high byte
        if (data[3] != 0x00.toByte()) {
            Log.e(TAG, "Invalid counter high byte: 0x${data[3].toString(16).uppercase()} (expected 0x00)")
            return false
        }
        
        // Check that we have valid 16-bit PCM data (non-zero samples)
        var hasNonZeroSamples = false
        for (i in 4 until minOf(20, size) step 2) {
            val sample = (data[i + 1].toInt() shl 8) or (data[i].toInt() and 0xFF)
            if (sample != 0) {
                hasNonZeroSamples = true
                break
            }
        }
        
        if (!hasNonZeroSamples) {
            Log.e(TAG, "No non-zero audio samples found in test pattern")
            return false
        }
        
        // Log the counter value
        val counter = data[2].toInt() and 0xFF
        Log.d(TAG, "Test pattern validation successful - Counter: $counter")
        Log.d(TAG, "Audio format validation passed - 32-byte 2kHz tone detected!")
        
        return true
    }

    private fun generateTestTone() {
        try {
            // Generate a 1-second 440Hz sine wave test tone
            val sampleRate = SAMPLE_RATE
            val duration = 1.0 // 1 second
            val frequency = 440.0 // 440 Hz (A note)
            val numSamples = (sampleRate * duration).toInt()
            
            val testTone = ByteArray(numSamples * 2) // 16-bit = 2 bytes per sample
            var sampleIndex = 0
            
            for (i in 0 until numSamples) {
                val sample = (Math.sin(2.0 * Math.PI * frequency * i / sampleRate) * 16384.0).toInt().toShort()
                testTone[sampleIndex++] = (sample.toInt() and 0xFF).toByte() // Low byte
                testTone[sampleIndex++] = (sample.toInt() shr 8).toByte()   // High byte
            }
            
            val written = audioTrack?.write(testTone, 0, testTone.size, AudioTrack.WRITE_BLOCKING) ?: 0
            Log.d(TAG, "=== TEST TONE GENERATED: $written bytes ===")
            Log.d(TAG, "You should hear a 1-second 440Hz tone")
            
        } catch (e: Exception) {
            Log.e(TAG, "Error generating test tone", e)
        }
    }

    fun playTestTone() {
        try {
            Log.d(TAG, "=== PLAY TEST TONE CALLED ===")
            
            // Ensure AudioTrack is initialized
            if (audioTrack == null) {
                Log.w(TAG, "AudioTrack not initialized, initializing now")
                if (!initializePlayback()) {
                    Log.e(TAG, "Failed to initialize AudioTrack for test tone")
                    return
                }
            }

            // Check AudioTrack state
            val trackState = audioTrack?.state
            Log.d(TAG, "AudioTrack state for test tone: $trackState")
            
            if (trackState != AudioTrack.STATE_INITIALIZED) {
                Log.e(TAG, "AudioTrack not in INITIALIZED state: $trackState")
                return
            }

            // Ensure AudioTrack is playing
            if (audioTrack?.playState != AudioTrack.PLAYSTATE_PLAYING) {
                Log.w(TAG, "AudioTrack not playing, starting playback")
                audioTrack?.play()
            }

            // Wait a bit for AudioTrack to be ready
            Thread.sleep(50)
            
            generateTestTone()
        } catch (e: Exception) {
            Log.e(TAG, "Error playing test tone", e)
            e.printStackTrace()
        }
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
