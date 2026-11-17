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

    // ---- the project API (contracts/c-api-project.md), only what the project probe modes of
    // spec 002 need. A project or a report is a handle (a long the UI never interprets); 0 means
    // none. Every handle is released with projectClose / reportFree.

    /** loomora_project_open_path: { code, project handle, report handle }, or null if the glue failed. */
    public static native long[] projectOpen(String path);

    /** loomora_validate_memory: { code, 0, report handle }, or null if the glue failed. */
    public static native long[] validateMemory(byte[] data);

    public static native long reportCount(long report);

    public static native long reportErrorCount(long report);

    public static native void reportFree(long report);

    public static native void projectClose(long project);

    /** loomora_project_is_light: 1 for a light project, 0 for a full one. */
    public static native int projectIsLight(long project);

    /**
     * loomora_project_export_stream of every result into a byte array. Null on failure, with the
     * error code in rc[0] (0 on success). profile: 1 full, 2 light (LOOMORA_PROFILE_*).
     */
    public static native byte[] projectExport(long project, int profile, String modifiedAt, int[] rc);

    /** The JSON of an item (kind: LOOMORA_KIND_*), or null. */
    public static native byte[] projectItemJson(long project, int kind, int id);

    /** The whole content of an entry, or null. */
    public static native byte[] projectReadEntry(long project, String entry);
}
