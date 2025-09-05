package com.example.bleaudio

/**
 * Implements the u-law encoding and decoding algorithm.
 * This is a standard for telephony and provides better dynamic range than linear 8-bit PCM.
 */
object MuLawCodec {

    private const val BIAS = 0x84 // aka 132, an arbitrary number
    private const val MAX = 32635 // 2^15 - 2^5 - 1, max value for 16-bit audio

    /**
     * Encodes a 16-bit PCM sample into an 8-bit u-law sample.
     */
    private fun encodeSample(pcmSample: Short): Byte {
        var sample = pcmSample.toInt()
        val sign = if (sample < 0) 0x80 else 0x00
        if (sample < 0) sample = -sample
        if (sample > MAX) sample = MAX

        sample += BIAS

        var exponent = 7
        while (exponent > 0 && (sample and 0x8000) == 0) {
            exponent--
            sample = sample shl 1
        }

        val mantissa = (sample shr 8) and 0x0F

        return (sign or (exponent shl 4) or mantissa).inv().toByte()
    }

    /**
     * Decodes an 8-bit u-law sample into a 16-bit PCM sample.
     */
    private fun decodeSample(ulawSample: Byte): Short {
        val ulaw = ulawSample.toInt().inv() and 0xFF

        val sign = ulaw and 0x80
        val exponent = (ulaw and 0x70) shr 4
        val mantissa = ulaw and 0x0F

        val t = ((mantissa shl 3) + BIAS) shl exponent
        
        val pcm = if (sign != 0) {
            BIAS - t
        } else {
            t - BIAS
        }

        return pcm.toShort()
    }

    /**
     * Encodes an array of 16-bit PCM samples into an array of 8-bit u-law samples.
     */
    fun encode(pcm: ShortArray, size: Int): ByteArray {
        val ulaw = ByteArray(size)
        for (i in 0 until size) {
            ulaw[i] = encodeSample(pcm[i])
        }
        return ulaw
    }

    /**
     * Decodes an array of 8-bit u-law samples into an array of 16-bit PCM samples.
     */
    fun decode(ulaw: ByteArray, size: Int): ShortArray {
        val pcm = ShortArray(size)
        for (i in 0 until size) {
            pcm[i] = decodeSample(ulaw[i])
        }
        return pcm
    }
}
