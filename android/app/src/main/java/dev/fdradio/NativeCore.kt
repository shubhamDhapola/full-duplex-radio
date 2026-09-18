package dev.fdradio

/**
 * The JNI surface of the project core.
 *
 * Everything native lives behind this object so there is exactly one place that
 * loads the library and one place to look when a symbol fails to resolve.
 *
 * Nothing here may be called from an audio callback. That rule has no teeth
 * yet -- these two calls run at start-up -- but it is the rule the real
 * boundary is built to, and it is easier to keep than to retrofit: a JNI call
 * can block on a class load, and a blocked audio callback is a glitch.
 */
object NativeCore {

    /** True when libfdradio_jni loaded; false leaves the UI able to say so. */
    val available: Boolean = runCatching { System.loadLibrary("fdradio_jni") }.isSuccess

    /**
     * Runs the core's on-device self-check and returns a human-readable report.
     *
     * Exercises the packet codec, Opus, and the monotonic clock. The same
     * conformance vectors the host runs land here in a later milestone; this is
     * the smallest thing that proves the toolchain produced a working core.
     */
    external fun selfCheck(): String

    /** The ABI this process is running, as the native build sees it. */
    external fun abi(): String
}
