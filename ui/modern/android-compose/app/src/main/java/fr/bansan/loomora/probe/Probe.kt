package fr.bansan.loomora.probe

import android.content.Intent
import android.os.Build
import android.util.Log
import java.io.File
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/**
 * Probe mode of the Android UI (contracts/ui-contract.md): intent extras "probe" (boolean) and
 * "probe_input" (text that replaces "Coucou") or "probe_input_file" (path of a file holding that
 * text as UTF-8: the `am` command of Android 4.1 and older crashes on a character outside the BMP
 * and adb refuses a command line of 4,096 bytes, so tools/run_vectors.py uses the file for those
 * texts and the plain extra for the others; same contract as the Legacy UI). It only reads the
 * system, measures and logs: the JSON line itself comes from the backend (Native.probeRecord).
 *
 * The line goes to logcat with the tag LOOMORA_PROBE. A logcat entry holds about 4,000 bytes and
 * the line of a long input is longer, so a line of more than CHUNK characters is logged as
 * numbered parts "#i/n " + text (ASCII, in order) that tools/run_vectors.py joins; a short line
 * is logged as it is.
 */
internal object Probe {
    const val TAG = "LOOMORA_PROBE"
    const val EXTRA_PROBE = "probe"
    const val EXTRA_INPUT = "probe_input"
    const val EXTRA_INPUT_FILE = "probe_input_file"
    const val EXIT_DELAY_MS = 2000L
    const val CHUNK = 3000
    private const val UI_MODERN = 2 // LOOMORA_UI_MODERN
    private const val CORE_NATIVE = 1 // LOOMORA_CORE_NATIVE

    fun enabled(intent: Intent?): Boolean = intent != null && intent.getBooleanExtra(EXTRA_PROBE, false)

    /** The text of probe_input_file or of probe_input, or null when neither is given. */
    fun inputOverride(intent: Intent?): String? {
        if (intent == null) return null
        val path = intent.getStringExtra(EXTRA_INPUT_FILE)
        if (path != null) {
            return try {
                String(File(path).readBytes(), Charsets.UTF_8)
            } catch (e: IOException) {
                Log.e(TAG, "cannot read $path: ${e.message}")
                null
            }
        }
        return intent.getStringExtra(EXTRA_INPUT)
    }

    /** "android-RELEASE-ABI", for example android-7.0-x86_64 (^[a-z0-9][a-z0-9._-]*$). */
    fun platformId(): String {
        val abi = Build.SUPPORTED_ABIS.firstOrNull() ?: ""
        val id = "android-" + Build.VERSION.RELEASE + (if (abi.isEmpty()) "" else "-$abi")
        return id.map { c ->
            val l = c.lowercaseChar()
            if ((l in 'a'..'z') || (l in '0'..'9') || l == '.' || l == '_' || l == '-') l else '-'
        }.joinToString("")
    }

    /** Major and minor of Build.VERSION.RELEASE; -1 -1 when it does not start with digits. */
    fun androidVersion(): IntArray {
        val s = Build.VERSION.RELEASE ?: ""
        val v = intArrayOf(-1, -1)
        var part = 0
        var i = 0
        while (part < 2 && i < s.length && s[i].isDigit()) {
            var n = 0
            while (i < s.length && s[i].isDigit()) {
                n = n * 10 + (s[i] - '0')
                i++
            }
            v[part++] = n
            if (i < s.length && s[i] == '.') i++ else break
        }
        if (v[0] >= 0 && v[1] < 0) v[1] = 0
        return v
    }

    /** VmHWM of /proc/self/status in kB, or -1. */
    fun peakRssKb(): Long {
        try {
            File("/proc/self/status").bufferedReader().useLines { lines ->
                for (line in lines) {
                    if (line.startsWith("VmHWM:")) {
                        val digits = line.substring(6).trim().takeWhile { it.isDigit() }
                        return if (digits.isEmpty()) -1 else digits.toLong()
                    }
                }
            }
        } catch (e: IOException) {
            return -1
        } catch (e: NumberFormatException) {
            return -1
        }
        return -1
    }

    fun timestampUtc(): String {
        val f = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US)
        f.timeZone = TimeZone.getTimeZone("UTC")
        return f.format(Date())
    }

    /** Builds the line with the backend and logs it; false when the backend cannot build it. */
    fun emit(
        input: String,
        message: String,
        displayed: Boolean,
        displayMs: Int,
        eolWarningShown: Boolean,
        disabledFeatures: String,
    ): Boolean {
        val line = Native.probeRecord(
            platformId(), UI_MODERN, CORE_NATIVE, input, message, if (displayed) 1 else 0, displayMs,
            peakRssKb(), if (eolWarningShown) 1 else 0, disabledFeatures, timestampUtc(),
        )
        if (line == null) {
            Log.e(TAG, "the backend could not build the probe record")
            return false
        }
        if (line.length <= CHUNK) {
            Log.i(TAG, line)
            return true
        }
        val parts = (line.length + CHUNK - 1) / CHUNK
        for (i in 0 until parts) {
            val from = i * CHUNK
            val to = minOf(line.length, from + CHUNK)
            Log.i(TAG, "#${i + 1}/$parts ${line.substring(from, to)}")
        }
        return true
    }
}
