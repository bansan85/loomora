package fr.bansan.loomora.probe;

import android.content.Intent;
import android.os.Build;
import android.util.Log;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.FileReader;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;

/**
 * Probe mode of the Android UI (contracts/ui-contract.md): intent extras "probe" (boolean) and
 * "probe_input" (text that replaces "Coucou") or "probe_input_file" (path of a file holding that
 * text as UTF-8: the `am` command of Android 4.1 and older crashes on a character outside the BMP
 * and adb refuses a command line of 4,096 bytes, so tools/run_vectors.py uses the file for those
 * texts and the plain extra for the others). It only reads the system, measures and logs: the
 * JSON line itself comes from the backend (Native.probeRecord).
 *
 * The line goes to logcat with the tag LOOMORA_PROBE. A logcat entry holds about 4,000 bytes and
 * the line of a long input is longer, so a line of more than CHUNK characters is logged as
 * numbered parts "#i/n " + text (ASCII, in order) that tools/run_vectors.py joins; a short line
 * is logged as it is.
 */
final class Probe {
    static final String TAG = "LOOMORA_PROBE";
    static final String EXTRA_PROBE = "probe";
    static final String EXTRA_INPUT = "probe_input";
    static final String EXTRA_INPUT_FILE = "probe_input_file";
    static final long EXIT_DELAY_MS = 2000L;
    static final int CHUNK = 3000;

    private Probe() {
    }

    static boolean enabled(Intent intent) {
        return intent != null && intent.getBooleanExtra(EXTRA_PROBE, false);
    }

    /** The text of probe_input_file or of probe_input, or null when neither is given. */
    static String inputOverride(Intent intent) {
        if (intent == null) {
            return null;
        }
        String path = intent.getStringExtra(EXTRA_INPUT_FILE);
        if (path != null) {
            FileInputStream in = null;
            try {
                in = new FileInputStream(path);
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                byte[] buffer = new byte[4096];
                int n;
                while ((n = in.read(buffer)) > 0) {
                    bytes.write(buffer, 0, n);
                }
                return new String(bytes.toByteArray(), "UTF-8");
            } catch (IOException e) {
                Log.e(TAG, "cannot read " + path + ": " + e.getMessage());
                return null;
            } finally {
                if (in != null) {
                    try {
                        in.close();
                    } catch (IOException e) {
                        // nothing to do
                    }
                }
            }
        }
        return intent.getStringExtra(EXTRA_INPUT);
    }

    /** "android-RELEASE-ABI", for example android-4.1.2-x86 (^[a-z0-9][a-z0-9._-]*$). */
    static String platformId() {
        String abi = Build.CPU_ABI == null ? "" : Build.CPU_ABI;
        String id = "android-" + Build.VERSION.RELEASE + (abi.length() == 0 ? "" : "-" + abi);
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < id.length(); i++) {
            char c = Character.toLowerCase(id.charAt(i));
            boolean ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_'
                    || c == '-';
            out.append(ok ? c : '-');
        }
        return out.toString();
    }

    /** Major and minor of Build.VERSION.RELEASE; -1 -1 when it does not start with digits. */
    static int[] androidVersion() {
        int[] v = new int[] {-1, -1};
        String s = Build.VERSION.RELEASE == null ? "" : Build.VERSION.RELEASE;
        int part = 0;
        int i = 0;
        while (part < 2 && i < s.length() && Character.isDigit(s.charAt(i))) {
            int n = 0;
            while (i < s.length() && Character.isDigit(s.charAt(i))) {
                n = n * 10 + (s.charAt(i) - '0');
                i++;
            }
            v[part++] = n;
            if (i < s.length() && s.charAt(i) == '.') {
                i++;
            } else {
                break;
            }
        }
        if (v[0] >= 0 && v[1] < 0) {
            v[1] = 0;
        }
        return v;
    }

    /** VmHWM of /proc/self/status in kB, or -1. */
    static long peakRssKb() {
        BufferedReader r = null;
        try {
            r = new BufferedReader(new FileReader("/proc/self/status"));
            String line;
            while ((line = r.readLine()) != null) {
                if (line.startsWith("VmHWM:")) {
                    String digits = line.substring(6).trim();
                    int end = 0;
                    while (end < digits.length() && Character.isDigit(digits.charAt(end))) {
                        end++;
                    }
                    return end == 0 ? -1 : Long.parseLong(digits.substring(0, end));
                }
            }
        } catch (IOException e) {
            return -1;
        } catch (NumberFormatException e) {
            return -1;
        } finally {
            if (r != null) {
                try {
                    r.close();
                } catch (IOException e) {
                    // nothing to do
                }
            }
        }
        return -1;
    }

    static String timestampUtc() {
        SimpleDateFormat f = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US);
        f.setTimeZone(TimeZone.getTimeZone("UTC"));
        return f.format(new Date());
    }

    /** Builds the line with the backend and logs it; false when the backend cannot build it. */
    static boolean emit(String input, String message, boolean displayed, int displayMs,
            boolean eolWarningShown, String disabledFeatures) {
        String line = Native.probeRecord(platformId(), 1 /* LOOMORA_UI_LEGACY */,
                1 /* LOOMORA_CORE_NATIVE */, input, message, displayed ? 1 : 0, displayMs,
                peakRssKb(), eolWarningShown ? 1 : 0, disabledFeatures, timestampUtc());
        if (line == null) {
            Log.e(TAG, "the backend could not build the probe record");
            return false;
        }
        if (line.length() <= CHUNK) {
            Log.i(TAG, line);
            return true;
        }
        int parts = (line.length() + CHUNK - 1) / CHUNK;
        for (int i = 0; i < parts; i++) {
            int from = i * CHUNK;
            int to = Math.min(line.length(), from + CHUNK);
            Log.i(TAG, "#" + (i + 1) + "/" + parts + " " + line.substring(from, to));
        }
        return true;
    }
}
