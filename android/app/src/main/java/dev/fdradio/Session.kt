package dev.fdradio

/**
 * The media session: microphone to the network, network to the speaker.
 *
 * Same boundary rules as [AudioEngine] — commands down from an ordinary thread,
 * state up by polling — because the audio callbacks drive this and they may not
 * call into Java.
 */
object Session {

    private external fun nativeStart(
        peerHost: String,
        peerPort: Int,
        localPort: Int,
        bitrate: Int,
        fec: Boolean,
        expectedLoss: Int,
        targetDelayMs: Int,
    ): Boolean

    private external fun nativeStop()
    private external fun nativeSetTransmitting(on: Boolean)
    private external fun nativeSnapshot(): LongArray?

    /**
     * Opens the socket and the codecs. The peer must be a numeric address —
     * [radio::net::Endpoint] refuses hostnames on purpose, because
     * `getaddrinfo` blocks and nothing reachable from the media path may block.
     * Discovery is M3's job and hands over numeric addresses.
     */
    fun start(
        peerHost: String,
        peerPort: Int,
        localPort: Int = 47000,
        bitrate: Int = 32_000,
        fec: Boolean = false,
        expectedLoss: Int = 0,
        targetDelayMs: Int = 60,
    ): Boolean = if (NativeCore.available) {
        nativeStart(peerHost, peerPort, localPort, bitrate, fec, expectedLoss, targetDelayMs)
    } else {
        false
    }

    fun stop() {
        if (NativeCore.available) nativeStop()
    }

    /** Push to talk. Each press opens a new talkspurt, and a new stream id. */
    fun setTransmitting(on: Boolean) {
        if (NativeCore.available) nativeSetTransmitting(on)
    }

    data class Snapshot(
        val running: Boolean,
        val transmitting: Boolean,
        val packetsSent: Long,
        val bytesSent: Long,
        val sendFailed: Long,
        val encodeFailed: Long,
        val talkspurts: Long,
        val datagramsReceived: Long,
        val rejected: Long,
        val rxRingOverflows: Long,
        val fromPacket: Long,
        val fecRecovered: Long,
        val concealed: Long,
        val silence: Long,
        val late: Long,
        val gaps: Long,
        val duplicates: Long,
        val depthFrames: Long,
        val encodeCalls: Long,
        val encodeTotalUs: Long,
        val encodeMaxUs: Long,
        val decodeCalls: Long,
        val decodeTotalUs: Long,
        val decodeMaxUs: Long,
        val txPcmOverflows: Long,
    ) {
        val encodeMeanUs: Double
            get() = if (encodeCalls > 0) encodeTotalUs.toDouble() / encodeCalls else 0.0

        val decodeMeanUs: Double
            get() = if (decodeCalls > 0) decodeTotalUs.toDouble() / decodeCalls else 0.0

        /** Buffer depth in milliseconds, at the 20 ms frames the wire uses. */
        val depthMillis: Double get() = depthFrames * 20.0
    }

    fun snapshot(): Snapshot? {
        if (!NativeCore.available) return null
        val v = nativeSnapshot() ?: return null
        if (v.size < 25) return null
        return Snapshot(
            running = v[0] != 0L,
            transmitting = v[1] != 0L,
            packetsSent = v[2],
            bytesSent = v[3],
            sendFailed = v[4],
            encodeFailed = v[5],
            talkspurts = v[6],
            datagramsReceived = v[7],
            rejected = v[8],
            rxRingOverflows = v[9],
            fromPacket = v[10],
            fecRecovered = v[11],
            concealed = v[12],
            silence = v[13],
            late = v[14],
            gaps = v[15],
            duplicates = v[16],
            depthFrames = v[17],
            encodeCalls = v[18],
            encodeTotalUs = v[19],
            encodeMaxUs = v[20],
            decodeCalls = v[21],
            decodeTotalUs = v[22],
            decodeMaxUs = v[23],
            txPcmOverflows = v[24],
        )
    }
}
