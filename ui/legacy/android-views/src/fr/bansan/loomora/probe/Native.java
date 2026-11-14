package fr.bansan.loomora.probe;

/**
 * The C API of the backend (contracts/c-api.md) as seen from Java: the implementation is the JNI
 * glue bridge/jni/loomora_jni.cpp, which only converts between Java strings and UTF-8. The UI
 * never builds a greeting itself.
 */
public final class Native {
    /** True once libloomora_jni.so is loaded; false (with the reason) when it cannot be. */
    public static final boolean LOADED;
    public static final String LOAD_ERROR;

    static {
        boolean loaded = false;
        String error = null;
        try {
            System.loadLibrary("loomora_jni");
            loaded = true;
        } catch (UnsatisfiedLinkError e) {
            error = e.getMessage();
        }
        LOADED = loaded;
        LOAD_ERROR = error;
    }

    private Native() {
    }

    /** LOOMORA_ABI_VERSION of the backend. */
    public static native int abiVersion();

    /** "Bonjour " + input, or null when the backend refuses the input. */
    public static native String greet(String input);

    /** 1 when the system version is end of life (family: LOOMORA_FAMILY_*), 0 otherwise. */
    public static native int isEol(int family, int major, int minor);

    public static native String eolWarning();

    public static native String buildCompiler();

    /** The probe JSON line (ASCII), or null when the backend cannot build it. */
    public static native String probeRecord(String platformId, int uiVariant, int coreKind,
            String input, String displayedText, int displayed, int displayMs, long rssKb,
            int eolWarningShown, String disabledFeatures, String timestampUtc);
}
