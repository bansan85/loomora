package fr.bansan.loomora.probe

/**
 * The C API of the backend (contracts/c-api.md) as seen from Kotlin: the implementation is the JNI
 * glue bridge/jni/loomora_jni.cpp (the same file as the Legacy UI, which exports
 * Java_fr_bansan_loomora_probe_Native_*), which only converts between Java strings and UTF-8. The UI never
 * builds a greeting itself. The functions are static methods of this class.
 */
object Native {
    /** True once libloomora_jni.so is loaded; false (with the reason in [loadError]) when it cannot be. */
    val loaded: Boolean

    val loadError: String?

    init {
        var ok = false
        var error: String? = null
        try {
            System.loadLibrary("loomora_jni")
            ok = true
        } catch (e: UnsatisfiedLinkError) {
            error = e.message
        }
        loaded = ok
        loadError = error
    }

    /** LOOMORA_ABI_VERSION of the backend. */
    @JvmStatic
    external fun abiVersion(): Int

    /** The greeting of the input, or null when the backend refuses the input. */
    @JvmStatic
    external fun greet(input: String): String?

    /** 1 when the system version is end of life (family: LOOMORA_FAMILY_*), 0 otherwise. */
    @JvmStatic
    external fun isEol(family: Int, major: Int, minor: Int): Int

    @JvmStatic
    external fun eolWarning(): String?

    @JvmStatic
    external fun buildCompiler(): String?

    /** The probe JSON line (ASCII), or null when the backend cannot build it. */
    @JvmStatic
    external fun probeRecord(
        platformId: String,
        uiVariant: Int,
        coreKind: Int,
        input: String,
        displayedText: String,
        displayed: Int,
        displayMs: Int,
        rssKb: Long,
        eolWarningShown: Int,
        disabledFeatures: String,
        timestampUtc: String,
    ): String?
}
