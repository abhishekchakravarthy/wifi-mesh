package com.example.bleaudio

import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Opus frame format implementation for WiFi mesh communication.
 * 
 * Frame format: 'W','M', type=1 (Opus), seq(le16), len(le16), payload (Opus bytes)
 * - 'W','M': Magic bytes (2 bytes)
 * - type: Frame type, 1 for Opus (1 byte)
 * - seq: Sequence number, little-endian (2 bytes)
 * - len: Payload length, little-endian (2 bytes)
 * - payload: Opus encoded audio data (variable length)
 */
object OpusFrameFormat {
    private const val TAG = "OpusFrameFormat"
    
    // Frame format constants
    private const val MAGIC_W = 'W'.code.toByte()
    private const val MAGIC_M = 'M'.code.toByte()
    private const val TYPE_OPUS = 1.toByte()
    private const val HEADER_SIZE = 7 // 'W','M',type,seq(2),len(2)
    private const val MAX_PAYLOAD_SIZE = 4000 // Maximum Opus packet size
    
    private var sequenceNumber = 0
    
    /**
     * Create a WM frame with Opus payload.
     * 
     * @param opusData Encoded Opus audio data
     * @return Complete WM frame with header and payload
     */
    fun createFrame(opusData: ByteArray): ByteArray {
        if (opusData.size > MAX_PAYLOAD_SIZE) {
            Log.w(TAG, "Opus payload too large: ${opusData.size} bytes, max: $MAX_PAYLOAD_SIZE")
        }
        
        val frameSize = HEADER_SIZE + opusData.size
        val frame = ByteArray(frameSize)
        val buffer = ByteBuffer.wrap(frame).order(ByteOrder.LITTLE_ENDIAN)
        
        // Write header
        buffer.put(MAGIC_W)
        buffer.put(MAGIC_M)
        buffer.put(TYPE_OPUS)
        buffer.putShort(sequenceNumber.toShort())
        buffer.putShort(opusData.size.toShort())
        
        // Write payload
        buffer.put(opusData)
        
        sequenceNumber = (sequenceNumber + 1) and 0xFFFF // Wrap at 16 bits
        
        Log.v(TAG, "Created WM frame: seq=$sequenceNumber, payload=${opusData.size} bytes")
        return frame
    }
    
    /**
     * Parse a WM frame and extract Opus payload.
     * 
     * @param frameData Complete WM frame data
     * @return OpusFrameData if parsing successful, null otherwise
     */
    fun parseFrame(frameData: ByteArray): OpusFrameData? {
        if (frameData.size < HEADER_SIZE) {
            Log.w(TAG, "Frame too small: ${frameData.size} bytes, minimum: $HEADER_SIZE")
            return null
        }
        
        val buffer = ByteBuffer.wrap(frameData).order(ByteOrder.LITTLE_ENDIAN)
        
        // Check magic bytes
        val magicW = buffer.get()
        val magicM = buffer.get()
        if (magicW != MAGIC_W || magicM != MAGIC_M) {
            Log.w(TAG, "Invalid magic bytes: 0x${magicW.toString(16)}, 0x${magicM.toString(16)}")
            return null
        }
        
        // Read header
        val type = buffer.get()
        val seq = buffer.short.toInt() and 0xFFFF
        val len = buffer.short.toInt() and 0xFFFF
        
        // Validate type
        if (type != TYPE_OPUS) {
            Log.w(TAG, "Unsupported frame type: $type")
            return null
        }
        
        // Validate payload length
        if (len > MAX_PAYLOAD_SIZE) {
            Log.w(TAG, "Payload too large: $len bytes, max: $MAX_PAYLOAD_SIZE")
            return null
        }
        
        if (frameData.size < HEADER_SIZE + len) {
            Log.w(TAG, "Frame truncated: ${frameData.size} bytes, expected: ${HEADER_SIZE + len}")
            return null
        }
        
        // Extract payload
        val payload = ByteArray(len)
        buffer.get(payload)
        
        Log.v(TAG, "Parsed WM frame: seq=$seq, payload=$len bytes")
        return OpusFrameData(seq, payload)
    }
    
    /**
     * Check if data starts with WM frame magic bytes.
     * 
     * @param data Data to check
     * @return true if data starts with 'W','M'
     */
    fun isWmFrame(data: ByteArray): Boolean {
        return data.size >= 2 && data[0] == MAGIC_W && data[1] == MAGIC_M
    }
    
    /**
     * Get the expected header size.
     */
    fun getHeaderSize(): Int = HEADER_SIZE
    
    /**
     * Get the maximum payload size.
     */
    fun getMaxPayloadSize(): Int = MAX_PAYLOAD_SIZE
    
    /**
     * Reset sequence number (useful for testing or reconnection).
     */
    fun resetSequenceNumber() {
        sequenceNumber = 0
        Log.d(TAG, "Sequence number reset")
    }
    
    /**
     * Get current sequence number.
     */
    fun getCurrentSequenceNumber(): Int = sequenceNumber
}

/**
 * Data class representing parsed Opus frame data.
 */
data class OpusFrameData(
    val sequenceNumber: Int,
    val opusPayload: ByteArray
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false

        other as OpusFrameData

        if (sequenceNumber != other.sequenceNumber) return false
        if (!opusPayload.contentEquals(other.opusPayload)) return false

        return true
    }

    override fun hashCode(): Int {
        var result = sequenceNumber
        result = 31 * result + opusPayload.contentHashCode()
        return result
    }
}
