package dev.fdradio

/**
 * The audio engine, as seen from Kotlin.
 *
 * Two kinds of traffic cross this boundary and they go in opposite directions
 * for a reason:
 *
 * - **Commands** (`start`, `stop`) go down, from an ordinary thread. They open
 *   and close streams, which blocks until the audio callback returns, so they
 *   must never be called from audio.
 * - **State** comes up by *polling*. The audio callback never calls into Java.
 *   A JNI call can block on a class load, on the GC, or on the JNI lock, and a
 *   blocked audio callback is an audible click. Inverting the direction removes
 *   the possibility rather than making it unlikely.
 *
 * The cost is that the UI learns about an underrun up to one poll interval
 * late, which for a diagnostics screen is free.
 */
object AudioEngine {

    /**
     * Which capture chain to ask the platform for.
     *
     * Found by measurement, not preference: on a Redmi Note 9 Pro,
     * [VOICE_COMMUNICATION] gets 960-frame bursts and no low-latency mode,
     * while the other two get the fast path. The platform will not give you its
     * echo canceller and its fast capture path at the same time, and M7 has to
     * choose between them — so the choice is exposed rather than hardcoded to
     * whichever happened to work first.
     */
    enum class Capture(val code: Int, val label: String) {
        VOICE_COMMUNICATION(0, "VoiceComm"),
        VOICE_RECOGNITION(1, "VoiceRecog"),
        UNPROCESSED(2, "Unprocessed"),
    }

    private external fun nativeStart(capture: Int): Boolean
    private external fun nativeStop()
    private external fun nativeSnapshot(): LongArray?
    private external fun nativeErrorText(code: Int): String

    fun start(capture: Capture): Boolean =
        if (NativeCore.available) nativeStart(capture.code) else false

    fun stop() {
        if (NativeCore.available) nativeStop()
    }

    /**
     * A point-in-time view of the audio path.
     *
     * Fields are read individually from atomics on the native side, so two
     * fields may disagree by a few microseconds. That is deliberate: the
     * alternative is a lock the audio callback has to take.
     */
    data class Snapshot(
        val running: Boolean,
        val inputSampleRate: Int,
        val inputBurstFrames: Int,
        val inputBufferFrames: Int,
        val inputCapacityFrames: Int,
        val inputXRuns: Long,
        val inputAAudio: Boolean,
        val inputLowLatency: Boolean,
        val outputSampleRate: Int,
        val outputBurstFrames: Int,
        val outputBufferFrames: Int,
        val outputCapacityFrames: Int,
        val outputXRuns: Long,
        val outputAAudio: Boolean,
        val outputLowLatency: Boolean,
        val outputLatencyUs: Long,
        val ringSamples: Long,
        val ringOverflows: Long,
        val ringUnderruns: Long,
        val inputCallbacks: Long,
        val outputCallbacks: Long,
        val worstOutputGapUs: Long,
        val lastError: Int,
        val capture: Capture,
        val primedSamples: Long,
    ) {
        /** How much audio the ring is holding, which is latency we added. */
        val ringMillis: Double
            get() = if (inputSampleRate > 0) ringSamples * 1000.0 / inputSampleRate else 0.0

        /** One burst of output, in milliseconds: the callback's deadline. */
        val outputBurstMillis: Double
            get() = if (outputSampleRate > 0) {
                outputBurstFrames * 1000.0 / outputSampleRate
            } else {
                0.0
            }
    }

    fun snapshot(): Snapshot? {
        if (!NativeCore.available) return null
        val v = nativeSnapshot() ?: return null
        // The field order is the contract with jni_bridge.cpp. Reading it out
        // by name here once, rather than indexing raw longs at the call site,
        // means a change on either side breaks in one place.
        if (v.size < 25) return null
        return Snapshot(
            running = v[0] != 0L,
            inputSampleRate = v[1].toInt(),
            inputBurstFrames = v[2].toInt(),
            inputBufferFrames = v[3].toInt(),
            inputCapacityFrames = v[4].toInt(),
            inputXRuns = v[5],
            inputAAudio = v[6] != 0L,
            inputLowLatency = v[7] != 0L,
            outputSampleRate = v[8].toInt(),
            outputBurstFrames = v[9].toInt(),
            outputBufferFrames = v[10].toInt(),
            outputCapacityFrames = v[11].toInt(),
            outputXRuns = v[12],
            outputAAudio = v[13] != 0L,
            outputLowLatency = v[14] != 0L,
            outputLatencyUs = v[15],
            ringSamples = v[16],
            ringOverflows = v[17],
            ringUnderruns = v[18],
            inputCallbacks = v[19],
            outputCallbacks = v[20],
            worstOutputGapUs = v[21],
            lastError = v[22].toInt(),
            capture = Capture.entries.firstOrNull { it.code == v[23].toInt() }
                ?: Capture.VOICE_COMMUNICATION,
            primedSamples = v[24],
        )
    }

    fun errorText(code: Int): String =
        if (NativeCore.available && code != 0) nativeErrorText(code) else ""
}
